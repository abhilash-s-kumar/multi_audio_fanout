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

Easiest: open MAF and click **Driver**. It checks whether the driver is installed, up to date
and working, shows the manual steps, and installs or updates it (Windows asks for administrator
permission). MAF opens this window by itself when the driver is missing.

Manually, open PowerShell as Administrator:

```powershell
cd <path to multi_audio_fanout>
powershell -ExecutionPolicy Bypass -File .\install_maf_driver.ps1
```

- If the driver is already installed, the script updates the existing MAF device in place
  (it never adds a second one).
- If it enables Windows test-signing, restart and run the same command again.
- If Secure Boot is on, it stops and says so: turn off Secure Boot in the BIOS/UEFI setup,
  start Windows, then run it again.
- Exit codes: 0 done, 3010 restart needed, 4 blocked by Secure Boot, 1 failed.

Other options:

```powershell
.\install_maf_driver.ps1 -Clean              # remove every MAF device and driver, then install
.\install_maf_driver.ps1 -Status             # driver state as JSON, no admin needed
.\install_maf_driver.ps1 -InstallPythonDeps  # also install Python packages for the .py scripts
```

## Build the driver

The driver is SysVAD's TabletAudioSample with MAF changes. Its loopback output (what MAF captures)
is a real mixer: every playback stream, such as music, Windows alert sounds or calls, is added
into one 48 kHz timeline, other sample rates are converted, and the loopback reads it 40 ms behind
real time. Before this, streams were appended one after another, so any system sound mangled the
music and added lag that never went away.

```powershell
powershell -ExecutionPolicy Bypass -File .\build_driver.ps1
```

- Needs Visual Studio 2022 Build Tools (C++ x64) and the WDK/SDK NuGet packages in `packages\`.
  The full WDK is not needed.
- Builds `TabletAudioSample.sys`, stamps a new DriverVer into the INF, makes the catalog and
  signs both with the "MAF Test Driver Signing" certificate, all into `maf_sysvad_package\`.
- The driver source changes are in `driver\sysvad_maf.patch` (against `Windows-driver-samples`)
  plus `driver\Directory.Build.props/.targets` for `audio\sysvad`.
- `driver\mixer_test\run_mixer_test.ps1` runs the mixer code in user mode with AddressSanitizer
  (mixing, rate conversion, pause/resume, constant delay, fuzzing). Run it after changing the mixer.

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

- master volume (follows the Windows volume of the MAF output)
- each speaker's own volume: for Bluetooth speakers with absolute volume this is the speaker's
  internal volume, so the slider moves when you press the speaker's buttons and the other way round
- per-output delay in milliseconds
- Calibrate: measures each speaker's delay with the laptop mic, then evens out their loudness at
  the mic by turning the louder speakers down (re-checked with short sweeps until within 1 dB)

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
