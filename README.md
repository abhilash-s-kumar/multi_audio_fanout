# Multi Audio Fanout

Prototype Windows audio fan-out engine.

It can work in two modes:

- Capture `Speakers (MAF Virtual Audio Output)` through WASAPI loopback.
- Capture the current default Windows playback device through WASAPI loopback.

The second mode is the VoiceMeeter-style architecture:

```text
Windows apps
  -> Speakers (MAF Virtual Audio Output), selected in Windows
  -> Matching MAF loopback source selected in this app
  -> N selected speakers/headphones/Bluetooth outputs
```

## Setup

```powershell
python -m pip install --user soundcard numpy pycaw comtypes
```

This is only needed if you want to run the `.py` files directly. The portable
package includes `MAF.exe` for normal use.

## Install MAF Virtual Driver

Open PowerShell as Administrator:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
cd C:\Windows\System32\multi_audio_fanout
.\install_maf_driver.ps1
```

If the script enables Windows test-signing, reboot and run the same command
again. If `bcdedit` reports that Secure Boot policy blocks the change, disable
Secure Boot in BIOS/UEFI first, boot Windows, then rerun the installer.

For a clean reinstall:

```powershell
.\install_maf_driver.ps1 -Clean
```

To also install Python packages for source-script usage:

```powershell
.\install_maf_driver.ps1 -InstallPythonDeps
```

## List Devices

```powershell
python .\fanout.py --list
```

## GUI

```powershell
.\MAF.exe
```

For development:

```powershell
python .\maf_gui.py
```

The GUI scans active playback devices, lets you select exactly which outputs
receive MAF audio, and provides:

- master volume
- per-output volume
- per-output delay in milliseconds

For sync correction, delay the devices that are playing early. A device that is
already trailing cannot be made earlier in software; the other devices must be
delayed to match it.

## Run

Start the app and choose output devices interactively:

```powershell
python .\fanout.py
```

Send to specific devices by index or name substring:

```powershell
python .\fanout.py --output 0 --output "Realtek"
```

Send to all active devices only when explicitly requested:

```powershell
python .\fanout.py --all
```

Use the MAF virtual output as the source:

```powershell
python .\fanout.py --source "Speakers (MAF Virtual Audio Output)" --source-kind loopback
```

Use a config file:

```powershell
copy .\fanout.config.example.json .\fanout.config.json
python .\fanout.py --config .\fanout.config.json
```

Include the current default device as one of the mirrored outputs:

```powershell
python .\fanout.py --include-default
```

Run a short bounded test:

```powershell
python .\fanout.py --seconds 5
```

## Notes

- Windows must expose a target as an active stereo playback endpoint before the
  app can use it.
- For the virtual-output workflow, set the Windows default output device to
  `Speakers (MAF Virtual Audio Output)`. The installer calls
  `set_maf_default.py` automatically when Python dependencies are present.
- If no `--output` values are supplied, the app prompts you to select outputs.
  It will not blindly fan out to every connected device unless `--all` is used.
- Bluetooth "Hands-Free" endpoints are call-profile devices and are not suitable
  for music fan-out.
- The test-signed driver requires Windows test mode. Secure Boot must stay off
  for this development build. A Microsoft-signed production driver is required
  for normal Secure Boot systems.
