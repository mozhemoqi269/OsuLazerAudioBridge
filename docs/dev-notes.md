# Development Notes

This file is internal working documentation. It is not intended as the public
GitHub README.

## Current Milestone

ASIO-first bridge prototype.

Current state:

- Keep the official osu!lazer client unchanged.
- Inject a native x64 hook DLL into the osu!lazer process.
- Hook selected BASS and BASSmix exports.
- Publish audio and probe events through a shared-memory channel.
- Decode captured sample payloads in the external host process.
- Mirror audio to XAudio2, WASAPI Exclusive, or ASIO.
- Expose the host through both CLI and the Win32 GUI launcher.

Effects mirroring is the primary supported path. Music mirroring exists, but is
still more experimental and backend-sensitive.

The earlier probe-only milestone is complete. The notes below are still useful
for trace capture, decoding, and low-level debugging.

## Project Boundaries

The project is audio-output research only.

- No network behavior.
- No input injection.
- No score or judgement manipulation.
- No gameplay assist overlay.
- No anti-detection, hiding, or bypass work.

The purpose is to reduce osu!lazer audio output latency while leaving the
official game client behavior intact.

## Build

Requires Visual Studio or Visual Studio Build Tools with the Desktop C++
workload.

```powershell
.\scripts\build-native.ps1
```

Artifacts are placed under:

```text
build/bin/
```

The script uses `vswhere` and `vcvars64.bat`, then configures CMake with
`NMake Makefiles`. This also works with Visual Studio versions newer than the
generators known by the installed CMake.

## Probe Run

Start osu!lazer first, then run:

```powershell
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe"
```

The host creates the shared-memory event channel, injects
`OsuLazerBassHook.dll`, then prints hook/install and BASS event records.

Use `--pid <id>` when multiple osu processes are present.

By default the host filters very high-frequency attribute/position events.
Use `--verbose` to print the full raw probe stream:

```powershell
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --verbose
```

The host automatically writes a UTF-8 probe log under `logs/`:

```text
logs/probe-YYYYMMDD-HHMMSS.log
```

Useful logging flags:

```powershell
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --log-dir .\artifacts\probe-logs
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --log .\artifacts\last-probe.log
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --no-log
```

To verify that memory-loaded BASS samples are copied into the host-readable
shared blob area, dump raw sample payloads:

```powershell
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --dump-samples .\artifacts\sample-dumps
```

Decode copied samples with vendored decoders and mirror inferred playback
through XAudio2:

```powershell
.\build\bin\OsuLazerAudioHost.exe --process "osu!.exe" --mirror-audio
```

Current vendored decoders cover the observed lazer sample containers:

- Ogg/Vorbis: `stb_vorbis`
- WAV: `dr_wav`
- MP3: `dr_mp3`

`--mirror-audio` currently decodes and caches PCM samples, then plays inferred
sample events through XAudio2. The actual ASIO mixer/output stage is the next
step.

Offline decoder validation:

```powershell
.\build\bin\OsuLazerAudioHost.exe --decode-dir .\artifacts\sample-dumps
```

Observed on the first captured lazer play session:

- 18 dumped memory samples.
- 18/18 decoded successfully.
- Containers observed: Ogg/Vorbis, WAV, MP3.
- Sample rates observed: 44.1 kHz, 48 kHz, 96 kHz, 192 kHz.
- Channel layouts observed: mono and stereo.

The host also derives `InferredSamplePlayback` lines from this observed lazer
path:

```text
BASS_SampleGetChannel(sample -> channel)
  -> BASS_Mixer_StreamAddChannel(mixer, channel)
```

These derived lines are the current best candidate trigger points for the
future external low-latency renderer.

## Current Hook Points

- `BASS_SampleLoad`
- `BASS_SampleCreate`
- `BASS_StreamCreateFile`
- `BASS_SampleGetChannel`
- `BASS_ChannelPlay`
- `BASS_ChannelStop`
- `BASS_ChannelSetAttribute`
- `BASS_ChannelSetPosition`
- `BASS_Mixer_StreamCreate` when `bassmix.dll` is already loaded
- `BASS_Mixer_StreamAddChannel` when `bassmix.dll` is already loaded
- `BASS_Mixer_StreamRemoveChannel` when `bassmix.dll` is already loaded

The probe may need additional BASSmix hooks after real lazer traces are
captured.
