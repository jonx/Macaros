# Implementation spec — Volume / mixer gadget (desktop sound-level control)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).
> **Foundation (do NOT reimplement):** the CoreAudio-backed AHI sub-driver —
> [../coreaudio-audio/spec.md](../coreaudio-audio/spec.md). This spec extends that
> shim's ABI with a volume verb and reuses its render-to-WAV + RMS verification
> harness. Mac-side counterpart: [../host-app-shell/spec.md](../host-app-shell/spec.md)
> (the Sound tab / menu-bar volume item).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent, driver,
commodity, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this spec
+ the approved sources cited by tag: `[PUB]` Apple CoreAudio AudioObject volume-property
docs / POSIX / published standards and the published MUI/Zune + Commodities API;
`[AROS]` in-tree AROS headers and source (paths given; APL/LGPL — ours); `[OURS]` this
project's spikes and shims (the H-series, `hosted/coreaudio/`, the audio feature's
markers). `[DERIVED]` items are independently-derived requirements flagged for extra
verification; each stands solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification —
implement from that justification, never from any reference. No identifier name, call
sequence, file layout, slider-window shape, or gain algorithm in this spec derives from
any third-party implementation.

## Scope

**In.** A desktop **volume / mixer gadget** for hosted `aarch64-darwin` AROS that:
exposes a sound-level control on the AROS desktop (a Commodities commodity with a small
MUI/Zune Slider); drives **AROS AHI master volume** by default through the public
`AHI_SetEffect(AHIET_MASTERVOLUME)` LVO; optionally also drives the **macOS default
output-device volume** through a new verb in the existing CoreAudio host shim; and is
verified unattended by the audio feature's render-to-WAV + RMS oracle (the gadget value
scales the rendered output linearly) plus a host-volume read-back — no human listening,
no TCC prompt.

**Reused verbatim — cite, do NOT reimplement (`[AROS]`/`[OURS]`):**
- The **AHI device + software mixer + the master-volume effect path** — `_AHI_SetEffect`
  (`workbench/devs/AHI/Device/sound.c:706`), `DoMasterVolume`
  (`workbench/devs/AHI/Device/mixer.c:1080`), `ahiac_SetMasterVolume`
  (`ahi_def.h:220`). We *call* `AHI_SetEffect`; we do not modify the mixer. `[AROS]`
- The **CoreAudio AHI sub-driver + host shim** —
  [../coreaudio-audio/spec.md](../coreaudio-audio/spec.md); the shim
  (`hosted/coreaudio/coreaudio_shim.{c,h}`, `build/libcoreaudio.dylib`) and its flat
  `ca_*` C ABI loaded via `hostlib.resource`. We *append* two symbols. `[OURS]`
- The **render-to-WAV + RMS/FFT oracle** — `ca_render_to_wav`
  (`coreaudio_shim.h:86`) and the audio feature's WAV/RMS assertions. `[OURS]`
- The **Zune Slider widget + MUI app skeleton + Commodities broker** — published AROS
  classes/APIs (`muimaster` Slider, `commodities.library` `NewBroker`/`CxBroker`). `[AROS]`

**Out (non-goals, this spec).** Per-channel mixing (a full mixer with one fader per
voice — this is a *master* control); recording/input gain; above-unity boost (gadget is
0..100% → `0..0x10000` only); per-application volume; the Mac menu-bar/Sound-tab UI
itself (that is [host-app-shell](../host-app-shell/spec.md) — this spec only provides the
shared `ca_set_host_volume` verb + `volume_apply` semantics it calls); a screen-titlebar
gadget (deferred; the Commodity ships first); any change to the AHI mixer source.

## Architecture

Three small pieces over contracts that already exist. The AROS gadget and the host shim
are joined by the audio feature's flat `ca_*` C ABI (extended append-only); the gadget
couples to AHI through the public `AHI_SetEffect` LVO.

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple toolchain)
┌──────────────────────────────────────┐            ┌────────────────────────────┐
│ Volume commodity  [OURS, AROS-shaped] │            │ libcoreaudio.dylib  [OURS] │
│  · NewBroker/CxBroker + hotkey [AROS]  │  hostlib + │  (audio shim, reused)      │
│  · MUI Slider 0..100         [AROS]    │  H3 ABI    │  + ca_set_host_volume()    │
│  · volume_apply(pct, alsoHost) [OURS] ─┼─ ca_* ───► │  + ca_get_host_volume()    │
│        │                               │            │    · default output dev    │
│        ├─AROS: AHI_SetEffect(          │            │    · kAudioDeviceProperty  │
│        │   AHIEffMasterVolume{         │  [AROS]    │      VolumeScalar  [PUB]   │
│        │     AHIET_MASTERVOLUME, Fixed})│            └────────────────────────────┘
│        │  → ahiac_SetMasterVolume       │
│        │  → DoMasterVolume scales PCM ──┼── the SAME PCM the sub-driver renders → WAV
│        └─host (opt): ca_set_host_volume─┼──────────────────────────────────────►
└──────────────────────────────────────┘
```

- **AROS commodity** `[AROS]`-shaped — built by AROS crosstools, no host headers.
- **Host shim verbs** `[OURS]`/`[PUB]` — native arm64 C added to the existing audio
  shim, built with host clang, no AROS headers; reached via `hostlib.resource`.
- Spike-phase: shim verbs in `hosted/coreaudio/`; at graft, the AROS commodity lands in
  the proposed `workbench/tools/commodities/Volume/` (or `workbench/prefs/Volume/` —
  decided at graft).

## The host ABI extension (`coreaudio_shim.h`, append-only)

Two new verbs, appended to the existing flat ABI (the audio feature froze the existing
`ca_*` symbols; this only adds). Hand-authored, neutral.

```c
/* Set the macOS DEFAULT OUTPUT device master volume. `scalar` is 0.0..1.0 (clamped).
   Returns 0 on success; nonzero if the default output exposes no settable volume
   (e.g. a fixed HDMI/optical output) — in which case the AROS master volume remains
   the only target. Never blocks the audio RT path; a plain host-object get/set
   serialised under HostLib_Lock. No entitlement, no TCC. */
int ca_set_host_volume(float scalar);

/* Read the macOS default output device master volume into *outScalar (0.0..1.0).
   Returns 0 on success; nonzero if unavailable. Used to save/restore and to verify. */
int ca_get_host_volume(float *outScalar);
```

`coreaudio.exports` gains `_ca_set_host_volume` and `_ca_get_host_volume`; the AHI
bridge's `CA_*` function-pointer table (the audio feature's `coreaudio-bridge/`) gains
the two pointers resolved through `HostLib_GetPointer`. The header stays the only contact
surface; the shim pulls no AROS headers.

### R-HOSTVOL — host output-volume get/set (`[PUB]`, restated)

Implement against Apple's CoreAudio object API `[PUB]`:

1. **Find the default output device.** `AudioObjectGetPropertyData` on
   `kAudioObjectSystemObject` with an `AudioObjectPropertyAddress`
   `{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
   kAudioObjectPropertyElementMain}` → an `AudioDeviceID`. `[PUB]`
2. **Get/set the volume scalar.** On that `AudioDeviceID`, address
   `{kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput,
   kAudioObjectPropertyElementMain}`. If `AudioObjectHasProperty` for the **main**
   element is false, fall back to per-channel elements (`1, 2, …`) and set each; if no
   element is settable, return nonzero. **Preferred convenience:**
   `AudioHardwareServiceSetPropertyData` /
   `kAudioHardwareServiceDeviceProperty_VirtualMainVolume` (AudioToolbox) picks
   main-or-per-channel automatically — use it if available, else the manual path. `[PUB]`
3. **Mute (optional).** `kAudioDevicePropertyMute` (UInt32 0/1) if present; the gadget's
   mute may use this or simply set scalar 0 and restore. `[PUB]`
4. **Clamp** `scalar` to `[0.0, 1.0]`. **Discipline:** the call runs on the calling
   (AROS-task or host-test) thread, serialised under `HostLib_Lock` like every other host
   call; it must **not** be issued from the audio RT render callback (it is not, by
   construction — the gadget thread calls it). `[OURS]` (the audio feature's RT-callback
   iron rules are untouched.)

**UNVERIFIED:** which volume element the Apple-Silicon built-in output reports (main vs
per-channel), and whether `VirtualMainVolume` is present on this OS version — [VL1]
settles both by reading back. The "no settable volume" branch must be real, not assumed
away.

## The AROS commodity (`[AROS]`-shaped, contract from design.md)

A new small program (proposed `workbench/tools/commodities/Volume/`; final home a graft
decision). Three parts: the broker, the slider window, and the apply core. The **apply
core is the only thing the unattended oracle needs** — it is GUI- and broker-free.

### R-BROKER — the Commodities broker (`[AROS]`)

Open `commodities.library`; build a `struct NewBroker`
(`compiler/include/libraries/commodities.h:25`): `nb_Version = NB_VERSION` (= 5,
`commodities.h:39`), `nb_Name = "Volume"`, `nb_Title`/`nb_Descr` set, `nb_Unique =
NBU_UNIQUE` (`:43`), `nb_Flags = COF_SHOW_HIDE | COF_ACTIVE` (`:47–48`), `nb_Port = ` our
`MsgPort`. `CxBroker(&nb, NULL)` → check `CBERR_OK` (`:55`). Attach a `CxFilter` on a
hotkey (`HotKey`/`CxTranslate`) that posts a "toggle window" command to `nb_Port`;
`ActivateCxObj(broker, TRUE)`. The broker survives with no window open (the always-
available applet shape). On a CXM_DISAPPEAR / quit signal, deactivate and free.

### R-SLIDER — the slider window (`[AROS]`)

A MUI app (skeleton per `workbench/system/SysMon/main.c:143,410`,
`#include <proto/muimaster.h>`): `ApplicationObject` → one `WindowObject` containing a
horizontal `SliderObject` (`workbench/libs/muimaster/classes/slider.c`) with
`MUIA_Numeric_Min 0`, `MUIA_Numeric_Max 100`, `MUIA_Slider_Horiz TRUE`
(idiom: `developer/debug/test/Zune/busy.c:43`); a "Mute" checkmark; and an optional
"Also set Mac volume" checkmark. Wire
`DoMethod(slider, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime, app, …)` to call
`volume_apply` with the live value (idiom: `busy.c:53`). The window opens on the broker
hotkey and closes-to-hide. Set the slider's initial value from the current AHI master
volume / host volume read-back. `[AROS]` (the AHI prefs GUI proves the same Numeric
slider for volume, `workbench/devs/AHI/AHI/gui_mui.c:381–402`.)

### R-APPLY — `volume_apply(int pct, BOOL alsoHost)` (the testable core, `[OURS]`+`[AROS]`)

Map and dispatch:

- **AROS master volume (always) `[AROS]`.** Convert `pct` (0..100) to 16.16 `Fixed`:
  `Fixed v = (Fixed)(((LONG)pct * 0x10000) / 100);` (so 100 → `0x10000` unity, 50 →
  `0x8000` half, 0 → `0`). Build and apply the effect:
  ```c
  struct AHIEffMasterVolume eff = { AHIET_MASTERVOLUME, v };   /* ahi.h:94,301 */
  AHI_SetEffect((APTR)&eff, audioctrl);                        /* ahi_lib.sfd:31 */
  ```
  This lands in `audioctrl->ahiac_SetMasterVolume` (`sound.c:709–711`) and is applied by
  `DoMasterVolume` (`mixer.c:1080`), which scales every output frame
  (`vol = SetMasterVolume>>4; sample = *dst*vol; *dst = sample>>12`, `mixer.c:1129–1144`).
  Do **not** use `AHIET_MASTERVOLUME|AHIET_CANCEL` for silence — that resets to unity
  (`sound.c:716–722`); for silence pass `ahiemv_Volume = 0`. `[AROS]`
- **The `AudioCtrl` to target.** Apply to the desktop's system output `AudioCtrl`.
  **Primary path:** the one canonical system `AudioCtrl` if it can be obtained
  (UNVERIFIED — see Open questions); **guaranteed fallback (R-GAIN):** if no single
  `AudioCtrl` is reachable, *or* if [VL2] shows the AHI effect doesn't track (post-proc
  off), apply the gain in **our own CoreAudio sub-driver** below all `AudioCtrl`s (next
  requirement). `volume_apply` calls whichever path is the confirmed control point; the
  spec ships both and [VL2] selects.
- **Host volume (only if `alsoHost`) `[PUB]`/`[OURS]`.** Call
  `ca_set_host_volume(pct / 100.0f)` through the bridge. Ignore-with-warning if it
  returns nonzero (fixed-output device) — the AROS volume still applied.
- **Mute.** Save the pre-mute `pct`, set 0 (AROS effect `ahiemv_Volume = 0`; host
  `ca_set_host_volume(0)` or `kAudioDevicePropertyMute`); on un-mute restore the saved
  `pct` through the normal path. `[OURS]`

### R-GAIN — sub-driver gain fallback (the guaranteed control point, `[OURS]`+`[DERIVED]`)

If the AHI master-volume effect does not reliably scale the desktop's audio path (the
`AHIACF_POSTPROC` gate, `mixer.c:1045`; V5's `update_MasterVolume` forcing unity,
`effectinit.c:62–80`), apply the volume as a **gain in the CoreAudio sub-driver's
int16→host copy** — the slot where the sub-driver already converts each frame
(`[../coreaudio-audio/spec.md]` §Sample format, the OSS-narrow slot
`OSS-playslave.c:169`). One multiply per sample: `out = (short)((int)in * curGain >> 16)`
with `curGain` a 16.16 word the gadget sets via a new bridge verb (or the same
`ca_set_host_volume` interpreted as a *ring* gain — decision at graft). This sits **below
every `AudioCtrl`**, so it is a true machine-wide AROS master regardless of mixer
post-proc state. `[DERIVED]` independent justification: a single linear gain on the final
PCM before hand-off is the minimal, always-correct master attenuator; it stands on the
`[AROS]` observation that `DoMasterVolume` may be gated off + the `[OURS]` sub-driver
copy slot — implement from those, no reference. **Decision rule:** primary = AHI effect
(scales the in-AROS mix too, the "right" layer); fallback = sub-driver gain (always
works). [VL2] picks; both are spec'd.

### R-INIT — initial value (`[AROS]`/`[PUB]`)

On window open, set the slider from the live state: AROS side read back the current
`ahiac_SetMasterVolume` if reachable (default `0x00010000` = 100%,
`audioctrl.c:192–193`); host side `ca_get_host_volume(&s)` → `pct = s*100`. If neither is
readable, default the slider to 100%.

## Verification (unattended — `[OURS]` audio-feature + H7 discipline)

No human listens; no TCC / Screen-Recording / Microphone prompt is hit
(reading/writing a device-volume property and rendering to a file need no entitlement).
The oracle is **(1) the audio feature's render-to-WAV + RMS ratio** and **(2) a
host-volume read-back** — never an ear, never a pixel for the verdict.

**The volume-specific assertions** (each marker asserts *values*):

- **RMS ratio.** Render the same sine to WAV at gadget = 100% and = 50%; assert
  `RMS(50%) ≈ 0.5 × RMS(100%)` within tolerance, and `RMS(0%) ≈ 0`. (Reuses the audio
  feature's WAV/RMS/Goertzel harness verbatim.) This is the one assertion that proves
  "the gadget scaled the real mixer output by the requested amount."
- **Monotonicity.** A percentage sweep {0,25,50,75,100} yields a non-decreasing RMS
  sequence with the 50% point halving the 100% point.
- **Host read-back.** Set host volume to a known scalar, read it back independently,
  assert equality within tolerance, restore the original.

**Markers** (one host/AROS binary per marker, `[VL?]` PASS/FAIL block, clean-exit on
PASS; family `[VL*]`):

- **[VL1] host volume get/set round-trips (pure host).** Extend the shim; a host probe
  `ca_get_host_volume` (save), `ca_set_host_volume(0.5)`, `ca_get_host_volume` (assert ≈
  0.5), restore. PASS = read-back == set value within tol; no TCC prompt; the "no
  settable volume" branch returns nonzero cleanly if hit. Proves R-HOSTVOL + the
  no-entitlement claim before any AROS. `[VL1]`
- **[VL2] AHI master volume scales the mixed PCM (the core numeric oracle).** Drive a
  known sine through the AHI mixer (reuse coreaudio-audio [A2]/[A5] pull), render to WAV
  at `ahiemv_Volume = 0x10000` then `0x8000` via `AHI_SetEffect(AHIET_MASTERVOLUME)`.
  PASS = `RMS(0x8000) ≈ 0.5 × RMS(0x10000)`, `RMS(0) ≈ 0`. **Settles the
  `AHIACF_POSTPROC` caveat:** if the ratio does not track, the AHI effect is gated off →
  switch `volume_apply` to R-GAIN and re-assert the ratio there (same PASS condition).
  `[VL2]`
- **[VL3] host volume changed, verified two-sided.** Run `volume_apply(50, alsoHost=TRUE)`;
  an independent re-read (`ca_get_host_volume` *and* a direct
  `AudioObjectGetPropertyData` from a separate read path) confirms the macOS
  default-output volume changed to ≈ 0.5; restore. PASS = independent read-back matches.
  Mirrors the host-volume feature's two-sided assert. `[VL3]`
- **[VL4] `volume_apply` logic end-to-end (GUI-free).** Call `volume_apply` with
  {0,25,50,75,100}, each driving a fresh WAV render through the confirmed control point.
  PASS = the RMS sequence is monotone non-decreasing and 50% halves 100%. Proves the
  gadget's logic without painting the slider (invoke the action core directly — the
  host-shell `[SET]` technique). `[VL4]`
- **[VL5] graft: the commodity on the desktop.** Build the real Commodities commodity +
  MUI slider; boot windowed; via [`aros-ctl`](../control-harness/README.md) trigger the
  hotkey, move the slider, and capture a **screenshot** (offscreen oracle, TCC-free) as
  corroboration. The verdict stays the numeric WAV-RMS ratio from [VL2]/[VL4] driven
  through the live gadget; the screenshot proves it paints + tracks, it is not the
  oracle. Rides the AHI sub-driver graft ([A5]) + muimaster/Commodities. `[VL5]`

## Build / integration

- The two shim verbs link the **already-linked** `AudioToolbox, CoreAudio,
  CoreFoundation` (no new framework — `AudioHardwareService*` is in AudioToolbox); built
  with host clang `-arch arm64` as part of `build/libcoreaudio.dylib`; deployed exactly
  as today (`graft/run-window.sh` / `graft/aros-ctl run` copy the dylib to `~/lib`;
  `graft/deploy-check` hashes it). Add `_ca_set_host_volume`/`_ca_get_host_volume` to
  `hosted/coreaudio/coreaudio.exports`.
- [VL1]/[VL3] compile to a Mach-O host probe via the existing `make hosted-coreaudio` /
  `make coreaudio-abi` pattern (assert the marker on stdout, uniform
  `result=(PASS|FAIL)`). [VL2]/[VL4] reuse the audio feature's WAV/RMS harness.
- The AROS commodity is built by the **AROS crosstools** (muimaster + commodities), not
  host clang. It pulls no CoreAudio headers; it reaches the host volume only through the
  `CA_*` bridge (the audio feature's `coreaudio-bridge/`).
- The C ABI header is shared, hand-written, independent work; the shim must not include
  AROS headers; the AROS side must not include CoreAudio headers.

## Open questions / UNVERIFIED

- **`AHIACF_POSTPROC` gating** — whether the AHI master-volume effect reliably scales the
  desktop audio path, or whether R-GAIN (sub-driver gain) is the real control point.
  [VL2] decides; both paths are spec'd. *(The single biggest unknown.)*
- **Which system `AudioCtrl`** the gadget should target for the AHI effect (one canonical
  handle vs enumerate open units) — the R-GAIN fallback sidesteps it.
- **Host volume element** — main vs per-channel vs no-settable on the Apple-Silicon
  built-in output; whether `VirtualMainVolume` is present. [VL1] reads it back.
- **Whether the AHI effect or the sub-driver gain is the user-visible "right" layer** —
  the effect also scales the in-AROS mix (good for AHI clients that read the output
  buffer); the gain is below everything (always works). Default = effect-primary,
  gain-fallback; revisit if both prove reliable.
- **Mute via `kAudioDevicePropertyMute` vs scalar-0** on the host — present on built-in?
  Falls back to scalar-0 + restore.
- **Gadget home / autostart** — Commodity under `Tools/Commodities` vs a Prefs applet;
  WBStartup vs manual launch. Graft decision.

## Provenance summary

`[PUB]` Apple CoreAudio AudioObject volume property API
(`AudioObjectGetPropertyData`/`AudioObjectSetPropertyData`, `kAudioObjectSystemObject`,
`kAudioHardwarePropertyDefaultOutputDevice`, `kAudioDevicePropertyVolumeScalar`,
`kAudioDevicePropertyMute`, `kAudioObjectPropertyScopeOutput`/`ElementMain`),
AudioToolbox `AudioHardwareService` `VirtualMainVolume`; the published MUI/Zune Slider
(over Numeric) and Commodities (`NewBroker`/`CxBroker`/`CxFilter`/`HotKey`) APIs;
16.16 fixed-point scaling and linear-gain / RMS arithmetic. ·
`[AROS]` `Include/SFD/ahi_lib.sfd` (`AHI_SetEffect` :31, `AHI_SetVol` :26),
`Include/C/devices/ahi.h` (`struct AHIEffMasterVolume` :94, `AHIET_MASTERVOLUME` :301,
`AHIET_CANCEL` :300), `Device/sound.c` (master-volume effect handler :706–722, autodoc
range :552–572), `Device/mixer.c` (`DoMasterVolume` :1080–1147, post-proc gate :1045),
`Device/effectinit.c` (`update_MasterVolume` V5 unity force :54–80), `Device/devcommands.c`
(`UpdateMasterVolume` :1516), `Device/audioctrl.c` (default `0x00010000` :192–193),
`Device/ahi_def.h` (`ahiac_SetMasterVolume` :220); `libraries/commodities.h`
(`NewBroker` :25, `NB_VERSION` :39, `NBU_UNIQUE`/`COF_*` :41–48, `CBERR_OK` :55),
`workbench/tools/commodities/Exchange.c`; `libs/muimaster/classes/slider.c`,
`developer/debug/test/Zune/busy.c:43,53`, `workbench/system/SysMon/main.c:143,410`,
`workbench/devs/AHI/AHI/gui_mui.c:381–402` (the in-tree volume-slider precedent);
`workbench/workbench.h:294` (`AddAppIconA` — rejected option). ·
`[OURS]` the CoreAudio shim + flat `ca_*` ABI (`hosted/coreaudio/coreaudio_shim.{c,h}`,
`coreaudio.exports`, `build/libcoreaudio.dylib`) and its `ca_render_to_wav` +
WAV/RMS/Goertzel oracle ([../coreaudio-audio/spec.md] [A1]–[A5]); `hostlib.resource` +
the H3 host-call boundary (`hosted/abishim.S`); the control harness for the [VL5]
screenshot (`graft/aros-ctl`); the host-vs-guest ownership rule + Sound tab /
`CM_OPT_AUDIO_VOLUME` ([../host-app-shell/spec.md]). ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) **R-GAIN** — a single linear gain on the final PCM in the sub-driver's int16→host
copy is the minimal always-correct master attenuator, restated from the `[AROS]`
post-proc gate + the `[OURS]` sub-driver copy slot; and (b) restricting the gadget to the
`0..0x10000` attenuator range (no above-unity boost), restated from the `[AROS]` autodoc
range + clipping risk. No third-party code, identifiers, or call sequence used; any
resemblance is coincidental.
