# MAF Native Engine

C++/WASAPI backend for MAF. The Python GUI launches it, sends it live commands on stdin and
reads its status from stdout.

## How sync works

The MAF virtual device and every Bluetooth speaker run on their own clocks, which differ by
tens to hundreds of ppm (two speakers measured 150 ppm apart, about 9 ms per minute). Without
correction each speaker's buffer slowly fills or drains and the speakers drift apart.

- The capture thread reads loopback audio from the MAF endpoint and pushes it into one ring
  buffer per output.
- Each output has its own event-driven render thread on the device's clock.
- Each output measures its latency (ring buffer + device buffer) and a PI controller trims its
  playback rate by up to 1000 ppm (inaudible) to hold `target latency + delay offset`.
  - Preferred mode `rateadjust`: Windows' own resampler applies the trim
    (`AUDCLNT_STREAMFLAGS_RATEADJUST` + `IAudioClockAdjustment`).
  - Fallback mode `resample`: the device's mix format with a stateful cubic resampler.
- Errors above 25 ms (for example after a long Bluetooth hiccup) are fixed at once by skipping
  audio or inserting silence, with a short fade-in.
- When audio starts after silence, a reopen or a resync, every output is placed exactly on its
  target, so all speakers start together.
- A speaker that disconnects or fails is reopened by itself (by endpoint id) without stopping
  the others. A lost source is reopened as well.

Delays are relative: the output with the smallest delay gets offset 0, and the others wait the
difference. Delays only correct differences inside the speakers (codec and speaker buffering),
which the PC cannot measure.

## Mic calibration

The PC cannot see delays inside the speakers (Bluetooth stack, codec, speaker buffer, distance).
`calibrate` measures them: it mutes the music for about 12 s, plays a 0.4 s sweep on one speaker
at a time (3 rounds) and records with a microphone. Each speaker's delay is the time from the
sweep leaving Windows to the sweep reaching the mic. The engine then sets the delays so that
every speaker matches the slowest one. Put the laptop where you listen; the distance to each
speaker counts.

A broken mic is never blamed on a speaker:

1. Before any sweep plays, the mic is checked and calibration stops with a `MIC_*` error if:
   - there is no usable mic (`MIC_MISSING`, `MIC_UNAVAILABLE` disabled/unplugged/driver not
     loaded, `MIC_ONLY_BLUETOOTH`);
   - microphone privacy blocks desktop apps (`MIC_PRIVACY_BLOCKED`) or the mic is muted (`MIC_MUTED`);
   - the driver will not start (`MIC_DRIVER_ERROR`, `MIC_IN_USE`);
   - it delivers nothing, at the wrong speed, or garbage (`MIC_NO_DATA`, `MIC_CLOCK_BAD`,
     `MIC_SILENT`, `MIC_STUCK`, `MIC_CLIPPING`).
   The same checks run again over the sweep period, in case the mic died midway.
2. A speaker is reported `not_heard` only if the same run heard another speaker. If no speaker
   is heard the result is `MIC_HEARD_NOTHING` (mic level did not change) or
   `MIC_SOUND_UNRECOGNISED` (level rose but no sweep was recognised, usually Windows mic effects),
   never a speaker fault.
3. Delays are only applied when every speaker was heard at least twice with results within 5 ms.

Mic choice: Windows' default mic, else the built-in mic, else any usable one. Bluetooth mics
(including a speaker's call mic on another bus, found through its device container), Stereo Mix
and virtual inputs are skipped, because a Bluetooth mic would switch that speaker to call quality.
The mic is opened in raw mode where the driver allows it, to bypass noise suppression.
`--mic <name-or-id>` (GUI: `"calibration_mic"` in settings.json) picks one explicitly.

`miccheck` runs only the mic checks, without muting the music. `--list` shows every mic with its
state and whether it is Bluetooth. `--selftest` checks detection and mic verdicts offline with
synthetic recordings.

Results are `cal {json}` lines: `start`, `mic`, `warning`, `mic_ok`, `playing`, `dev` (per output:
`status` ok / unstable / not_heard / not_receiving / unknown, `latency_ms`, `spread_ms`), `delay`,
then `done` or `fail` with `code` and `detail`.

## Build

Install Visual Studio 2022 Build Tools with C++ desktop development, then:

```powershell
cd <path to multi_audio_fanout>\native_engine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\maf_native_engine.exe .
```

## Usage

```powershell
.\maf_native_engine.exe --list
.\maf_native_engine.exe --source "MAF Virtual Audio Output" --out "Motion Boom|left|1.0|0" --out "X600|right|1.0|35"
```

`--out <name-or-endpoint-id>|<stereo|left|right>|<gain>|<delay_ms>`. Trailing fields are
optional. The old `name:mode:gain` form still works for names without `:` or `|`.

Options:

- `--master <gain>`
- `--latency-ms <ms>`: shared target latency, 40 to 1000 (default 100).
- `--no-sync`: disable drift correction.
- `--exit-on-stdin-eof`: quit when the controlling process goes away (the GUI uses this).
- `--duration <seconds>`: bounded test run.

## stdin commands

```text
master <gain>
gain <index> <gain>
mode <index> <stereo|left|right>
delay <index> <ms>
latency <ms>
sync <0|1>
resync        realign every output in place (no device reopen)
calibrate     measure speaker delays with the mic and apply them
miccheck      check the mic only
quit
```

## Status output (UTF-8, one line each, every second)

```text
src state=running packets=1234 glitches=0
dev 0 state=locked lat=100.4 target=100.0 err=+0.4 ppm=48 under=0 snaps=0 over=0 mode=rateadjust
```

States: `opening`, `waiting` (device not connected), `idle` (no audio), `settling`, `locked`,
`free` (sync off), `stopped`. `ppm` is the rate trim currently applied. `under` counts buffer
underruns, `snaps` counts hard corrections and `over` counts frames dropped because the buffer
was full.
