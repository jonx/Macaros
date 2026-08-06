# CoreAudio Audio

Status: **implemented and audibly proven through AHI** (2026-06-28).

This feature is the Darwin hosted audio path. AROS uses its existing
retargetable AHI mixer; the new Darwin piece is a `CoreAudio` AHI sub-driver
that pulls `AHIST_S16S` mixed PCM from AHI and pushes it into the Mac-side
CoreAudio shim through `hostlib.resource`.

Implemented pieces:

- Mac host shim: `hosted/coreaudio/coreaudio_shim.c`,
  `hosted/coreaudio/coreaudio_shim.h`, `hosted/coreaudio/coreaudio.exports`,
  `build/libcoreaudio.dylib`
- AROS AHI sub-driver:
  `../aros-upstream/workbench/devs/AHI/Drivers/CoreAudio/`
- AHI mode file: `DEVS:AudioModes/COREAUDIO`
- AROS playback smoke command: `C:AHISmoke`
- host harness: `graft/audio-smoke` / `make audio-smoke`

## What Is Proven

Host-only checks:

```sh
make hosted-coreaudio
make coreaudio-abi
```

`hosted-coreaudio` compiles the shim directly into the test binary. `coreaudio-abi`
builds `build/libcoreaudio.dylib`, links the same test through the dylib boundary,
and verifies the exported ABI surface.

Both tests render a 5-second 440 Hz sine through the shim's SPSC ring into an
offline CoreAudio GenericOutput unit, write `run/coreaudio-a.wav`, read it back,
and assert RMS, dominant frequency, frame count, zero underruns, and zero
RT-thread AROS calls. They also exercise `ca_set_global_volume(50)` with a short
render to `run/coreaudio-volume50.wav` and assert the RMS is halved. These are
headless and silent.

End-to-end audible check:

```sh
make audio-smoke
```

The harness runs `make coreaudio-abi`, deploys `libcoreaudio.dylib`, verifies the
AROS audio artifacts with `graft/deploy-check`, boots windowed AROS with a
temporary startup file, registers `DEVS:AudioModes/COREAUDIO`, and runs
`C:AHISmoke`. The host log must show:

- `CoreAudio] ca_start live=1`
- `CoreAudio] first ring push`
- `CoreAudio] ca_stop pushed=... underruns=0`

It saves the screenshot under `run/darwin-aarch64/audio-smoke-<timestamp>.png`.
The user also confirmed the tone was audible through the Mac speaker path.

Normal launcher startup now also prepares the audio mode:

- console mode creates/assigns `T:` to `RAM:T`, registers
  `DEVS:AudioModes/COREAUDIO`, then starts `ConClip`
- desktop mode creates/assigns `T:` and starts `AddAudioModes` in a quiet
  background task before Wanderer, so heavier AHI database work does not block
  or destabilize the compact desktop startup path

## Important Notes

`C:Play MacRW:coreaudio-a.wav` is not the validator for this port right now.
`Play` goes through sound DataTypes, and this image currently cannot open that
WAV as a DataTypes object. `C:AHISmoke` talks directly to `ahi.device`, so it
tests the actual CoreAudio AHI path without a DataTypes dependency.

The AHI mode file and driver package versions must match. `AddAudioModes` opens
the sub-driver at AHI's driver version, so `coreaudio.audio` must advertise
version 6; an earlier version-1 driver loaded from disk but could not be opened
by AHI, making `AHI_AllocAudio` fail.

## Deployment

`graft/aros-ctl deploy`, `graft/aros-ctl run`, and `graft/run-window.sh` copy
`build/libcoreaudio.dylib` to `~/lib/libcoreaudio.dylib` when it exists. Use
`graft/aros-ctl deploy` before `graft/deploy-check` when you want to verify a
fresh build without launching. `graft/deploy-check` reports source/destination
hashes for stale-build checks and now also checks:

- `DEVS:ahi.device`
- `C:AddAudioModes`
- `DEVS:AHI/coreaudio.audio`
- `DEVS:AudioModes/COREAUDIO`
- `C:AHISmoke`

`graft/make-aros-app.sh` bundles `libcoreaudio.dylib` in
`Macaros.app/Contents/Frameworks/` when the build artifact exists; its
`--check` mode verifies the bundled dylib and signature.

## App Shell Volume

The native Settings audio-volume control is wired as host-owned global CoreAudio
gain. `cm_set_option(CM_OPT_AUDIO_VOLUME, percent)` clamps 0..100, calls
`ca_set_global_volume()` in `libcoreaudio.dylib`, and still mirrors the request
to AROS as a `CM_EV_SETTING` for logging/UI visibility. It is deliberately not
an AHI mixer-control path yet; AROS produces PCM, and the Mac host applies the
final output gain.

## Build (first-class since 2026-07-13)

The AHI audio build is now part of the standard rebuild — no manual rescue.
`graft/rebuild-aros.sh` builds it (target group `AUDIO_TARGETS`), or by hand:

```sh
cd ~/aros-build
make AHI-coreaudio-bridge-darwin     # the host-CoreAudio bridge linklib FIRST
make workbench-devs-AHI-quick        # subsystem + drivers (installs all 4 pieces)
make workbench-c-ahismoke            # the AHISmoke test client
```

This installs `Devs/ahi.device`, `Devs/AHI/coreaudio.audio`,
`Devs/AudioModes/COREAUDIO` and `C:AddAudioModes` into the boot tree.

Two things made it first-class (`workbench/devs/AHI/mmakefile.src`):

- **Order.** AHI is an autotools subsystem; its `configure` probes for
  `-lcoreaudio-bridge` and *silently drops the CoreAudio driver* if the linklib
  is not yet in `Developer/lib`. The `-quick` targets skip the prereq that would
  build it, so the bridge (`AHI-coreaudio-bridge-darwin`) must be built first.
  `rebuild-aros.sh` orders them; a from-clean build without it leaves you with
  `ahi.device` but no `coreaudio.audio`.
- **Flags.** `%build_with_configure` feeds AHI `USER_CFLAGS`/`USER_LDFLAGS`
  through the configure environment (its Makefiles use `@CFLAGS@`/`@LDFLAGS@`)
  but not the tree-wide `LDFLAGS`/`make.cfg` flags. So the AHI mmakefile now
  restates the two aarch64-host-port requirements — `-ffixed-x18` and
  `-Wl,--allow-multiple-definition` — or every AHI link fails with
  `duplicate symbol __aros_libreq_SysBase`.

The translation catalogs (`workbench/devs/AHI/{AHI,Device}/translations`) must
be present as git submodules; they are already initialized in `../aros-upstream`.

## Streaming Clients (Moonstone)

`C:Moonstone` is the first application that *streams* through this path rather
than playing one preloaded sample. Its shim is
[hosted/rust/aros_moonstone_audio.c](../../../hosted/rust/aros_moonstone_audio.c):
two `AHIST_DYNAMICSAMPLE` buffers on one channel, flipped by AHI's `SoundFunc`
hook, refilled by the game loop. Two things are worth reusing:

- **Pick the mode by driver name.** Nine audio modes are registered here and
  only `coreaudio` has a host behind it. With no AHI prefs file, the device's
  default unit falls back to `AHI_BestAudioID`, which can name a driver for
  hardware this machine does not have. The shim walks `AHI_NextAudioID` and
  matches `AHIDB_Driver` against `coreaudio` instead.
- **Open the device after loading assets.** Host-backed file reads block the
  whole guest, so reading a sample bank with AHI already streaming empties the
  driver's ring (21 ring underruns became 12 by swapping the order).

`Moonstone audio` is a windowless check: it plays one tune for five seconds and
reports how many buffers went out stale. Measured on this port, playback holds
0 stale buffers once the host file cache is warm. A first cold play stutters
about ten times in 40 s, and the game's own worst-frame counter says why: a
cold frame can reach **920 ms**, far beyond any buffering an application can
queue.

Ring underruns are not the same measure: the reference `AHISmoke` (one static
sample, no file I/O) reports 0, while a streaming client that also reads assets
sees a couple of dozen over a 40 s run, nearly all of them at startup.

### Setting the audio buffer

`MOONSTONE_AUDIO_FRAMES` is the sample frames per AHI buffer. Two buffers are
in flight, so it sets both how far ahead sound is queued (the slack against a
slow frame) and how late a combat sample is heard. Those move together: more
lookahead is always more latency.

| Value | Buffer | Queued ahead | Effect |
| --- | --- | --- | --- |
| `1024` | 46 ms | 93 ms | snappiest samples, stutters soonest |
| `2048` (default) | 93 ms | 186 ms | clean once the file cache is warm |
| `4096` | 186 ms | 372 ms | about half the cold-play stutter |
| `0` | | | run silent |

Out of range values are clamped to 256..16384, and AHI raises anything below
the mode's own minimum. Set it for one session, or in `ENVARC:` so it survives
a reboot:

```
setenv MOONSTONE_AUDIO_FRAMES 4096          ; this session
copy ENV:MOONSTONE_AUDIO_FRAMES ENVARC:     ; and the next ones
```

`MOONSTONE_AUDIO_SECS` sets how long `Moonstone audio` plays (default 5).

## Remaining Polish

The low-level playback path works and its build is first-class. Remaining work
is UX polish:

- add default AHI preference wiring so the CoreAudio mode is selected by normal
  desktop configuration rather than only registered by startup
- add mute and, if needed later, AHI-native mixer preference integration
- expand the smoke to cover longer playback and repeated start/stop cycles
- **audio-smoke** now sets `COREAUDIO_DEBUG=1` so the shim's `ca_start`/ring
  markers appear (the dylib is quiet by default); the same env turns them on for
  any manual launch.

The older `design.md` and `spec.md` remain the design record. Their planning
language predates this implementation; use this README as the live status.
