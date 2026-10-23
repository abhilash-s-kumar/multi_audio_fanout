from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
import ctypes
from ctypes import wintypes
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from tkinter import font
from tkinter import messagebox, ttk

sys.coinit_flags = 2

from comtypes import CLSCTX_ALL, CoInitialize, CoUninitialize
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

import numpy as np
import soundcard as sc

from fanout import BLOCK_FRAMES, SAMPLE_RATE, device_name, normalize_block


APP_NAME = "MAF"
MAF_ENDPOINT_MARKER = "MAF Virtual Audio Output"
BASE_BUFFER_BLOCKS = 4
DRIFT_TOLERANCE_BLOCKS = 3
STARTUP_PREBUFFER_BLOCKS = 6
ADAPTIVE_QUEUE_BLOCKS = 36
DRIFT_GAIN = 0.00018
DRIFT_DEADBAND_BLOCKS = 2
DRIFT_SMOOTHING = 0.015
MAX_RESAMPLE_CORRECTION = 0.0008
CORRECTION_HOLD_BLOCKS = 90
SILENCE_RESET_BLOCKS = 30
SILENCE_RMS = 0.0004
VOLUME_STEP = 0.05
DELAY_STEP_MS = 5.0
MIN_DELAY_MS = -1000.0
MAX_DELAY_MS = 2000.0
DEFAULT_OUTPUT_NAME_WIDTH = 42
MIN_OUTPUT_NAME_WIDTH = 24
MAX_OUTPUT_NAME_WIDTH = 64
CHANNEL_MODES = ("Stereo", "Left", "Right")
DEFAULT_CHANNEL_MODE = "Stereo"
WINDOWS_VOLUME_POLL_MS = 150
AUTO_RESET_DRAGGING_REPORTS = 3
AUTO_RESET_COOLDOWN_SECONDS = 20.0
RESET_RESTART_DELAY_MS = 700
NATIVE_LATENCY_MS = 100.0
NATIVE_STOP_TIMEOUT_SECONDS = 2.0
NATIVE_AUTO_RESTART_LIMIT = 3
NATIVE_AUTO_RESTART_WINDOW_SECONDS = 60.0
VISUALIZER_REFRESH_MS = 33
VISUALIZER_DECAY = 0.94
SETTINGS_PATH = Path(os.environ.get("APPDATA", str(Path.home()))) / "MAF" / "settings.json"
AVRT_PRIORITY_NORMAL = 0
AVRT_PRIORITY_HIGH = 1


def app_base_dir() -> Path:
    bundled_dir = getattr(sys, "_MEIPASS", None)
    if bundled_dir:
        return Path(bundled_dir).resolve()
    return Path(__file__).resolve().parent


def native_engine_path() -> Path:
    candidates = [
        app_base_dir() / "native_engine" / "maf_native_engine.exe",
        Path(sys.executable).resolve().parent / "native_engine" / "maf_native_engine.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def is_maf_endpoint(name: str) -> bool:
    return MAF_ENDPOINT_MARKER.casefold() in name.casefold()


def compact_text(value: str, limit: int) -> str:
    if len(value) <= limit:
        return value
    return value[: max(0, limit - 3)] + "..."


@dataclass
class SourceItem:
    index: int
    name: str
    device: object
    is_loopback: bool

    @property
    def label(self) -> str:
        kind = "loopback" if self.is_loopback else "capture"
        if is_maf_endpoint(self.name) and self.is_loopback:
            kind = "MAF source"
        return f"[{self.index}] {self.name} ({kind})"


@dataclass
class OutputItem:
    index: int
    name: str
    device: object | None
    connected: bool = True


@dataclass
class OutputControl:
    item: OutputItem
    enabled: tk.BooleanVar
    volume: tk.DoubleVar
    delay_ms: tk.DoubleVar
    channel_mode: tk.StringVar
    status: tk.StringVar


@dataclass
class DriftStatus:
    queue_blocks: int = 0
    correction: float = 1.0
    dropped: int = 0
    underruns: int = 0
    mode: str = "idle"

    @property
    def label(self) -> str:
        if self.mode != "tracking":
            direction = self.mode
        elif self.correction > 1.0004:
            direction = "dragging"
        elif self.correction < 0.9996:
            direction = "leading"
        else:
            direction = "locked"
        return f"{direction} q={self.queue_blocks}x adj={self.correction:.4f}"


def resample_block(block: np.ndarray, correction: float) -> np.ndarray:
    if block.shape[0] < 2:
        return block

    correction = min(1.0 + MAX_RESAMPLE_CORRECTION, max(1.0 - MAX_RESAMPLE_CORRECTION, correction))
    output_frames = max(2, int(round(block.shape[0] / correction)))
    if output_frames == block.shape[0]:
        return block

    source_x = np.arange(block.shape[0], dtype=np.float32)
    target_x = np.linspace(0, block.shape[0] - 1, output_frames, dtype=np.float32)
    channels = [
        np.interp(target_x, source_x, block[:, channel]).astype(np.float32)
        for channel in range(block.shape[1])
    ]
    return np.column_stack(channels)


def apply_channel_mode(block: np.ndarray, mode: str) -> np.ndarray:
    if block.ndim != 2 or block.shape[1] < 2 or mode == "Stereo":
        return block

    if mode == "Left":
        channel = block[:, 0]
    elif mode == "Right":
        channel = block[:, 1]
    else:
        return block
    return np.column_stack((channel, channel)).astype(np.float32, copy=False)


class MmcssAudioTask:
    def __init__(self, task_name: str = "Audio", priority: int = AVRT_PRIORITY_HIGH) -> None:
        self.task_name = task_name
        self.priority = priority
        self._avrt = None
        self._handle = None

    def __enter__(self) -> bool:
        if os.name != "nt":
            return False
        try:
            self._avrt = ctypes.WinDLL("avrt.dll")
            self._avrt.AvSetMmThreadCharacteristicsW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
            self._avrt.AvSetMmThreadCharacteristicsW.restype = wintypes.HANDLE
            self._avrt.AvSetMmThreadPriority.argtypes = [wintypes.HANDLE, ctypes.c_int]
            self._avrt.AvSetMmThreadPriority.restype = wintypes.BOOL
            self._avrt.AvRevertMmThreadCharacteristics.argtypes = [wintypes.HANDLE]
            self._avrt.AvRevertMmThreadCharacteristics.restype = wintypes.BOOL

            task_index = wintypes.DWORD(0)
            handle = self._avrt.AvSetMmThreadCharacteristicsW(self.task_name, ctypes.byref(task_index))
            if not handle:
                return False
            self._handle = handle
            self._avrt.AvSetMmThreadPriority(handle, self.priority)
            return True
        except Exception:
            self._handle = None
            return False

    def __exit__(self, _exc_type: object, _exc: object, _traceback: object) -> None:
        if self._avrt is not None and self._handle:
            try:
                self._avrt.AvRevertMmThreadCharacteristics(self._handle)
            except Exception:
                pass
        self._handle = None


class AudioSettings:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.master_volume = 1.0
        self.sync_enabled = True
        self.auto_reset_enabled = False
        self.output_name_width = DEFAULT_OUTPUT_NAME_WIDTH
        self.calibration_mic = ""  # Empty: the engine picks the default / built-in microphone.
        self.delay_floor_ms = 0.0
        self.output_volume: dict[str, float] = {}
        self.output_delay_ms: dict[str, float] = {}
        self.output_selected: dict[str, bool] = {}
        self.output_channel_mode: dict[str, str] = {}
        self.known_outputs: set[str] = set()
        self._load()

    def _load(self) -> None:
        try:
            if not SETTINGS_PATH.exists():
                return
            data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            if "sync_enabled" in data:
                self.sync_enabled = bool(data["sync_enabled"])
            if "auto_reset_enabled" in data:
                self.auto_reset_enabled = bool(data["auto_reset_enabled"])
            if "output_name_width" in data:
                self.output_name_width = min(
                    MAX_OUTPUT_NAME_WIDTH,
                    max(MIN_OUTPUT_NAME_WIDTH, int(data["output_name_width"])),
                )
            if isinstance(data.get("calibration_mic"), str):
                self.calibration_mic = data["calibration_mic"]
            outputs = data.get("outputs", {})
            if not isinstance(outputs, dict):
                return

            for name, values in outputs.items():
                if not isinstance(name, str) or not isinstance(values, dict):
                    continue
                self.known_outputs.add(name)
                if "volume" in values:
                    self.output_volume[name] = max(0.0, float(values["volume"]))
                if "delay_ms" in values:
                    self.output_delay_ms[name] = min(MAX_DELAY_MS, max(MIN_DELAY_MS, float(values["delay_ms"])))
                if "selected" in values:
                    self.output_selected[name] = bool(values["selected"])
                if values.get("channel_mode") in CHANNEL_MODES:
                    self.output_channel_mode[name] = str(values["channel_mode"])
        except Exception:
            pass

    def _save(self) -> None:
        try:
            with self._lock:
                names = (
                    set(self.output_volume)
                    | set(self.output_delay_ms)
                    | set(self.output_selected)
                    | set(self.output_channel_mode)
                    | set(self.known_outputs)
                )
                data = {
                    "sync_enabled": self.sync_enabled,
                    "auto_reset_enabled": self.auto_reset_enabled,
                    "output_name_width": self.output_name_width,
                    "calibration_mic": self.calibration_mic,
                    "outputs": {
                        name: {
                            "volume": self.output_volume.get(name, 1.0),
                            "delay_ms": self.output_delay_ms.get(name, 0.0),
                            "selected": self.output_selected.get(name, False),
                            "channel_mode": self.output_channel_mode.get(name, DEFAULT_CHANNEL_MODE),
                        }
                        for name in sorted(names)
                    }
                }
            SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
            temp_path = SETTINGS_PATH.with_suffix(".tmp")
            temp_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
            temp_path.replace(SETTINGS_PATH)
        except Exception:
            pass

    def set_master_volume(self, value: float) -> None:
        with self._lock:
            self.master_volume = max(0.0, float(value))

    def set_output_volume(self, name: str, value: float) -> None:
        with self._lock:
            self.known_outputs.add(name)
            self.output_volume[name] = max(0.0, float(value))
            self._save()

    def set_output_delay_ms(self, name: str, value: float) -> None:
        with self._lock:
            self.known_outputs.add(name)
            self.output_delay_ms[name] = min(MAX_DELAY_MS, max(MIN_DELAY_MS, float(value)))
            self._save()

    def set_output_selected(self, name: str, value: bool) -> None:
        with self._lock:
            self.known_outputs.add(name)
            self.output_selected[name] = bool(value)
            self._save()

    def set_output_channel_mode(self, name: str, value: str) -> None:
        if value not in CHANNEL_MODES:
            value = DEFAULT_CHANNEL_MODE
        with self._lock:
            self.known_outputs.add(name)
            self.output_channel_mode[name] = value
            self._save()

    def remember_output(self, name: str) -> None:
        with self._lock:
            if name not in self.known_outputs:
                self.known_outputs.add(name)
                self._save()

    def forget_output(self, name: str) -> None:
        with self._lock:
            self.known_outputs.discard(name)
            self.output_volume.pop(name, None)
            self.output_delay_ms.pop(name, None)
            self.output_selected.pop(name, None)
            self.output_channel_mode.pop(name, None)
            self._save()

    def set_sync_enabled(self, value: bool) -> None:
        with self._lock:
            self.sync_enabled = bool(value)
            self._save()

    def set_auto_reset_enabled(self, value: bool) -> None:
        with self._lock:
            self.auto_reset_enabled = bool(value)
            self._save()

    def set_output_name_width(self, value: int) -> None:
        with self._lock:
            self.output_name_width = min(MAX_OUTPUT_NAME_WIDTH, max(MIN_OUTPUT_NAME_WIDTH, int(value)))
            self._save()

    def set_delay_floor_ms(self, value: float) -> None:
        with self._lock:
            self.delay_floor_ms = min(0.0, float(value))

    def snapshot(self, name: str) -> tuple[float, int, bool, str]:
        with self._lock:
            gain = self.master_volume * self.output_volume.get(name, 1.0)
            delay_ms = self.output_delay_ms.get(name, 0.0) - self.delay_floor_ms
            sync_enabled = self.sync_enabled
            channel_mode = self.output_channel_mode.get(name, DEFAULT_CHANNEL_MODE)
        return max(0.0, gain), int((max(0.0, delay_ms) / 1000.0) * SAMPLE_RATE), sync_enabled, channel_mode


class WindowsMasterVolume:
    def __init__(self) -> None:
        self.endpoint = None
        self.refresh()

    def refresh(self) -> None:
        devices = AudioUtilities.GetSpeakers()
        if hasattr(devices, "EndpointVolume"):
            self.endpoint = devices.EndpointVolume
            return

        interface = devices.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        self.endpoint = interface.QueryInterface(IAudioEndpointVolume)

    def get(self) -> float:
        if self.endpoint is None:
            self.refresh()
        return float(self.endpoint.GetMasterVolumeLevelScalar())

    def set(self, value: float) -> None:
        if self.endpoint is None:
            self.refresh()
        value = min(1.0, max(0.0, float(value)))
        self.endpoint.SetMasterVolumeLevelScalar(value, None)


class PlaybackWorker(threading.Thread):
    def __init__(
        self,
        control: OutputControl,
        settings: AudioSettings,
        stop_event: threading.Event,
    ) -> None:
        super().__init__(name=f"maf-output-{control.item.index}", daemon=True)
        self.control = control
        self.settings = settings
        self.stop_event = stop_event
        self.frames: queue.Queue[np.ndarray | None] = queue.Queue(maxsize=ADAPTIVE_QUEUE_BLOCKS)
        self.error: BaseException | None = None
        self.dropped = 0
        self.underruns = 0
        self._correction = 1.0
        self._status_lock = threading.Lock()
        self._status = DriftStatus()

    def push(self, block: np.ndarray) -> None:
        try:
            self.frames.put_nowait(block)
        except queue.Full:
            self.dropped += 1
            try:
                self.frames.get_nowait()
            except queue.Empty:
                pass
            try:
                self.frames.put_nowait(block)
            except queue.Full:
                pass

    def close(self) -> None:
        try:
            self.frames.put_nowait(None)
        except queue.Full:
            pass

    def status(self) -> DriftStatus:
        with self._status_lock:
            return DriftStatus(
                queue_blocks=self._status.queue_blocks,
                correction=self._status.correction,
                dropped=self._status.dropped,
                underruns=self._status.underruns,
                mode=self._status.mode,
            )

    def _set_status(self, queue_blocks: int, correction: float, mode: str = "tracking") -> None:
        with self._status_lock:
            self._status.queue_blocks = queue_blocks
            self._status.correction = correction
            self._status.dropped = self.dropped
            self._status.underruns = self.underruns
            self._status.mode = mode

    def _reset_correction(self) -> None:
        self._correction = 1.0

    def run(self) -> None:
        CoInitialize()
        mmcss = MmcssAudioTask()
        mmcss_enabled = mmcss.__enter__()
        if mmcss_enabled:
            self._set_status(0, self._correction, "audio prio")
        delayed_blocks: deque[np.ndarray] = deque()
        buffered_frames = 0
        last_playable: np.ndarray | None = None
        started_audio = False
        hold_blocks = CORRECTION_HOLD_BLOCKS
        silent_blocks = 0
        try:
            with self.control.item.device.player(
                samplerate=SAMPLE_RATE,
                channels=2,
                blocksize=BLOCK_FRAMES,
            ) as player:
                while not self.stop_event.is_set():
                    block = self.frames.get()
                    if block is None:
                        break

                    delayed_blocks.append(block)
                    buffered_frames += len(block)
                    gain, delay_frames, sync_enabled, channel_mode = self.settings.snapshot(self.control.item.name)
                    block_rms = float(np.sqrt(np.mean(np.square(block)))) if block.size else 0.0
                    is_silent = block_rms < SILENCE_RMS

                    if not sync_enabled and self._correction != 1.0:
                        self._reset_correction()

                    if is_silent:
                        silent_blocks += 1
                        if silent_blocks == SILENCE_RESET_BLOCKS:
                            self._reset_correction()
                            hold_blocks = CORRECTION_HOLD_BLOCKS
                            self._set_status(self.frames.qsize(), self._correction, "silent")
                    else:
                        if silent_blocks >= SILENCE_RESET_BLOCKS:
                            self._reset_correction()
                            started_audio = False
                            hold_blocks = CORRECTION_HOLD_BLOCKS
                        silent_blocks = 0

                    if not started_audio:
                        queue_blocks = len(delayed_blocks) + self.frames.qsize()
                        if len(delayed_blocks) < STARTUP_PREBUFFER_BLOCKS:
                            self._set_status(queue_blocks, self._correction, "warming")
                            continue
                        started_audio = True

                    while delayed_blocks and buffered_frames > delay_frames:
                        playable = delayed_blocks.popleft()
                        buffered_frames -= len(playable)
                        last_playable = playable
                        playable = apply_channel_mode(playable, channel_mode)
                        queue_blocks = self.frames.qsize()
                        if not sync_enabled:
                            target_correction = 1.0
                            mode = "sync off"
                        elif is_silent:
                            target_correction = 1.0
                            mode = "silent"
                        elif hold_blocks > 0:
                            hold_blocks -= 1
                            target_correction = 1.0
                            mode = "settling"
                        else:
                            drift_error = queue_blocks - BASE_BUFFER_BLOCKS
                            if abs(drift_error) <= DRIFT_DEADBAND_BLOCKS:
                                target_correction = 1.0
                            else:
                                target_correction = 1.0 + max(
                                    -MAX_RESAMPLE_CORRECTION,
                                    min(MAX_RESAMPLE_CORRECTION, drift_error * DRIFT_GAIN),
                                )
                            mode = "tracking"
                        self._correction += (target_correction - self._correction) * DRIFT_SMOOTHING
                        correction = self._correction
                        corrected = playable if not sync_enabled else resample_block(playable, correction)
                        self._set_status(queue_blocks, correction, mode)
                        player.play(np.asarray(corrected * gain, dtype=np.float32))

                    if not delayed_blocks and self.frames.qsize() == 0 and last_playable is not None:
                        self.underruns += 1
                        hold_blocks = max(hold_blocks, CORRECTION_HOLD_BLOCKS // 2)
                        self._correction += (1.0 - self._correction) * DRIFT_SMOOTHING
                        self._set_status(0, self._correction, "underrun")
                        time.sleep(0.001)
        except BaseException as exc:
            self.error = exc
        finally:
            mmcss.__exit__(None, None, None)
            CoUninitialize()


class FanoutSession(threading.Thread):
    def __init__(
        self,
        source: SourceItem,
        controls: list[OutputControl],
        settings: AudioSettings,
        stop_event: threading.Event,
        status_queue: queue.Queue[object],
    ) -> None:
        super().__init__(name="maf-capture", daemon=True)
        self.source = source
        self.controls = controls
        self.settings = settings
        self.stop_event = stop_event
        self.status_queue = status_queue
        self.workers: list[PlaybackWorker] = []

    def run(self) -> None:
        CoInitialize()
        mmcss = MmcssAudioTask()
        mmcss_enabled = mmcss.__enter__()
        if mmcss_enabled:
            self.status_queue.put(("status", "MMCSS audio scheduling enabled."))
        else:
            self.status_queue.put(("status", "MMCSS audio scheduling unavailable; running normal priority."))
        try:
            blocked = [control.item.name for control in self.controls if is_maf_endpoint(control.item.name)]
            if blocked:
                self.status_queue.put(
                    ("status", "Refusing to play into the MAF virtual output; remove it from selected outputs.")
                )
                return

            if not self.controls:
                self.status_queue.put(("status", "No outputs selected."))
                return

            self.workers = [
                PlaybackWorker(control, self.settings, self.stop_event)
                for control in self.controls
            ]
            for worker in self.workers:
                worker.start()

            self.status_queue.put((
                "status",
                f"Running: {self.source.name} -> "
                + ", ".join(control.item.name for control in self.controls)
            ))

            blocks = 0
            started = time.monotonic()
            with self.source.device.recorder(
                samplerate=SAMPLE_RATE,
                channels=2,
                blocksize=BLOCK_FRAMES,
            ) as recorder:
                while not self.stop_event.is_set():
                    block = normalize_block(recorder.record(numframes=BLOCK_FRAMES))
                    block_level = float(np.sqrt(np.mean(np.square(block)))) if block.size else 0.0
                    for worker in self.workers:
                        worker.push(block.copy())
                        self.status_queue.put((
                            "device_level",
                            worker.control.item.name,
                            min(1.0, block_level * 1.4),
                        ))
                    blocks += 1

                    if blocks % 250 == 0:
                        elapsed = max(time.monotonic() - started, 0.001)
                        dropped = sum(worker.status().dropped for worker in self.workers)
                        underruns = sum(worker.status().underruns for worker in self.workers)
                        self.status_queue.put((
                            "status",
                            f"Running: {blocks / elapsed:.1f} blocks/s, "
                            f"dropped={dropped}, underruns={underruns}"
                        ))
                    if blocks % 50 == 0:
                        for worker in self.workers:
                            status = worker.status()
                            self.status_queue.put((
                                "device_status",
                                worker.control.item.name,
                                f"{status.label} drop={status.dropped} under={status.underruns}",
                            ))
        except BaseException as exc:
            self.status_queue.put(("status", f"Error: {exc}"))
        finally:
            mmcss.__exit__(None, None, None)
            self.stop_event.set()
            for worker in self.workers:
                worker.close()
            for worker in self.workers:
                worker.join(timeout=2)

            failed = [worker for worker in self.workers if worker.error is not None]
            for worker in failed:
                self.status_queue.put(("status", f"Output failed: {worker.control.item.name}: {worker.error}"))
            CoUninitialize()


class NativeFanoutSession(threading.Thread):
    def __init__(
        self,
        source: SourceItem,
        controls: list[OutputControl],
        stop_event: threading.Event,
        status_queue: queue.Queue[object],
        engine_path: Path,
        master_gain: float,
        sync_enabled: bool,
        latency_ms: float = NATIVE_LATENCY_MS,
        calibration_mic: str = "",
    ) -> None:
        super().__init__(name="maf-native-engine", daemon=True)
        self.source = source
        self.controls = controls
        self.stop_event = stop_event
        self.status_queue = status_queue
        self.engine_path = engine_path
        self.master_gain = max(0.0, float(master_gain))
        self.sync_enabled = bool(sync_enabled)
        self.latency_ms = float(latency_ms)
        self.calibration_mic = calibration_mic
        self.process: subprocess.Popen[str] | None = None
        self.unexpected_exit = False
        self._control_lock = threading.Lock()
        self._control_index = {control.item.name: index for index, control in enumerate(controls)}

    def _send_control(self, command: str) -> None:
        process = self.process
        if not process or process.poll() is not None or not process.stdin:
            return
        try:
            with self._control_lock:
                process.stdin.write(command + "\n")
                process.stdin.flush()
        except Exception:
            pass

    def set_master_gain(self, gain: float) -> None:
        self._send_control(f"master {max(0.0, float(gain)):.4f}")

    def set_output_gain(self, name: str, gain: float) -> None:
        index = self._control_index.get(name)
        if index is None:
            return
        self._send_control(f"gain {index} {max(0.0, float(gain)):.4f}")

    def set_output_delay(self, name: str, delay_ms: float) -> None:
        index = self._control_index.get(name)
        if index is None:
            return
        self._send_control(f"delay {index} {float(delay_ms):.1f}")

    def set_output_channel_mode(self, name: str, mode: str) -> None:
        index = self._control_index.get(name)
        if index is None:
            return
        self._send_control(f"mode {index} {mode.lower()}")

    def set_sync(self, enabled: bool) -> None:
        self._send_control(f"sync {1 if enabled else 0}")

    def resync(self) -> None:
        self._send_control("resync")

    def calibrate(self) -> None:
        self._send_control("calibrate")

    def mic_check(self) -> None:
        self._send_control("miccheck")

    @staticmethod
    def _device_status_text(fields: dict[str, str]) -> str:
        state = fields.get("state", "?")
        text = state
        if "err" in fields:
            text += f" {fields['err']}ms {fields.get('ppm', '0')}ppm"
        extras = [
            f"{label}={fields[key]}"
            for key, label in (("under", "under"), ("snaps", "snap"))
            if fields.get(key, "0") != "0"
        ]
        if extras:
            text += " " + " ".join(extras)
        return text

    def _read_stdout(self) -> None:
        if not self.process or not self.process.stdout:
            return
        for line in self.process.stdout:
            text = line.strip()
            if not text:
                continue
            parts = text.split()
            if parts[0] == "dev" and len(parts) >= 2 and parts[1].isdigit():
                index = int(parts[1])
                if index < len(self.controls):
                    fields = dict(part.split("=", 1) for part in parts[2:] if "=" in part)
                    self.status_queue.put((
                        "device_status",
                        self.controls[index].item.name,
                        self._device_status_text(fields),
                    ))
                continue
            if parts[0] == "cal" and len(parts) >= 2:
                try:
                    event = json.loads(text[4:])
                except ValueError:
                    event = None
                if isinstance(event, dict):
                    index = event.get("index")
                    if isinstance(index, int) and 0 <= index < len(self.controls):
                        event["name"] = self.controls[index].item.name
                    self.status_queue.put(("calibration", event))
                    continue
            if parts[0] == "src" and len(parts) >= 2 and parts[1].startswith("state="):
                if parts[1] != "state=running":
                    self.status_queue.put(("status_bar", "Waiting for the MAF source..."))
                continue
            self.status_queue.put(("status", text))

    def _output_selector(self, control: OutputControl) -> str:
        device_id = getattr(control.item.device, "id", None)
        return str(device_id) if device_id else control.item.name

    def _stop_process(self) -> None:
        if not self.process or self.process.poll() is not None:
            return
        # Ask first so the engine stops its audio clients cleanly; Bluetooth recovers faster that way.
        self._send_control("quit")
        try:
            self.process.wait(timeout=NATIVE_STOP_TIMEOUT_SECONDS)
            return
        except subprocess.TimeoutExpired:
            pass
        self.process.terminate()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)

    def run(self) -> None:
        if not self.engine_path.exists():
            self.status_queue.put(("status", f"Native engine missing: {self.engine_path}"))
            self.stop_event.set()
            return

        source_selector = MAF_ENDPOINT_MARKER if is_maf_endpoint(self.source.name) else self.source.name
        args = [
            str(self.engine_path),
            "--source",
            source_selector,
            "--master",
            f"{self.master_gain:.4f}",
            "--latency-ms",
            f"{self.latency_ms:.0f}",
            "--exit-on-stdin-eof",
        ]
        if not self.sync_enabled:
            args.append("--no-sync")
        if self.calibration_mic:
            args.extend(["--mic", self.calibration_mic])
        for control in self.controls:
            mode = control.channel_mode.get().lower()
            gain = max(0.0, float(control.volume.get()))
            delay_ms = float(control.delay_ms.get())
            args.extend(["--out", f"{self._output_selector(control)}|{mode}|{gain:.4f}|{delay_ms:.1f}"])

        creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        try:
            self.process = subprocess.Popen(
                args,
                cwd=str(self.engine_path.parent),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                creationflags=creation_flags,
            )
            self.status_queue.put((
                "status",
                "Running native engine: " + ", ".join(control.item.name for control in self.controls),
            ))
            for control in self.controls:
                self.status_queue.put(("device_status", control.item.name, "starting"))

            reader = threading.Thread(target=self._read_stdout, name="maf-native-stdout", daemon=True)
            reader.start()
            while not self.stop_event.is_set():
                if self.process.poll() is not None:
                    break
                time.sleep(0.05)

            if self.stop_event.is_set():
                self._stop_process()
            else:
                self.unexpected_exit = True
                return_code = self.process.returncode
                self.status_queue.put(("status", f"Error: native engine exited with code {return_code}"))
        except BaseException as exc:
            self.status_queue.put(("status", f"Error: native engine failed: {exc}"))
        finally:
            self.stop_event.set()


class MAFApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("MAF - Multi Audio Fanout")
        self.geometry("920x620")
        self.minsize(760, 520)

        self.sources: list[SourceItem] = []
        self.outputs: list[OutputItem] = []
        self.controls: list[OutputControl] = []
        self.source_var = tk.StringVar()
        self.master_volume = tk.DoubleVar(value=1.0)
        self.settings = AudioSettings()
        self.output_name_width = self.settings.output_name_width
        self.sync_enabled = tk.BooleanVar(value=self.settings.sync_enabled)
        self.sync_button_text = tk.StringVar()
        self.auto_reset_enabled = tk.BooleanVar(value=self.settings.auto_reset_enabled)
        self.windows_volume: WindowsMasterVolume | None = None
        self.follow_windows_volume = tk.BooleanVar(value=True)
        self._syncing_windows_volume = False
        self._last_windows_volume: float | None = None
        self.status_var = tk.StringVar(value="Idle")
        self.status_queue: queue.Queue[object] = queue.Queue()
        self.output_levels: dict[str, float] = {}
        self.stop_event: threading.Event | None = None
        self.session: FanoutSession | NativeFanoutSession | None = None
        self._reset_pending = False
        self._restart_scheduled = False
        self._dragging_reports: dict[str, int] = {}
        self._last_auto_reset = 0.0
        self._native_restarts: deque[float] = deque()
        self._calibration_report: list[str] = []
        self._name_resize_start_x = 0
        self._name_resize_start_width = self.output_name_width

        self._build_ui()
        self._update_sync_button()
        self._bind_master_volume()
        self._init_windows_volume()
        self.refresh_devices()
        self.after(WINDOWS_VOLUME_POLL_MS, self._poll_windows_volume)
        self.after(200, self._poll_status)
        self.after(VISUALIZER_REFRESH_MS, self._draw_visualizer)
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._log("MAF ready")

    def _build_ui(self) -> None:
        outer = ttk.Frame(self, padding=14)
        outer.pack(fill=tk.BOTH, expand=True)

        top = ttk.Frame(outer)
        top.pack(fill=tk.X)

        ttk.Label(top, text="MAF Source").pack(side=tk.LEFT)
        self.source_combo = ttk.Combobox(top, textvariable=self.source_var, state="readonly", width=54)
        self.source_combo.pack(side=tk.LEFT, padx=(10, 8), fill=tk.X, expand=True)
        self.source_combo.bind("<<ComboboxSelected>>", lambda _event: self._render_outputs())
        ttk.Button(top, text="Refresh", command=self.refresh_devices).pack(side=tk.LEFT)

        volume_row = ttk.Frame(outer)
        volume_row.pack(fill=tk.X, pady=(14, 8))
        ttk.Label(volume_row, text="Master Volume").pack(side=tk.LEFT)
        ttk.Button(
            volume_row,
            text="-",
            width=3,
            command=lambda: self._nudge(self.master_volume, -VOLUME_STEP, 0.0, 1.5),
        ).pack(side=tk.LEFT, padx=(10, 0))
        ttk.Scale(
            volume_row,
            from_=0.0,
            to=1.5,
            variable=self.master_volume,
            orient=tk.HORIZONTAL,
        ).pack(side=tk.LEFT, padx=6, fill=tk.X, expand=True)
        ttk.Button(
            volume_row,
            text="+",
            width=3,
            command=lambda: self._nudge(self.master_volume, VOLUME_STEP, 0.0, 1.5),
        ).pack(side=tk.LEFT, padx=(0, 10))
        ttk.Label(volume_row, textvariable=self._volume_label(self.master_volume)).pack(side=tk.LEFT)
        ttk.Checkbutton(
            volume_row,
            text="Follow Windows",
            variable=self.follow_windows_volume,
        ).pack(side=tk.LEFT, padx=(12, 0))

        body = ttk.Frame(outer)
        body.pack(fill=tk.BOTH, expand=True, pady=(10, 0))

        output_area = ttk.Frame(body)
        output_area.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.output_canvas = tk.Canvas(output_area, highlightthickness=0)
        scrollbar = ttk.Scrollbar(output_area, orient=tk.VERTICAL, command=self.output_canvas.yview)
        self.output_frame = ttk.Frame(self.output_canvas)
        self.output_window = self.output_canvas.create_window((0, 0), window=self.output_frame, anchor="nw")
        self.output_frame.bind(
            "<Configure>",
            lambda _event: self.output_canvas.configure(scrollregion=self.output_canvas.bbox("all")),
        )
        self.output_canvas.bind(
            "<Configure>",
            lambda event: self.output_canvas.itemconfigure(self.output_window, width=event.width),
        )
        self.output_canvas.configure(yscrollcommand=scrollbar.set)
        self.output_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        side_panel = ttk.Frame(body, width=320)
        side_panel.pack(side=tk.RIGHT, fill=tk.Y, padx=(12, 0))
        side_panel.pack_propagate(False)

        session_panel = ttk.PanedWindow(side_panel, orient=tk.VERTICAL)
        session_panel.pack(fill=tk.BOTH, expand=True)

        log_frame = ttk.Frame(session_panel)
        ttk.Label(log_frame, text="Session Log").pack(anchor="w")
        self.log_text = tk.Text(log_frame, height=10, wrap=tk.WORD, state=tk.DISABLED)
        log_font = font.nametofont(str(self.log_text.cget("font")))
        self.log_timestamp_font = log_font.copy()
        self.log_timestamp_font.configure(weight="bold")
        self.log_text.tag_configure("timestamp", font=self.log_timestamp_font)
        log_scrollbar = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=log_scrollbar.set)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, pady=(4, 0))
        log_scrollbar.pack(side=tk.RIGHT, fill=tk.Y, pady=(4, 0))

        visual_frame = ttk.Frame(session_panel)
        ttk.Label(visual_frame, text="Output Levels").pack(anchor="w")
        self.visual_canvas = tk.Canvas(visual_frame, height=140, highlightthickness=0)
        self.visual_canvas.pack(fill=tk.BOTH, expand=True, pady=(4, 0))

        session_panel.add(log_frame, weight=1)
        session_panel.add(visual_frame, weight=1)

        bottom = ttk.Frame(outer)
        bottom.pack(fill=tk.X, pady=(12, 0))
        self.start_button = ttk.Button(bottom, text="Start", command=self.start)
        self.start_button.pack(side=tk.LEFT)
        self.stop_button = ttk.Button(bottom, text="Stop", command=self.stop, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=(8, 0))
        self.reset_button = ttk.Button(bottom, text="Reset", command=self.reset, state=tk.DISABLED)
        self.reset_button.pack(side=tk.LEFT, padx=(8, 0))
        self.resync_button = ttk.Button(bottom, text="Resync", command=self.resync, state=tk.DISABLED)
        self.resync_button.pack(side=tk.LEFT, padx=(8, 0))
        self.mic_check_button = ttk.Button(bottom, text="Test Mic", command=self.mic_check, state=tk.DISABLED)
        self.mic_check_button.pack(side=tk.LEFT, padx=(8, 0))
        self.calibrate_button = ttk.Button(bottom, text="Calibrate", command=self.calibrate, state=tk.DISABLED)
        self.calibrate_button.pack(side=tk.LEFT, padx=(8, 0))
        self.sync_button = ttk.Button(bottom, textvariable=self.sync_button_text, command=self._toggle_sync)
        self.sync_button.pack(side=tk.LEFT, padx=(8, 0))
        ttk.Checkbutton(
            bottom,
            text="Auto Reset",
            variable=self.auto_reset_enabled,
            command=self._toggle_auto_reset,
        ).pack(side=tk.LEFT, padx=(8, 0))
        ttk.Label(bottom, textvariable=self.status_var).pack(side=tk.LEFT, padx=(16, 0), fill=tk.X, expand=True)

    def _bind_master_volume(self) -> None:
        def update(*_args: object) -> None:
            self.settings.set_master_volume(self.master_volume.get())
            self._send_native_master_gain()
            if (
                self.windows_volume is not None
                and not self._syncing_windows_volume
            ):
                try:
                    windows_value = min(1.0, max(0.0, self.master_volume.get()))
                    self.windows_volume.set(windows_value)
                    self._last_windows_volume = windows_value
                except Exception:
                    pass

        self.master_volume.trace_add("write", update)
        update()

    def _send_native_master_gain(self) -> None:
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.set_master_gain(self.master_volume.get())

    def _send_native_output_gain(self, control: OutputControl) -> None:
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.set_output_gain(control.item.name, control.volume.get())

    def _send_native_output_delay(self, control: OutputControl) -> None:
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.set_output_delay(control.item.name, control.delay_ms.get())

    def _send_native_channel_mode(self, control: OutputControl) -> None:
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.set_output_channel_mode(control.item.name, control.channel_mode.get())

    def _toggle_sync(self) -> None:
        self.sync_enabled.set(not self.sync_enabled.get())
        self.settings.set_sync_enabled(self.sync_enabled.get())
        self._update_sync_button()
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.set_sync(self.sync_enabled.get())
        self._log(f"Sync {'enabled' if self.sync_enabled.get() else 'disabled'}")

    def _toggle_auto_reset(self) -> None:
        self.settings.set_auto_reset_enabled(self.auto_reset_enabled.get())
        self._dragging_reports.clear()
        self._log(f"Auto reset {'enabled' if self.auto_reset_enabled.get() else 'disabled'}")

    def _update_sync_button(self) -> None:
        if self.sync_enabled.get():
            self.sync_button_text.set("Sync On")
        else:
            self.sync_button_text.set("Sync Off")

    def _log(self, message: str) -> None:
        if not hasattr(self, "log_text"):
            return
        timestamp = f"[{time.strftime('%H:%M:%S')}] "
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, timestamp, "timestamp")
        self.log_text.insert(tk.END, f"{message}\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    def _draw_visualizer(self) -> None:
        if not hasattr(self, "visual_canvas"):
            return
        canvas = self.visual_canvas
        canvas.delete("all")
        width = max(1, canvas.winfo_width())
        height = max(1, canvas.winfo_height())
        active = [
            control
            for control in self.controls
            if control.item.connected and control.enabled.get()
        ]
        if not active:
            canvas.create_text(width // 2, height // 2, text="No active outputs", fill="#666")
            self.after(VISUALIZER_REFRESH_MS, self._draw_visualizer)
            return

        row_height = max(22, height // max(1, len(active)))
        label_width = min(120, max(72, width // 3))
        bar_left = label_width + 8
        bar_right = width - 8
        for index, control in enumerate(active):
            top = index * row_height + 4
            center = top + row_height // 2
            name = compact_text(control.item.name, 18)
            level = min(1.0, max(0.0, self.output_levels.get(control.item.name, 0.0)))
            canvas.create_text(4, center, text=name, anchor="w", fill="#333")
            canvas.create_rectangle(bar_left, center - 5, bar_right, center + 5, outline="#aaa")
            canvas.create_rectangle(
                bar_left,
                center - 5,
                bar_left + int((bar_right - bar_left) * level),
                center + 5,
                fill="#3a7",
                outline="",
            )
        for name in list(self.output_levels):
            self.output_levels[name] *= VISUALIZER_DECAY
            if self.output_levels[name] < 0.001:
                self.output_levels[name] = 0.0
        self.after(VISUALIZER_REFRESH_MS, self._draw_visualizer)

    def _init_windows_volume(self) -> None:
        try:
            self.windows_volume = WindowsMasterVolume()
            value = self.windows_volume.get()
            self._last_windows_volume = value
            self._syncing_windows_volume = True
            self.master_volume.set(value)
            self._syncing_windows_volume = False
        except Exception as exc:
            self.windows_volume = None
            self.status_var.set(f"Windows volume unavailable: {exc}")

    def _poll_windows_volume(self) -> None:
        try:
            if self.windows_volume is not None and self.follow_windows_volume.get():
                value = self.windows_volume.get()
                if self._last_windows_volume is None or abs(value - self._last_windows_volume) > 0.002:
                    self._last_windows_volume = value
                    self._syncing_windows_volume = True
                    self.master_volume.set(value)
                    self._syncing_windows_volume = False
        except Exception:
            try:
                if self.windows_volume is not None:
                    self.windows_volume.refresh()
            except Exception:
                self.windows_volume = None
        finally:
            self.after(WINDOWS_VOLUME_POLL_MS, self._poll_windows_volume)

    def _nudge(self, variable: tk.DoubleVar, delta: float, minimum: float, maximum: float) -> None:
        value = min(maximum, max(minimum, variable.get() + delta))
        variable.set(round(value, 4))

    def _volume_label(self, variable: tk.DoubleVar) -> tk.StringVar:
        label = tk.StringVar()

        def update(*_args: object) -> None:
            label.set(f"{int(variable.get() * 100)}%")

        variable.trace_add("write", update)
        update()
        return label

    def _begin_name_resize(self, event: tk.Event) -> None:
        self._name_resize_start_x = int(event.x_root)
        self._name_resize_start_width = self.output_name_width
        self.bind("<B1-Motion>", self._drag_name_resize)
        self.bind("<ButtonRelease-1>", self._end_name_resize)

    def _drag_name_resize(self, event: tk.Event) -> None:
        delta_chars = round((int(event.x_root) - self._name_resize_start_x) / 8)
        new_width = min(
            MAX_OUTPUT_NAME_WIDTH,
            max(MIN_OUTPUT_NAME_WIDTH, self._name_resize_start_width + delta_chars),
        )
        if new_width == self.output_name_width:
            return
        self.output_name_width = new_width
        self.settings.set_output_name_width(new_width)
        self._render_outputs()

    def _end_name_resize(self, _event: tk.Event) -> None:
        self.unbind("<B1-Motion>")
        self.unbind("<ButtonRelease-1>")

    def refresh_devices(self) -> None:
        self.sources = [
            SourceItem(
                index=index,
                name=device_name(device),
                device=device,
                is_loopback=bool(getattr(device, "isloopback", False)),
            )
            for index, device in enumerate(sc.all_microphones(include_loopback=True))
        ]
        connected_outputs = [
            OutputItem(index=index, name=device_name(device), device=device, connected=True)
            for index, device in enumerate(
                device for device in sc.all_speakers() if not is_maf_endpoint(device_name(device))
            )
        ]
        connected_names = {output.name for output in connected_outputs}
        for name in connected_names:
            self.settings.remember_output(name)
        offline_outputs = [
            OutputItem(index=-1, name=name, device=None, connected=False)
            for name in sorted(self.settings.known_outputs - connected_names)
        ]
        self.outputs = connected_outputs + offline_outputs

        self.source_combo["values"] = [source.label for source in self.sources]
        preferred = self._preferred_source_index()
        if preferred is not None:
            self.source_combo.current(preferred)
        elif self.sources:
            self.source_combo.current(0)

        self._render_outputs()
        self.status_var.set(f"Found {len(connected_outputs)} connected outputs")
        if hasattr(self, "log_text"):
            self._log(
                f"Refreshed devices: {len(connected_outputs)} connected, {len(offline_outputs)} offline remembered"
            )

    def _preferred_source_index(self) -> int | None:
        for index, source in enumerate(self.sources):
            if is_maf_endpoint(source.name) and source.is_loopback and source.name.startswith("Speakers"):
                return index
        for index, source in enumerate(self.sources):
            if is_maf_endpoint(source.name) and source.is_loopback:
                return index
        for index, source in enumerate(self.sources):
            if "Virtual Audio Cable" in source.name and not source.is_loopback:
                return index
        for index, source in enumerate(self.sources):
            if "Line 1" in source.name and not source.is_loopback:
                return index
        return None

    def _selected_source(self) -> SourceItem:
        selection = self.source_combo.current()
        if selection < 0:
            raise ValueError("No source selected.")
        return self.sources[selection]

    def _render_outputs(self) -> None:
        for child in self.output_frame.winfo_children():
            child.destroy()
        self.controls = []

        outputs_header = ttk.Label(self.output_frame, text="Outputs", cursor="sb_h_double_arrow")
        outputs_header.grid(row=0, column=0, sticky="ew", padx=(0, 12), pady=(0, 4))
        outputs_header.bind("<ButtonPress-1>", self._begin_name_resize)
        outputs_header.bind("<B1-Motion>", self._drag_name_resize)
        ttk.Label(self.output_frame, text="Volume").grid(
            row=0,
            column=1,
            columnspan=4,
            sticky="w",
            padx=(4, 0),
            pady=(0, 4),
        )
        ttk.Label(self.output_frame, text="Delay ms").grid(
            row=0,
            column=5,
            columnspan=4,
            sticky="w",
            padx=(4, 0),
            pady=(0, 4),
        )
        ttk.Label(self.output_frame, text="Channel").grid(row=0, column=9, sticky="w", padx=(8, 0), pady=(0, 4))
        ttk.Label(self.output_frame, text="Sync").grid(row=0, column=10, sticky="w", padx=(8, 0), pady=(0, 4))

        source_name = ""
        try:
            source_name = self._selected_source().name
        except Exception:
            pass

        row = 1
        for output in self.outputs:
            if is_maf_endpoint(output.name):
                continue
            if output.connected and output.name == source_name:
                continue

            enabled = tk.BooleanVar(value=output.connected and self.settings.output_selected.get(output.name, False))
            volume = tk.DoubleVar(value=self.settings.output_volume.get(output.name, 1.0))
            delay_ms = tk.DoubleVar(value=self.settings.output_delay_ms.get(output.name, 0.0))
            channel_mode = tk.StringVar(value=self.settings.output_channel_mode.get(output.name, DEFAULT_CHANNEL_MODE))
            status = tk.StringVar(value="idle" if output.connected else "offline")
            control = OutputControl(output, enabled, volume, delay_ms, channel_mode, status)
            self.controls.append(control)
            self._bind_output_settings(control)

            ttk.Checkbutton(
                self.output_frame,
                text=compact_text(
                    f"[{output.index}] {output.name}" if output.connected else output.name,
                    self.output_name_width,
                ),
                width=self.output_name_width,
                variable=enabled,
                command=lambda control=control: self._toggle_output_selected(control),
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=0, sticky="w", padx=(0, 12), pady=7)
            ttk.Button(
                self.output_frame,
                text="-",
                width=3,
                command=lambda variable=volume: self._nudge(variable, -VOLUME_STEP, 0.0, 1.5),
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=1, sticky="e", padx=(4, 2), pady=7)
            ttk.Scale(
                self.output_frame,
                from_=0.0,
                to=1.5,
                variable=volume,
                orient=tk.HORIZONTAL,
                length=145,
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=2, sticky="ew", padx=(4, 4), pady=7)
            ttk.Button(
                self.output_frame,
                text="+",
                width=3,
                command=lambda variable=volume: self._nudge(variable, VOLUME_STEP, 0.0, 1.5),
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=3, sticky="w", padx=(2, 8), pady=7)
            ttk.Label(self.output_frame, textvariable=self._volume_label(volume), width=5).grid(
                row=row,
                column=4,
                sticky="w",
                padx=(0, 14),
            )
            ttk.Button(
                self.output_frame,
                text="-5",
                width=4,
                command=lambda variable=delay_ms: self._nudge(variable, -DELAY_STEP_MS, MIN_DELAY_MS, MAX_DELAY_MS),
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=5, sticky="e", padx=(4, 2), pady=7)
            ttk.Scale(
                self.output_frame,
                from_=MIN_DELAY_MS,
                to=MAX_DELAY_MS,
                variable=delay_ms,
                orient=tk.HORIZONTAL,
                length=155,
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=6, sticky="ew", padx=(4, 4), pady=7)
            ttk.Button(
                self.output_frame,
                text="+5",
                width=4,
                command=lambda variable=delay_ms: self._nudge(variable, DELAY_STEP_MS, MIN_DELAY_MS, MAX_DELAY_MS),
                state=tk.NORMAL if output.connected else tk.DISABLED,
            ).grid(row=row, column=7, sticky="w", padx=(2, 8), pady=7)
            ttk.Label(self.output_frame, textvariable=self._delay_label(delay_ms), width=9).grid(
                row=row,
                column=8,
                sticky="w",
            )
            channel_combo = ttk.Combobox(
                self.output_frame,
                textvariable=channel_mode,
                values=CHANNEL_MODES,
                state="readonly" if output.connected else tk.DISABLED,
                width=8,
            )
            channel_combo.bind("<<ComboboxSelected>>", lambda _event, control=control: self._set_output_channel_mode(control))
            channel_combo.grid(row=row, column=9, sticky="w", padx=(8, 0), pady=7)
            if output.connected:
                ttk.Label(self.output_frame, textvariable=status, width=34).grid(
                    row=row,
                    column=10,
                    sticky="w",
                    padx=(8, 0),
                )
            else:
                offline_cell = ttk.Frame(self.output_frame)
                offline_cell.grid(row=row, column=10, sticky="w", padx=(8, 0))
                ttk.Label(offline_cell, textvariable=status, width=12).pack(side=tk.LEFT)
                ttk.Button(
                    offline_cell,
                    text="Delete",
                    command=lambda name=output.name: self._forget_output(name),
                ).pack(side=tk.LEFT, padx=(6, 0))
            row += 1

        self.output_frame.columnconfigure(0, weight=0, minsize=(self.output_name_width * 8) + 24)
        self.output_frame.columnconfigure(2, weight=0)
        self.output_frame.columnconfigure(6, weight=0)
        self.output_frame.columnconfigure(9, weight=0)
        self.output_frame.columnconfigure(10, weight=1)

    def _bind_output_settings(self, control: OutputControl) -> None:
        def update_volume(*_args: object) -> None:
            self.settings.set_output_volume(control.item.name, control.volume.get())
            self._send_native_output_gain(control)

        def update_delay(*_args: object) -> None:
            self.settings.set_output_delay_ms(control.item.name, control.delay_ms.get())
            self._sync_delay_floor()
            self._send_native_output_delay(control)

        def update_channel(*_args: object) -> None:
            self.settings.set_output_channel_mode(control.item.name, control.channel_mode.get())
            self._send_native_channel_mode(control)

        control.volume.trace_add("write", update_volume)
        control.delay_ms.trace_add("write", update_delay)
        control.channel_mode.trace_add("write", update_channel)
        update_volume()
        update_delay()
        update_channel()

    def _set_output_channel_mode(self, control: OutputControl) -> None:
        self.settings.set_output_channel_mode(control.item.name, control.channel_mode.get())
        self._log(f"Channel for {control.item.name}: {control.channel_mode.get()}")

    def _toggle_output_selected(self, control: OutputControl) -> None:
        if not control.item.connected:
            control.enabled.set(False)
            return
        self.settings.set_output_selected(control.item.name, control.enabled.get())
        self._sync_delay_floor()
        self._log(f"{'Selected' if control.enabled.get() else 'Deselected'} output: {control.item.name}")

    def _forget_output(self, name: str) -> None:
        self.settings.forget_output(name)
        self.outputs = [output for output in self.outputs if output.name != name]
        self._render_outputs()
        self._log(f"Deleted offline device from history: {name}")

    def _sync_delay_floor(self) -> None:
        active = [
            control.delay_ms.get()
            for control in self.controls
            if control.item.connected and control.enabled.get()
        ]
        floor = min(active) if active else 0.0
        self.settings.set_delay_floor_ms(floor)

    def _delay_label(self, variable: tk.DoubleVar) -> tk.StringVar:
        label = tk.StringVar()

        def update(*_args: object) -> None:
            label.set(f"{int(variable.get())} ms")

        variable.trace_add("write", update)
        update()
        return label

    def start(self) -> None:
        if self.session and self.session.is_alive():
            return

        self._reset_pending = False
        self._restart_scheduled = False
        self._dragging_reports.clear()
        selected = [control for control in self.controls if control.item.connected and control.enabled.get()]
        blocked = [control.item.name for control in selected if is_maf_endpoint(control.item.name)]
        if blocked:
            messagebox.showerror(
                APP_NAME,
                "MAF cannot fan out to its own virtual output. Select only physical or Bluetooth outputs.",
            )
            return
        if not selected:
            messagebox.showwarning(APP_NAME, "Select at least one output device.")
            return

        self._sync_delay_floor()

        try:
            source = self._selected_source()
        except Exception as exc:
            messagebox.showerror(APP_NAME, str(exc))
            return

        self.stop_event = threading.Event()
        engine_path = native_engine_path()
        if engine_path.exists():
            self.session = NativeFanoutSession(
                source=source,
                controls=selected,
                stop_event=self.stop_event,
                status_queue=self.status_queue,
                engine_path=engine_path,
                master_gain=self.master_volume.get(),
                sync_enabled=self.sync_enabled.get(),
                calibration_mic=self.settings.calibration_mic,
            )
            self._log(f"Using native engine: {engine_path}")
        else:
            self.session = FanoutSession(
                source=source,
                controls=selected,
                settings=self.settings,
                stop_event=self.stop_event,
                status_queue=self.status_queue,
            )
            self._log("Native engine not found; using Python engine")
        self.session.start()
        self.start_button.configure(state=tk.DISABLED)
        self.stop_button.configure(state=tk.NORMAL)
        self.reset_button.configure(state=tk.NORMAL)
        self.resync_button.configure(state=tk.NORMAL)
        self._set_calibration_buttons(isinstance(self.session, NativeFanoutSession))
        self.status_var.set("Starting...")
        self.output_levels = {control.item.name: 0.0 for control in selected}
        self._log("Started fanout: " + ", ".join(control.item.name for control in selected))

    def stop(self, status: str = "Stopping...") -> None:
        if self.stop_event:
            self.stop_event.set()
        self.settings.set_delay_floor_ms(0.0)
        self.start_button.configure(state=tk.NORMAL)
        self.stop_button.configure(state=tk.DISABLED)
        self.reset_button.configure(state=tk.DISABLED)
        self.resync_button.configure(state=tk.DISABLED)
        self._set_calibration_buttons(False)
        self.status_var.set(status)
        self._log(status)

    def _set_calibration_buttons(self, enabled: bool) -> None:
        state = tk.NORMAL if enabled else tk.DISABLED
        self.mic_check_button.configure(state=state)
        self.calibrate_button.configure(state=state)

    def mic_check(self) -> None:
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.mic_check()

    def calibrate(self) -> None:
        if not (isinstance(self.session, NativeFanoutSession) and self.session.is_alive()):
            return
        if not messagebox.askokcancel(
            APP_NAME,
            "Calibration mutes the music for about 12 seconds and plays a short sweep on each speaker, "
            "three times.\n\nPut the laptop where you usually listen, keep the room quiet, then press OK.",
        ):
            return
        self.session.calibrate()

    def _handle_calibration_event(self, event: dict) -> None:
        kind = event.get("event")
        name = str(event.get("name", ""))
        detail = str(event.get("detail", ""))
        if kind == "start":
            self._calibration_report = []
            self._set_calibration_buttons(False)
            if event.get("mode") == "calibrate":
                self._log("Calibration started")
                self.status_var.set("Calibrating: checking the microphone...")
            else:
                self._log("Microphone test started")
                self.status_var.set("Testing the microphone...")
        elif kind == "mic":
            raw = " (unprocessed)" if event.get("raw") else ""
            self._log(f"Microphone: {name}{raw}")
        elif kind == "warning":
            self._calibration_report.append(f"Warning: {detail}")
            self._log(f"Microphone warning: {detail}")
        elif kind == "mic_ok":
            self._log(f"Microphone OK, level {event.get('level_dbfs', 0):.0f} dBFS")
        elif kind == "playing":
            self.status_var.set("Calibrating: playing sweeps...")
        elif kind == "dev":
            status = event.get("status")
            if status == "ok":
                line = f"{name}: {event.get('latency_ms', 0):.1f} ms (spread {event.get('spread_ms', 0):.1f} ms)"
            elif status == "unknown":
                return
            else:
                line = f"{name}: {detail}"
            self._calibration_report.append(line)
            self._log(f"Calibration {line}")
        elif kind == "delay":
            for control in self.controls:
                if control.item.name == name:
                    control.delay_ms.set(round(float(event.get("ms", 0.0)), 1))
                    break
            self._calibration_report.append(f"{name}: delay set to {event.get('ms', 0):.1f} ms")
        elif kind in ("done", "fail"):
            self._set_calibration_buttons(isinstance(self.session, NativeFanoutSession) and self.session.is_alive())
            code = str(event.get("code", ""))
            report = "\n".join(self._calibration_report)
            message = detail + (f"\n\n{report}" if report else "")
            self._log(f"Calibration {kind}: {code} {detail}")
            self.status_var.set(detail)
            if kind == "fail" and code == "STOPPED":
                return
            if kind == "fail":
                title = "Microphone problem" if code.startswith("MIC_") else "Calibration failed"
                messagebox.showwarning(title, message)
            elif event.get("applied") or code == "MIC_OK":
                messagebox.showinfo(APP_NAME, message)
            else:
                messagebox.showwarning(APP_NAME, message)

    def resync(self) -> None:
        # Native engine: realign every output in place, without reopening the speakers.
        if isinstance(self.session, NativeFanoutSession) and self.session.is_alive():
            self.session.resync()
            self._log("Resync requested")
            return
        self.reset(reason="Resync")

    def reset(self, reason: str = "Manual reset") -> None:
        if self.session and self.session.is_alive():
            self._reset_pending = True
            self._restart_scheduled = False
            self._dragging_reports.clear()
            self._log(f"Reset requested: {reason}")
            self.stop(status=f"Resetting: {reason}")
            return
        self.start()

    def _restart_after_reset(self) -> None:
        self._restart_scheduled = False
        if not self._reset_pending:
            return
        self._reset_pending = False
        self.status_var.set("Restarting...")
        self.start()

    def _maybe_auto_reset(self, name: str, text: str) -> None:
        if not self.auto_reset_enabled.get() or not self.sync_enabled.get():
            return
        if self._reset_pending or self._restart_scheduled:
            return
        if not (self.session and self.session.is_alive()):
            return

        if text.startswith("dragging"):
            count = self._dragging_reports.get(name, 0) + 1
            self._dragging_reports[name] = count
        else:
            self._dragging_reports[name] = 0
            return

        now = time.monotonic()
        if count < AUTO_RESET_DRAGGING_REPORTS:
            return
        if now - self._last_auto_reset < AUTO_RESET_COOLDOWN_SECONDS:
            return

        self._last_auto_reset = now
        self._log(f"Auto reset triggered: {name} reported dragging")
        self.reset(reason=f"{name} dragging")

    def _poll_status(self) -> None:
        while True:
            try:
                event = self.status_queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(event, tuple) and event:
                if event[0] == "status" and len(event) >= 2:
                    text = str(event[1])
                    self.status_var.set(text)
                    if not text.startswith("Running:") or "blocks/s" not in text:
                        self._log(text)
                elif event[0] == "status_bar" and len(event) >= 2:
                    self.status_var.set(str(event[1]))
                elif event[0] == "calibration" and len(event) >= 2 and isinstance(event[1], dict):
                    self._handle_calibration_event(event[1])
                elif event[0] == "device_status" and len(event) >= 3:
                    name = str(event[1])
                    text = str(event[2])
                    for control in self.controls:
                        if control.item.name == name:
                            control.status.set(text)
                            break
                    self._maybe_auto_reset(name, text)
                elif event[0] == "device_level" and len(event) >= 3:
                    name = str(event[1])
                    try:
                        level = float(event[2])
                    except Exception:
                        level = 0.0
                    self.output_levels[name] = max(self.output_levels.get(name, 0.0), level)
            else:
                self.status_var.set(str(event))

        if self.session and not self.session.is_alive():
            self.settings.set_delay_floor_ms(0.0)
            if not self._reset_pending and self._should_auto_restart_native():
                self._reset_pending = True
                self._restart_scheduled = False
                self._log("Native engine stopped unexpectedly; restarting")
            if self._reset_pending:
                self.start_button.configure(state=tk.DISABLED)
                self.stop_button.configure(state=tk.DISABLED)
                self.reset_button.configure(state=tk.DISABLED)
                self.resync_button.configure(state=tk.DISABLED)
                self._set_calibration_buttons(False)
                if not self._restart_scheduled:
                    self._restart_scheduled = True
                    self.after(RESET_RESTART_DELAY_MS, self._restart_after_reset)
            else:
                current_status = self.status_var.get()
                self.session = None
                self.start_button.configure(state=tk.NORMAL)
                self.stop_button.configure(state=tk.DISABLED)
                self.reset_button.configure(state=tk.DISABLED)
                self.resync_button.configure(state=tk.DISABLED)
                self._set_calibration_buttons(False)
                self._dragging_reports.clear()
                if not (current_status.startswith("Error:") or current_status.startswith("Output failed:")):
                    self.status_var.set("Stopped")
                    self._log("Stopped")

        self.after(200, self._poll_status)

    def _should_auto_restart_native(self) -> bool:
        if not (isinstance(self.session, NativeFanoutSession) and self.session.unexpected_exit):
            return False
        now = time.monotonic()
        while self._native_restarts and now - self._native_restarts[0] > NATIVE_AUTO_RESTART_WINDOW_SECONDS:
            self._native_restarts.popleft()
        if len(self._native_restarts) >= NATIVE_AUTO_RESTART_LIMIT:
            self._log("Native engine keeps stopping; not restarting again automatically")
            return False
        self._native_restarts.append(now)
        return True

    def _on_close(self) -> None:
        self.stop()
        self.destroy()


def main() -> None:
    app = MAFApp()
    app.mainloop()


if __name__ == "__main__":
    main()
