from __future__ import annotations

import argparse
import json
import queue
import signal
import sys
import threading
import time
from dataclasses import dataclass

import numpy as np
import soundcard as sc


SAMPLE_RATE = 48_000
BLOCK_FRAMES = 960
QUEUE_BLOCKS = 12
MAF_ENDPOINT_MARKER = "MAF Virtual Audio Output"


def is_maf_endpoint(name: str) -> bool:
    return MAF_ENDPOINT_MARKER.casefold() in name.casefold()


@dataclass(frozen=True)
class OutputTarget:
    index: int
    name: str
    device: object


def device_name(device: object) -> str:
    name = getattr(device, "name", None)
    if name:
        return str(name)
    try:
        return str(device)
    except Exception:
        return type(device).__name__


def active_speakers() -> list[OutputTarget]:
    return [
        OutputTarget(index=i, name=device_name(speaker), device=speaker)
        for i, speaker in enumerate(
            speaker for speaker in sc.all_speakers() if not is_maf_endpoint(device_name(speaker))
        )
    ]


def active_loopbacks() -> list[object]:
    return list(sc.all_microphones(include_loopback=True))


def print_devices() -> None:
    default = sc.default_speaker()
    print(f"Default output: {device_name(default)}")
    print()
    print("Playback outputs:")
    for target in active_speakers():
        print(f"  [{target.index}] {target.name}")
    print()
    print("Loopback/capture inputs:")
    for index, mic in enumerate(active_loopbacks()):
        kind = "loopback" if getattr(mic, "isloopback", False) else "capture"
        print(f"  [{index}] {device_name(mic)} ({kind})")


def select_one_device(devices: list[object], selector: str, label: str) -> object:
    if selector.isdigit():
        index = int(selector)
        try:
            return devices[index]
        except IndexError as exc:
            raise ValueError(f"No {label} at index {index}.") from exc

    needle = selector.lower()
    exact = [device for device in devices if device_name(device).lower() == needle]
    if len(exact) == 1:
        return exact[0]

    matches = [device for device in devices if needle in device_name(device).lower()]
    if not matches:
        raise ValueError(f"No {label} matching {selector!r}.")
    if len(matches) > 1:
        names = ", ".join(device_name(match) for match in matches)
        raise ValueError(f"{label.title()} selector {selector!r} is ambiguous: {names}")
    return matches[0]


def find_default_loopback() -> object:
    default_name = device_name(sc.default_speaker())
    loopbacks = active_loopbacks()

    for mic in loopbacks:
        name = device_name(mic)
        if getattr(mic, "isloopback", False) and is_maf_endpoint(name) and name.startswith("Speakers"):
            return mic

    for mic in loopbacks:
        if getattr(mic, "isloopback", False) and is_maf_endpoint(device_name(mic)):
            return mic

    for mic in loopbacks:
        if getattr(mic, "isloopback", False) and device_name(mic) == default_name:
            return mic

    for mic in loopbacks:
        if getattr(mic, "isloopback", False):
            return mic

    raise RuntimeError("No WASAPI loopback input is available.")


def filter_sources(source_kind: str) -> list[object]:
    devices = active_loopbacks()
    if source_kind == "any":
        return devices
    want_loopback = source_kind == "loopback"
    return [device for device in devices if bool(getattr(device, "isloopback", False)) == want_loopback]


def find_source(source_selector: str | None, source_kind: str) -> object:
    if source_selector:
        return select_one_device(filter_sources(source_kind), source_selector, "capture input")
    if source_kind == "capture":
        raise ValueError("--source is required when --source-kind capture is used.")
    return find_default_loopback()


def parse_output_selection(selection: str) -> list[str]:
    selection = selection.strip()
    if not selection:
        return []
    if selection.lower() in {"q", "quit", "exit"}:
        raise KeyboardInterrupt

    selectors: list[str] = []
    for part in selection.split(","):
        item = part.strip()
        if not item:
            continue
        if "-" in item:
            start_text, end_text = [piece.strip() for piece in item.split("-", 1)]
            if start_text.isdigit() and end_text.isdigit():
                start = int(start_text)
                end = int(end_text)
                step = 1 if end >= start else -1
                selectors.extend(str(index) for index in range(start, end + step, step))
                continue
        selectors.append(item)
    return selectors


def prompt_for_outputs(
    speakers: list[OutputTarget],
    source_name: str,
    include_source_output: bool,
) -> list[str]:
    visible = [
        target
        for target in speakers
        if include_source_output or target.name != source_name
    ]

    if not visible:
        raise ValueError("No selectable playback outputs are active.")

    print("Select output devices to fan out to:")
    for target in visible:
        print(f"  [{target.index}] {target.name}")
    print()
    print("Enter indexes or name fragments separated by commas. Example: 0,2,Soundcore")
    print("Use ranges like 0-2. Enter q to quit.")

    while True:
        selection = input("Outputs> ")
        selectors = parse_output_selection(selection)
        if selectors:
            return selectors
        print("No outputs selected.")


def select_outputs(
    selectors: list[str],
    include_default: bool,
    source_name: str | None = None,
    interactive: bool = False,
    all_outputs: bool = False,
) -> list[OutputTarget]:
    speakers = active_speakers()
    default_name = device_name(sc.default_speaker())
    selected: list[OutputTarget] = []

    def add(target: OutputTarget) -> None:
        if is_maf_endpoint(target.name):
            raise ValueError("Refusing to fan out into the MAF virtual output; choose physical outputs only.")
        if target.name not in {item.name for item in selected}:
            selected.append(target)

    if not selectors and interactive:
        selectors = prompt_for_outputs(
            speakers,
            source_name=source_name or "",
            include_source_output=include_default,
        )

    if selectors:
        for selector in selectors:
            if selector.isdigit():
                index = int(selector)
                try:
                    add(speakers[index])
                except IndexError as exc:
                    raise ValueError(f"No playback output at index {index}.") from exc
                continue

            needle = selector.lower()
            matches = [target for target in speakers if needle in target.name.lower()]
            if not matches:
                raise ValueError(f"No playback output matching {selector!r}.")
            for match in matches:
                add(match)
    elif all_outputs:
        for target in speakers:
            blocked_names = {default_name}
            if source_name:
                blocked_names.add(source_name)
            if is_maf_endpoint(target.name):
                continue
            if include_default or target.name not in blocked_names:
                add(target)
    else:
        raise ValueError("No outputs selected. Use --output, run interactively, or pass --all.")

    return selected


class PlaybackWorker(threading.Thread):
    def __init__(self, target: OutputTarget, stop_event: threading.Event) -> None:
        super().__init__(name=f"playback:{target.index}", daemon=True)
        self.target = target
        self.stop_event = stop_event
        self.frames: queue.Queue[np.ndarray | None] = queue.Queue(maxsize=QUEUE_BLOCKS)
        self.dropped = 0
        self.error: BaseException | None = None

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

    def run(self) -> None:
        try:
            with self.target.device.player(
                samplerate=SAMPLE_RATE,
                channels=2,
                blocksize=BLOCK_FRAMES,
            ) as player:
                while not self.stop_event.is_set():
                    block = self.frames.get()
                    if block is None:
                        break
                    player.play(block)
        except BaseException as exc:
            self.error = exc
            self.stop_event.set()


def normalize_block(block: np.ndarray) -> np.ndarray:
    if block.ndim == 1:
        block = block.reshape((-1, 1))
    if block.shape[1] == 1:
        block = np.repeat(block, 2, axis=1)
    elif block.shape[1] > 2:
        block = block[:, :2]
    return np.asarray(block, dtype=np.float32)


def load_config(path: str | None) -> dict:
    if not path:
        return {}
    with open(path, "r", encoding="utf-8") as config_file:
        data = json.load(config_file)
    if not isinstance(data, dict):
        raise ValueError("Config file must contain a JSON object.")
    return data


def run_fanout(
    source_selector: str | None,
    source_kind: str,
    selectors: list[str],
    include_default: bool,
    seconds: float | None,
    interactive: bool,
    all_outputs: bool,
) -> int:
    source = find_source(source_selector, source_kind)
    outputs = select_outputs(
        selectors,
        include_default=include_default,
        source_name=device_name(source),
        interactive=interactive,
        all_outputs=all_outputs,
    )
    if not outputs:
        print("No playback outputs selected.", file=sys.stderr)
        return 2

    stop_event = threading.Event()

    def stop(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    workers = [PlaybackWorker(target, stop_event) for target in outputs]
    for worker in workers:
        worker.start()

    print(f"Capturing: {device_name(source)}")
    print("Sending to:")
    for target in outputs:
        print(f"  [{target.index}] {target.name}")
    print("Press Ctrl+C to stop.")

    blocks = 0
    started = time.monotonic()
    try:
        with source.recorder(
            samplerate=SAMPLE_RATE,
            channels=2,
            blocksize=BLOCK_FRAMES,
        ) as recorder:
            while not stop_event.is_set():
                if seconds is not None and time.monotonic() - started >= seconds:
                    break
                block = normalize_block(recorder.record(numframes=BLOCK_FRAMES))
                for worker in workers:
                    worker.push(block.copy())
                blocks += 1

                if blocks % 250 == 0:
                    elapsed = max(time.monotonic() - started, 0.001)
                    dropped = sum(worker.dropped for worker in workers)
                    print(
                        f"status: {blocks * BLOCK_FRAMES / SAMPLE_RATE:.1f}s audio, "
                        f"{blocks / elapsed:.1f} blocks/s, dropped={dropped}",
                        flush=True,
                    )
    finally:
        stop_event.set()
        for worker in workers:
            worker.close()
        for worker in workers:
            worker.join(timeout=2)

    failed = [worker for worker in workers if worker.error is not None]
    if failed:
        for worker in failed:
            print(f"Output failed: {worker.target.name}: {worker.error}", file=sys.stderr)
        return 1

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fan out the current Windows default output loopback to N playback devices."
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List active playback and loopback devices.",
    )
    parser.add_argument(
        "--source",
        default=None,
        help=(
            "Capture source index or name substring. Use this for a virtual cable "
            "recording endpoint such as 'Cable Output'. Defaults to current "
            "Windows output loopback."
        ),
    )
    parser.add_argument(
        "--source-kind",
        choices=("any", "capture", "loopback"),
        default=None,
        help=(
            "Restrict --source to physical/virtual capture endpoints or loopback "
            "endpoints. Use 'capture' for Virtual Audio Cable's recording side."
        ),
    )
    parser.add_argument(
        "--output",
        action="append",
        default=[],
        help="Playback output index or name substring. Repeat for multiple outputs.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Fan out to every active playback output except the source/default device.",
    )
    parser.add_argument(
        "--include-default",
        action="store_true",
        help="When no --output is provided, include the default output too.",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="Stop automatically after this many seconds.",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="Read source/output selections from a JSON config file.",
    )
    parser.add_argument(
        "--no-interactive",
        action="store_true",
        help="Do not prompt for output selection when no outputs are configured.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    config = load_config(args.config)
    if args.list:
        print_devices()
        return 0

    try:
        source = args.source or config.get("source")
        source_kind = args.source_kind or config.get("source_kind", "any")
        outputs = args.output or config.get("outputs") or []
        include_default = args.include_default or bool(config.get("include_default", False))
        all_outputs = args.all or bool(config.get("all", False))
        interactive = not args.no_interactive and not all_outputs and not outputs
        return run_fanout(
            source_selector=source,
            source_kind=source_kind,
            selectors=outputs,
            include_default=include_default,
            seconds=args.seconds,
            interactive=interactive,
            all_outputs=all_outputs,
        )
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
