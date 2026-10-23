"""MAF virtual audio driver: status check and install, used by the GUI.

The work is done by install_maf_driver.ps1 next to MAF.exe: `-Status` reports the driver
state as JSON without admin rights, and a normal run (elevated) installs or updates it.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import tempfile
from ctypes import wintypes
from dataclasses import dataclass, field
from pathlib import Path

INSTALL_SCRIPT = "install_maf_driver.ps1"
PACKAGE_DIR = "maf_sysvad_package"
STATUS_TIMEOUT_SECONDS = 30

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_SECURE_BOOT = 4
EXIT_RESTART = 3010
EXIT_CANCELLED = 1223  # ERROR_CANCELLED: the UAC prompt was declined

OK = "ok"
WARNING = "warning"
ERROR = "error"


def driver_root() -> Path:
    """Folder holding install_maf_driver.ps1 and maf_sysvad_package (beside MAF.exe)."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def install_script() -> Path:
    return driver_root() / INSTALL_SCRIPT


def install_log_path() -> Path:
    return Path(tempfile.gettempdir()) / "MAF" / "driver_install.log"


def _powershell_args(*script_args: str) -> list[str]:
    # -ExecutionPolicy Bypass applies to this one process only.
    return ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(install_script()), *script_args]


def query_status() -> dict | None:
    """Runs the installer in -Status mode. Returns None if the check itself could not run."""
    if not install_script().exists():
        return None
    try:
        result = subprocess.run(
            _powershell_args("-Status"),
            capture_output=True,
            text=True,
            timeout=STATUS_TIMEOUT_SECONDS,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in reversed(result.stdout.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                return None
    return None


@dataclass
class DriverSummary:
    level: str                      # OK, WARNING or ERROR
    headline: str
    action: str | None = None       # "install", "update", "clean", "restart" or None
    details: list[str] = field(default_factory=list)
    steps: list[str] = field(default_factory=list)


def manual_steps(root: Path, clean: bool = False) -> list[str]:
    command = f'powershell -ExecutionPolicy Bypass -File .\\{INSTALL_SCRIPT}' + (" -Clean" if clean else "")
    return [
        "Open the Start menu, type PowerShell, right-click Windows PowerShell and choose Run as administrator.",
        f'Run:  cd "{root}"',
        f"Run:  {command}",
        "If it says to restart Windows, restart, then run the same command again.",
        "Stop and Start MAF afterwards if it was playing.",
    ]


def summarize(status: dict | None, root: Path | None = None) -> DriverSummary:
    """Turns the -Status JSON into a verdict, the button to offer, and manual steps."""
    root = root or driver_root()
    if status is None:
        if not (root / INSTALL_SCRIPT).exists():
            return DriverSummary(ERROR, f"{INSTALL_SCRIPT} is missing next to MAF.exe.", None,
                                 [f"Expected it in {root}."])
        return DriverSummary(ERROR, "Could not check the driver (PowerShell did not answer).", "install",
                             ["The automatic install may still work."], manual_steps(root))

    details = [
        f"Playback device: {'present' if status.get('endpoint_present') else 'missing'}"
        + (f" ({status.get('endpoint_status')})" if status.get("endpoint_status") else ""),
        f"Installed driver: {status.get('installed_version') or 'none'}",
        f"Driver in this folder: {status.get('package_version') or 'missing'}"
        + (f" ({status.get('package_date')})" if status.get("package_date") else ""),
        f"Windows test-signing: {'on' if status.get('test_signing') else 'off'}",
        f"Secure Boot: {'on' if status.get('secure_boot') else 'off'}",
        f"MAF certificate trusted: {'yes' if status.get('cert_trusted') else 'no'}",
    ]
    device_count = int(status.get("device_count") or 0)
    blocked = bool(status.get("secure_boot")) and not status.get("test_signing")
    secure_boot_steps = [
        "Restart into the BIOS/UEFI setup (usually F2, F10, Del or Esc while the laptop starts).",
        "Turn Secure Boot off, save, and start Windows.",
    ]

    if not status.get("package_present"):
        if device_count:
            return DriverSummary(WARNING, "The driver is installed, but its package is missing from this folder.",
                                 None, details, [f"Keep the {PACKAGE_DIR} folder next to MAF.exe to reinstall or update."])
        return DriverSummary(ERROR, f"The driver is not installed and {PACKAGE_DIR} is missing next to MAF.exe.",
                             None, details)

    if device_count == 0:
        if blocked:
            return DriverSummary(ERROR, "The MAF driver is not installed, and Secure Boot blocks it.", "install",
                                 details, secure_boot_steps + manual_steps(root))
        return DriverSummary(ERROR, "The MAF driver is not installed.", "install", details, manual_steps(root))

    if device_count > 1:
        return DriverSummary(WARNING, f"{device_count} MAF devices are installed; there should be one.", "clean",
                             details, manual_steps(root, clean=True))

    if status.get("reboot_required"):
        return DriverSummary(WARNING, "Restart Windows to finish the driver update.", "restart", details,
                             ["Restart Windows. The new driver loads at startup."])

    if status.get("update_available"):
        return DriverSummary(WARNING, f"A newer driver is available ({status.get('installed_version')} → "
                                      f"{status.get('package_version')}).", "update", details, manual_steps(root))

    if not status.get("endpoint_present") or status.get("device_status") != "OK":
        steps = (secure_boot_steps if blocked else []) + manual_steps(root, clean=True)
        return DriverSummary(ERROR, "The MAF driver is installed but not working.", "clean", details, steps)

    return DriverSummary(OK, f"MAF driver {status.get('installed_version')} is installed and working.", None, details)


def exit_code_message(code: int) -> tuple[str, str]:
    """Level and text for an installer exit code."""
    if code == EXIT_OK:
        return OK, "The driver is installed."
    if code == EXIT_RESTART:
        return WARNING, "Restart Windows. If the driver was not installed yet, run Install again after the restart."
    if code == EXIT_SECURE_BOOT:
        return ERROR, "Secure Boot blocks test-signed drivers. Turn Secure Boot off in the BIOS/UEFI setup, then try again."
    if code == EXIT_CANCELLED:
        return WARNING, "Install cancelled (administrator permission was not given)."
    return ERROR, f"The install failed (code {code}). See the log below."


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("fMask", ctypes.c_ulong),
        ("hwnd", wintypes.HWND),
        ("lpVerb", wintypes.LPCWSTR),
        ("lpFile", wintypes.LPCWSTR),
        ("lpParameters", wintypes.LPCWSTR),
        ("lpDirectory", wintypes.LPCWSTR),
        ("nShow", ctypes.c_int),
        ("hInstApp", wintypes.HINSTANCE),
        ("lpIDList", ctypes.c_void_p),
        ("lpClass", wintypes.LPCWSTR),
        ("hkeyClass", wintypes.HKEY),
        ("dwHotKey", wintypes.DWORD),
        ("hIconOrMonitor", wintypes.HANDLE),
        ("hProcess", wintypes.HANDLE),
    ]


def run_install_elevated(clean: bool = False) -> tuple[int, str]:
    """Runs the installer as administrator (UAC prompt) and waits. Returns (exit code, log text)."""
    log_path = install_log_path()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        log_path.unlink()
    except OSError:
        pass

    args = _powershell_args(*(["-Clean"] if clean else []), "-LogPath", str(log_path))[1:]
    parameters = subprocess.list2cmdline(args)

    see_mask_nocloseprocess = 0x00000040
    see_mask_noasync = 0x00000100
    sw_shownormal = 1
    info = _ShellExecuteInfo()
    info.cbSize = ctypes.sizeof(info)
    info.fMask = see_mask_nocloseprocess | see_mask_noasync
    info.lpVerb = "runas"
    info.lpFile = "powershell.exe"
    info.lpParameters = parameters
    info.lpDirectory = str(driver_root())
    info.nShow = sw_shownormal

    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(_ShellExecuteInfo)]
    shell32.ShellExecuteExW.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    if not shell32.ShellExecuteExW(ctypes.byref(info)):
        error = ctypes.get_last_error()
        if error == EXIT_CANCELLED:
            return EXIT_CANCELLED, ""
        return EXIT_FAILED, f"Could not start PowerShell as administrator (error {error})."
    if not info.hProcess:
        return EXIT_FAILED, "PowerShell started without a process handle."

    try:
        kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
        code = wintypes.DWORD()
        kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
    finally:
        kernel32.CloseHandle(info.hProcess)
    return int(code.value), read_install_log(log_path)


def read_install_log(path: Path | None = None) -> str:
    path = path or install_log_path()
    try:
        raw = path.read_bytes()
    except OSError:
        return ""
    # Start-Transcript writes UTF-8 with BOM on Windows PowerShell 5, UTF-16 on some systems.
    for encoding in ("utf-8-sig", "utf-16"):
        try:
            text = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = raw.decode("latin-1", errors="replace")
    lines = [line for line in text.splitlines() if not line.startswith("*****") and not _is_transcript_header(line)]
    return "\n".join(lines).strip()


def _is_transcript_header(line: str) -> bool:
    if line.startswith("Windows PowerShell transcript ") or line.startswith("PowerShell transcript "):
        return True
    return line.split(":", 1)[0] in {
        "Start time", "End time", "Username", "RunAs User", "Configuration Name", "Machine",
        "Host Application", "Process ID", "PSVersion", "PSEdition", "PSCompatibleVersions",
        "BuildVersion", "CLRVersion", "WSManStackVersion", "PSRemotingProtocolVersion",
        "SerializationVersion", "Transcript started, output file is",
    }


if __name__ == "__main__" and os.name == "nt":
    summary = summarize(query_status())
    print(summary.level, summary.headline)
    for line in summary.details + summary.steps:
        print(" ", line)
