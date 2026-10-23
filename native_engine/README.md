# MAF Native Engine

This is the first C++/WASAPI backend prototype for MAF. It is intended to move the real-time audio path out of Python while keeping the existing Python GUI.

## Current Scope

- Lists active render endpoints.
- Captures loopback audio from the MAF virtual render endpoint.
- Renders the captured audio to one or more selected render endpoints.
- Registers the engine thread with Windows MMCSS using the `Audio` task profile.
- Uses each output device's own shared-mode mix format and converts/resamples
  from the MAF source stream when needed.
- Supports per-output channel mode:
  - `stereo` sends `[L, R]`
  - `left` sends `[L, L]`
  - `right` sends `[R, R]`
- Supports per-output gain in the command line.

## Build

Install Visual Studio 2022 Build Tools with C++ desktop development, then from a Developer PowerShell:

```powershell
cd E:\multi_audio_fanout\native_engine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output:

```powershell
E:\multi_audio_fanout\native_engine\build\Release\maf_native_engine.exe
```

## Test

List endpoints:

```powershell
.\build\Release\maf_native_engine.exe --list
```

Run a manual fanout:

```powershell
.\build\Release\maf_native_engine.exe --source "MAF Virtual Audio Output" --out "Motion Boom:left:1.0" --out "X600:right:1.0"
```

Stop it with `Ctrl+C`.

Run a bounded test:

```powershell
.\build\Release\maf_native_engine.exe --source "MAF Virtual Audio Output" --out "Motion Boom:left:1.0" --out "X600:right:1.0" --duration 20
```

## Notes

This prototype expects the MAF source mix format to be 32-bit float stereo. That matches the intended MAF endpoint format. Output devices may use different shared-mode mix formats; the engine converts for those endpoints.

This is not wired into the Python GUI yet. The intended integration is for the GUI to launch this helper process with the selected devices, volumes, and channel modes, then read status from stdout.
