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

## Build

Install Visual Studio 2022 Build Tools with C++ desktop development, then:

```powershell
cd E:\multi_audio_fanout_v2\native_engine
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
