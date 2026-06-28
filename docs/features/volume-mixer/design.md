# Volume / mixer gadget — a desktop sound-level control for hosted AROS

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Implementation spec: [spec.md](spec.md). Process: [../CLEANROOM.md](../CLEANROOM.md).
> **Foundation:** the CoreAudio-backed AHI sub-driver —
> [../coreaudio-audio/design.md](../coreaudio-audio/design.md) +
> [spec.md](../coreaudio-audio/spec.md). This feature *depends on* it and reuses its
> render-to-WAV + RMS verification harness; it does **not** re-design the sub-driver.
> Cross-link: [../host-app-shell/design.md](../host-app-shell/design.md) (the Sound
> settings tab / menu-bar surface this gadget mirrors on the Mac side).

## What & why

Hosted AROS has a complete retargetable audio stack (AHI) with a software mixer, and —
once the [CoreAudio AHI sub-driver](../coreaudio-audio/design.md) lands — real output to
the Mac's speakers. What it still lacks is the one control a desktop user expects: a
**volume gadget** — a sound-level slider you can reach without dropping to a shell or
opening AHI's full prefs editor. This feature adds that gadget and wires it to the
right control point.

The gadget drives **two** layers, in order of priority:

1. **AROS AHI master volume** (default) — the global mixing volume applied inside AROS's
   own software mixer, so *every* AROS sound (AHI clients and, transparently, classic
   Paula programs through the `audio.device`→`ahi.device` shim) scales together.
2. **macOS host output volume** (optional second target) — the Mac's default
   output-device volume, so the gadget can act as a single master for the whole machine,
   not just for AROS-generated sound.

Why it earns a doc: the *control point* is the load-bearing decision. AHI has more than
one thing called "volume" (per-channel `AHI_SetVol`, the `audio.device` unit-volume
mapping, the master-volume **effect**), and the AROS V5 mixer treats one of them as a
near-no-op. Picking the wrong one yields a gadget that moves a slider and changes
nothing. This doc grounds the *exact* control point in the real mixer source, then
designs the smallest desktop gadget that drives it — and proves both with the same
numeric WAV oracle the audio feature already established.

This is the project thesis applied one more turn: *macOS owns the drivers; AROS reaches
them via standard exec I/O.* The AROS side reaches the host's master volume through the
same `hostlib.resource` shim ABI the audio feature uses; the AROS-side volume is pure
AHI, no host at all.

## Does it already exist?

**Partly — the plumbing exists, the desktop gadget does not.**

- **The AROS-side volume control point exists** (reused, not rebuilt): AHI's software
  mixer already scales every output sample by a master-volume word
  (`workbench/devs/AHI/Device/mixer.c` `DoMasterVolume`), set via the public
  `AHI_SetEffect(AHIET_MASTERVOLUME)` LVO. See "Background" — this is what we drive, not
  reimplement.
- **A full AHI prefs/mixer GUI exists** (`workbench/devs/AHI/AHI/`, MUI/Zune): the
  `gui_mui.c` panel already builds MUI **Numeric sliders** for output / monitor / gain
  volume (`gui_mui.c:381,391,401`). It is the *settings editor* for the whole AHI
  subsystem (mode, channels, frequency, clipping), opened deliberately — **not** a
  one-touch desktop level control, and it edits AHI *prefs*, not the live master volume.
  It is the precedent for the slider widget, not a substitute for the gadget.
- **An AHI handler exists** (`workbench/devs/AHI/AHI-Handler/`, the `AHI:`/`AUDIO:`
  device) — a DOS path for streaming samples, unrelated to a level control.
- **No desktop volume gadget exists** anywhere in the tree: no Commodities commodity, no
  AppIcon, no screen-titlebar gadget, no Wanderer prefs panel that exposes a live
  master-volume slider on the desktop. `grep -rIn -i 'mastervolume\|SetMasterVolume'`
  over `workbench/` hits only the AHI **device** internals and the AHI **prefs**
  channel-count mapping — never a desktop applet.
- **The host side has a CoreAudio shim** (`hosted/coreaudio/`, `build/libcoreaudio.dylib`,
  ABI-proven — see [../coreaudio-audio/README.md](../coreaudio-audio/README.md)). It
  owns the AudioUnit + SPSC ring and exposes a flat `ca_*` C ABI. It does **not** yet
  touch the host *device volume* property — that verb is new here (see "The bridge").

So the work is small and additive: (a) a thin AROS desktop gadget that calls the
existing AHI master-volume LVO; (b) one new verb in the existing CoreAudio shim for the
host output volume; (c) optional cross-link into the host-app-shell Sound tab. The AHI
device, mixer, and sub-driver are reused unchanged.

**External prior art (web-grounded, not in the AROS tree).** Every desktop OS ships a
master volume control (a menu-bar/tray slider on macOS, a panel applet on Linux). On
classic Amiga, a "volume commodity" was a common shareware shape — a small Commodities
broker with a hotkey and a tiny slider window. We don't read any of them; the
Commodities **broker model** (`NewBroker`/`CxBroker`) and the MUI **Slider** class are
published AROS/AmigaOS APIs we implement to directly. macOS's own master volume is the
`kAudioDevicePropertyVolumeScalar` property on the default output device — public
CoreAudio. No third-party implementation is consulted; resemblance is coincidental.

## Background: the AROS volume control points (grounded)

AHI exposes **three** distinct "volume" notions. Only one is the right master control
point; the other two are wrong for a global desktop slider. All paths verified in
`/Users/user/Source/aros-upstream`.

### (a) Per-channel `AHI_SetVol` — NOT the control point

`AHI_SetVol(Channel, Fixed Volume, sposition Pan, AudioCtrl, Flags)`
(`workbench/devs/AHI/Include/SFD/ahi_lib.sfd:26`) sets the volume of **one playing
channel**, in `Fixed` 16.16 (1.0 = `0x10000`). It is what a *player* (a tracker, a sound
effect) uses per voice. A desktop master slider would have to know and reach every
channel of every open `AudioCtrl` — wrong layer. Skip it.

### (b) `audio.device` unit volume — NOT the control point

The classic `audio.device` maps a Paula-style unit volume onto AHI by *synthesising a
master-volume effect scaled by channel count*
(`workbench/devs/AHI/Device/devcommands.c:1516` `UpdateMasterVolume`, building a
`struct AHIEffMasterVolume` with `ahiemv_Volume = iounit->Channels * 0x10000 / …` at
`:1612`). This is per-`audio.device`-unit, derived from how many Paula channels are
open — again not a single global knob. It is *evidence* that the master-volume effect is
the real lever (the device layer reaches volume through it), but it is not our entry
point.

### (c) The master-volume **effect** — THE control point ✅

The cleanest global knob is the AHI **master volume effect**, set through the public LVO
`AHI_SetEffect(APTR effect, AudioCtrl)` (`ahi_lib.sfd:31`) with a
`struct AHIEffMasterVolume` (`workbench/devs/AHI/Include/C/devices/ahi.h:94`):

```c
struct AHIEffMasterVolume {
    ULONG ahie_Effect;     /* = AHIET_MASTERVOLUME  (ahi.h:301, value 1) */
    Fixed ahiemv_Volume;   /* 16.16 fixed; 0x10000 = 1.0 = unity; range per autodoc */
};
```

**Where it lands.** `_AHI_SetEffect` (`workbench/devs/AHI/Device/sound.c:706–714`) copies
`ahiemv_Volume` straight into `audioctrl->ahiac_SetMasterVolume` and calls
`update_MasterVolume`. The `AHIET_MASTERVOLUME|AHIET_CANCEL` case resets it to `0x10000`
(`sound.c:716–722`).

**Where it bites the samples.** `DoMasterVolume` (`workbench/devs/AHI/Device/mixer.c:1080–1147`)
scales **every mixed output frame** by `ahiac_SetMasterVolume`. For the stereo-16-bit
buffer we use (`AHIST_S16S`), the inner loop is:

```c
/* mixer.c:1129–1144 */
WORD *dst = buffer;
vol = audioctrl->ahiac_SetMasterVolume >> 4;   /* 0x10000 -> 0x1000 */
while (cnt--) {
    sample = *dst * vol;
    /* clip to 28-bit */
    *dst++ = sample >> 12;                      /* unity: *0x1000 >>12 == identity */
}
```

So `ahiemv_Volume = 0x10000` is unity, `0x8000` is exactly half-amplitude, `0` is
silence — a clean linear scaler **right where the CoreAudio sub-driver pulls its PCM**.
This is precisely what makes it verifiable by the audio feature's RMS oracle: set the
effect to 50% → the mixed buffer (and therefore the rendered WAV) carries half the RMS.

**The one real caveat (must be handled, not hand-waved).** `DoMasterVolume` runs **only
when the post-processing flag is set** — `if (audioctrl->ac.ahiac_Flags & AHIACF_POSTPROC)`
(`mixer.c:1045`). And in V5 `update_MasterVolume` deliberately forces the per-channel
`ahiac_MasterVolume = 0x10000` (full) for clipping safety and **comments out** reading
`ahiac_SetMasterVolume` (`effectinit.c:62–80`). Net: the master volume takes effect via
the **post-processing `DoMasterVolume` pass**, which reads `ahiac_SetMasterVolume`
directly — *not* via per-channel scaling. **Implication for us:** the master-volume
effect is honoured **iff** the mixer is doing post-processing for that `AudioCtrl`. We
must (1) confirm post-proc is active for the path the CoreAudio sub-driver uses (it is
set when any post-processing effect / clipping path is engaged — **verify in [VL2]**),
and (2) if it is *not* reliably on, fall back to driving volume in our **own** layer (the
sub-driver's int16→host copy is the natural place to apply a gain) so the gadget always
works regardless of mixer post-proc state. The grounded primary path is the AHI effect;
the sub-driver-gain fallback is the safety net. This caveat is the headline risk.

### The Fixed range (autodoc)

The autodoc (`sound.c:564–572`) states `ahiemv_Volume` ranges `0.0 …
(channels / hardware-channels)` — i.e. it can **boost above 1.0** when you have spare
channels (mono 4-channel → up to 4.0). For a *master attenuator* gadget we restrict to
`0.0 … 1.0` (`0 … 0x10000`); boost is out of scope (clipping risk). The default/unity is
`0x10000` (`audioctrl.c:192–193` initialises both `ahiac_MasterVolume` and
`ahiac_SetMasterVolume` to `0x00010000`).

## Background: the gadget options (grounded)

Where does a "desktop volume slider" live on AROS? Four candidate shapes, each grounded:

| Option | What | In-tree precedent | Fit |
|--------|------|-------------------|-----|
| **Commodities commodity** | a background broker (hotkey + tiny MUI window with a Slider) registered with Exchange | `NewBroker`/`CxBroker` (`libraries/commodities.h:25`, `NB_VERSION 5`); Exchange app `workbench/tools/commodities/Exchange.c` | **chosen** — the canonical "small always-available applet" shape; survives with no window open; hotkey to pop the slider |
| **Wanderer prefs panel** | a Prefs editor under Prefs/ | the AHI prefs GUI `workbench/devs/AHI/AHI/gui_mui.c` (MUI sliders for volume already) | too heavy / deliberately-opened; it's the *settings editor*, not a one-touch level |
| **AppIcon on the desktop** | a Wanderer icon you double-click | `AddAppIconA` (`workbench/workbench.h:294`) | a launcher, not a live slider; clumsy as a level control |
| **Screen-titlebar gadget** | a custom gadget in the screen bar | Intuition screen-bar gadgets | most native-feeling but the most bespoke Intuition work; deferred |

**Decision: a Commodities commodity with a small MUI/Zune Slider window.** Rationale:
it is the smallest desktop-resident shape that (a) is always available (no window needed
until you want it), (b) has a published model (`NewBroker`), (c) reuses the proven Zune
Slider widget, and (d) is trivially scriptable for the unattended loop (the gadget's
*logic* — "value → `AHI_SetEffect`" — is a plain function the test calls directly,
without painting a pixel). A screen-titlebar gadget can be layered on later behind the
same logic.

**The slider widget (grounded).** Zune ships a `Slider` class
(`workbench/libs/muimaster/classes/slider.c`) over `Numeric`; the idiom is
`SliderObject, MUIA_Numeric_Min, 0, MUIA_Numeric_Max, 100, MUIA_Slider_Horiz, TRUE, End`
plus `DoMethod(slider, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime, …)` to react to
drags (`developer/debug/test/Zune/busy.c:43,53`). The AHI prefs GUI uses the same Numeric
sliders for its volume controls (`gui_mui.c:381–402`), so the widget is proven in-tree
for exactly this purpose. A MUI app skeleton (`ApplicationObject`/`WindowObject` +
`MUIM_Application_NewInput` loop) is grounded in `workbench/system/SysMon/main.c:143,410`
(`#include <proto/muimaster.h>`).

## Design

The feature is three small pieces joined by contracts that already exist.

```
 AROS desktop                                            macOS host
 ┌──────────────────────────────────────────┐           ┌──────────────────────────┐
 │ Volume commodity  [OURS, AROS-shaped]     │           │ libcoreaudio.dylib [OURS]│
 │  · NewBroker/CxBroker (hotkey)            │           │  (existing audio shim)   │
 │  · MUI Slider 0..100  [AROS muimaster]    │           │   + NEW verb:            │
 │  · on change → volume_apply(pct)          │           │   ca_set_host_volume()   │
 │        │                                  │           │   ca_get_host_volume()   │ hostlib
 │        ├── AROS target (default) ─────────┤           │     · default output dev │ + H3
 │        │   AHI_SetEffect(                  │           │     · kAudioDeviceProp   │ ABI
 │        │     AHIEffMasterVolume{           │           │       VolumeScalar       │
 │        │       AHIET_MASTERVOLUME,         │           │                          │
 │        │       ahiemv_Volume=Fixed(pct)})  │           │                          │
 │        │      → ahiac_SetMasterVolume      │           └──────────────────────────┘
 │        │      → DoMastervolume scales PCM ─┼── the SAME PCM the CoreAudio
 │        │                                   │   sub-driver pulls & renders → WAV
 │        └── host target (optional) ─────────┼──── ca_set_host_volume(pct) ────────►
 └──────────────────────────────────────────┘
```

### AROS side — the volume commodity (default target: AHI master volume)

A new small program (proposed `workbench/tools/commodities/Volume/` or a Prefs/Volume
applet — exact home decided at graft):

- **Broker.** Open `commodities.library`; `CxBroker(&newbroker, NULL)` with a
  `struct NewBroker` (`nb_Version = NB_VERSION`, `nb_Name = "Volume"`, a title/descr, a
  `MsgPort`, `nb_Unique = NBU_UNIQUE`) — `libraries/commodities.h:25–43`. Attach a hotkey
  filter (`CxFilter`/`HotKey`) to toggle the slider window. `ActivateCxObj`.
- **Slider window.** A MUI app (`ApplicationObject`/`WindowObject`, SysMon skeleton)
  holding one horizontal `SliderObject` `MUIA_Numeric_Min 0 … Max 100` plus a "Mute"
  checkbox and (optional) a "Also set Mac volume" checkbox. `MUIM_Notify` on
  `MUIA_Numeric_Value` calls the apply function. The window is open-on-hotkey,
  close-to-hide (broker stays alive).
- **The apply function — `volume_apply(int pct, BOOL alsoHost)`** — the load-bearing,
  fully testable core, host-and-GUI-free:
  - **AROS master volume:** map `pct` (0..100) → `Fixed` (`0 … 0x10000`):
    `Fixed v = (Fixed)((LONG)pct * 0x10000 / 100)`. Build
    `struct AHIEffMasterVolume eff = { AHIET_MASTERVOLUME, v }` and call
    `AHI_SetEffect(&eff, audioctrl)` on the active output `AudioCtrl`
    (`ahi_lib.sfd:31`, lands in `ahiac_SetMasterVolume`, `sound.c:709`). For `pct==0`
    use `AHIET_MASTERVOLUME|AHIET_CANCEL`? No — `CANCEL` resets to unity; for silence
    use `ahiemv_Volume = 0`. Mute = remember the prior value, set `0`, restore on
    un-mute.
  - **Host volume (if `alsoHost`):** call the new shim verb `ca_set_host_volume(pct/100.0)`
    (next section).
- **Which `AudioCtrl`?** The master-volume effect is per-`AudioCtrl`. The desktop's
  *system* sound path is whatever `AudioCtrl` the default AHI output mode/unit uses. The
  cleanest binding is to apply the effect to the **system AudioCtrl** (the one
  `datatypes`/`Play`/the desktop beep open). **Open question (see Risks):** whether
  there is a single canonical system `AudioCtrl` to grab, or whether the gadget must
  walk open units. The fallback that *always* works is to set the **host** volume and/or
  apply gain in our own CoreAudio sub-driver (which sits below all `AudioCtrl`s). The
  spec makes the sub-driver-gain path the guaranteed control point and the per-AudioCtrl
  AHI effect the "scales the in-AROS mix too" enhancement.

### Host side — the output-volume verb (optional second target)

Extend the **existing** CoreAudio shim (`hosted/coreaudio/coreaudio_shim.{c,h}`,
`coreaudio.exports`) with two new verbs — append-only, same flat C ABI the AHI bridge
already loads through `hostlib.resource`:

```c
/* Set / get the macOS DEFAULT OUTPUT device master volume. scalar 0.0..1.0.
   Returns 0 on success, nonzero on failure. No entitlement required. */
int ca_set_host_volume(float scalar);
int ca_get_host_volume(float *outScalar);
```

Implementation (Apple `[PUB]`):
1. `AudioObjectGetPropertyData` on `kAudioObjectSystemObject` with
   `kAudioHardwarePropertyDefaultOutputDevice` → the default output `AudioDeviceID`.
2. `AudioObjectSetPropertyData` / `…GetPropertyData` with
   `kAudioDevicePropertyVolumeScalar` on that device (scope
   `kAudioObjectPropertyScopeOutput`). Some devices expose only per-channel scalars or
   the "virtual main volume" — query `kAudioObjectPropertyElementMain` first and fall
   back to setting each channel if main is absent (**UNVERIFIED** which Apple-Silicon
   built-in output reports; handle both). `kAudioHardwareServiceDeviceProperty_VirtualMainVolume`
   (AudioToolbox `AudioHardwareService`) is the documented convenience that picks
   main-or-per-channel automatically — prefer it if present.

The shim already owns the AudioUnit + default-output handling
(`ca_make_output_unit(kAudioUnitSubType_DefaultOutput, …)`,
`coreaudio_shim.c:228`), so the device-volume property is a small, isolated addition. It
does **not** touch the SPSC ring or the RT callback — it's a plain
get/set on a host object, serialised under `HostLib_Lock` like every other host call.
**No TCC, no entitlement:** reading/writing a device's volume property is not screen
recording, microphone, or automation — confirm in [VL3] by reading the value back.

### The bridge (reuse, not new)

- AROS→host for the *host volume* uses the **same** `hostlib.resource` →
  `libcoreaudio.dylib` → `ca_*` path the [audio sub-driver](../coreaudio-audio/spec.md)
  already defines; we add two symbols to `coreaudio.exports` and the bridge's
  function-pointer table. No new dylib, no new boundary.
- AROS→AROS for the *master volume* uses the plain `AHI_SetEffect` LVO — no host call at
  all.
- The **verification** bridge is the audio feature's render-to-WAV path verbatim
  (`ca_render_to_wav` + the WAV/RMS asserts). We assert that changing the gadget value
  changes the WAV's RMS by the right ratio. No new oracle.

### Cross-link — the host-app-shell Sound surface

The [host-app-shell](../host-app-shell/design.md) design already reserves a **Sound**
settings tab and a `CM_OPT_AUDIO_VOLUME` option (stubbed, `cocoametal.h:130`; design.md
"How each existing feature maps to the host UI"). That Mac-side control and this AROS
desktop gadget are **two faces of the same two targets**:

- The host-app-shell **Sound tab / menu-bar volume item** is the *Mac-side* affordance →
  routes to `ca_set_host_volume` (host output) and/or enqueues a `CM_EV_SETTING` the
  AROS side pulls to call `AHI_SetEffect` (AROS master). It lives on the host boundary
  (the host-shell "if a running AROS could change it from inside the guest, it does NOT
  belong in the host menu" rule puts **host output volume** firmly in the host menu, and
  **AHI in-guest volume** in the guest — exactly this gadget).
- This **AROS desktop commodity** is the *in-guest* affordance → drives `AHI_SetEffect`
  by default, with an opt-in to also nudge the host volume.

They share the same `volume_apply` semantics and the same `ca_set_host_volume` verb. The
host-app-shell doc owns the Mac-menu presentation; this doc owns the AROS gadget + the
control-point grounding + the shim verb. Built independently; either can ship first.

## Plan — spikes in the loop

One standalone host/AROS binary per marker, one PASS/FAIL the agent greps, no human, no
TCC. Marker family **`[VL*]`**. The oracle is the audio feature's render-to-WAV + RMS
assertion (reused), plus a host-volume read-back.

- **[VL1] host volume get/set round-trips (pure host).** Extend the shim; a host probe
  reads the current default-output volume (`ca_get_host_volume`), sets it to a known
  scalar (`ca_set_host_volume(0.5)`), reads it back, asserts it matches within tolerance,
  then restores the original. PASS = read-back equals the set value; no TCC prompt
  hit. Proves the host verb + the no-entitlement claim, before any AROS. *(grounds the
  host target, like the audio feature's [A1].)*
- **[VL2] AHI master volume scales the mixed PCM (the core, AROS-side, WAV oracle).**
  Drive a known sine through the AHI mixer (reusing the audio feature's [A2]/[A5] pull),
  render to WAV at `ahiemv_Volume = 0x10000` (unity) and again at `0x8000` (50%) via
  `AHI_SetEffect(AHIET_MASTERVOLUME)`. PASS = `RMS(50%) ≈ 0.5 × RMS(100%)` within
  tolerance, and `RMS(0) ≈ 0`. **This is the numeric oracle for the whole feature.** It
  also settles the `AHIACF_POSTPROC` caveat: if the ratio does *not* track, post-proc is
  off for this path and we switch to the sub-driver-gain fallback (and re-assert the
  ratio there). *(reuses [coreaudio-audio §Verification].)*
- **[VL3] host volume changed, read back two-sided.** From the gadget's `volume_apply(50,
  alsoHost=TRUE)` path, set host volume; an independent host read (`ca_get_host_volume`
  *and* a second process / `AudioObjectGetPropertyData` re-read) confirms the macOS
  default-output device volume actually changed, then restore. PASS = independent
  read-back matches. (Mirrors the host-volume feature's two-sided assert.)
- **[VL4] `volume_apply` end-to-end logic.** Call the gadget's apply function with a
  sweep of percentages (0, 25, 50, 75, 100), each driving a fresh WAV render; assert the
  RMS sequence is monotonic and the 50% point halves the 100% point. PASS = monotone +
  the half-ratio. This proves the gadget's *logic* without painting the slider — the
  GUI-free core. (mirrors the host-shell `[SET]` "invoke the action selector directly"
  oracle.)
- **[VL5] graft: the commodity on the desktop.** Build the real Commodities commodity +
  MUI slider; boot windowed; drive it via [`aros-ctl`](../control-harness/README.md)
  (hotkey to open the slider, move the slider) and capture a **screenshot** as secondary
  proof the gadget paints. The PASS verdict stays the numeric WAV-RMS check from [VL2]/
  [VL4] driven through the live gadget; the screenshot is corroboration, not the oracle.
  Rides the AHI sub-driver graft ([A5]) and the muimaster/Commodities stack.

Build/run in the existing harness style (`make hosted-coreaudio`/`make coreaudio-abi`
for the host verbs, `make audio-smoke` style for the grafted path), clean-exit on PASS.

## How we verify it unattended

No human listens; no TCC / Screen-Recording / Microphone prompt is ever hit. Two
numeric oracles, both reused:

1. **The WAV-RMS oracle (primary).** Identical to [coreaudio-audio](../coreaudio-audio/spec.md)
   §Verification — render the AHI mixer output to a WAV (`ca_render_to_wav` or the
   Filesave path) and assert RMS / single-bin FFT. The *volume-specific* assertion is the
   **ratio**: `RMS at gadget=50% ≈ 0.5 × RMS at gadget=100%`, `RMS at 0% ≈ 0`. A single
   sine pins amplitude and frequency, so one PASS/FAIL covers "the gadget scaled the
   actual mixer output by the requested amount."
2. **The host-volume read-back oracle (for the optional host target).** Set the macOS
   default-output volume via the new shim verb, then read it back independently and assert
   it changed to the requested scalar. Pure get/set on a host property, no audio capture.
3. **The screenshot (secondary, [VL5] only).** The control harness captures the live
   slider via the offscreen oracle (TCC-free,
   [control-harness/README.md](../control-harness/README.md)) to corroborate that the
   gadget paints and tracks — never the verdict.

## Risks & open questions

- **`AHIACF_POSTPROC` gating (the headline risk).** The master-volume effect only scales
  samples through `DoMasterVolume`, which runs **only when post-processing is active**
  for that `AudioCtrl` (`mixer.c:1045`), and V5's `update_MasterVolume` does *not* push
  it into per-channel scaling (`effectinit.c:62–80`). If post-proc is off for the
  desktop's audio path, the AHI master volume silently does nothing. **Mitigation:**
  [VL2] proves whether the ratio tracks; if not, drive volume in **our own CoreAudio
  sub-driver** (apply a gain in the int16→host copy, below every `AudioCtrl`) as the
  guaranteed control point, keeping the AHI effect as the "also scales the in-AROS mix"
  enhancement. **UNVERIFIED** until [VL2].
- **Which system `AudioCtrl` to drive.** The AHI effect is per-`AudioCtrl`; a desktop
  master slider wants *the* output. **Open:** is there one canonical system `AudioCtrl`,
  or must the gadget enumerate open units? The sub-driver-gain fallback sidesteps this
  (it is below all `AudioCtrl`s). Decide at graft.
- **Above-unity boost.** The effect's autodoc range allows `>1.0` (channels/hw-channels,
  `sound.c:566`). We restrict the gadget to `0.0…1.0` to avoid clipping; boost is out of
  scope.
- **Host device-volume property variance.** Not every output device exposes a single
  "main" volume scalar; some are per-channel only, some report no settable volume (e.g.
  HDMI/optical fixed output). **UNVERIFIED** what the Apple-Silicon built-in output
  reports; the shim must handle main-volume, per-channel, and "no settable volume"
  (return failure, leave the AROS volume as the only target). Prefer
  `AudioHardwareService`'s `VirtualMainVolume` if available.
- **Mute semantics.** Mute = save-and-zero (AHI: `ahiemv_Volume = 0`; host:
  `kAudioDevicePropertyMute` if present, else scalar 0). Restoring exactly needs storing
  the pre-mute value. Trivial but stated so it isn't lost.
- **Gadget home / packaging.** Commodity vs Prefs applet vs screen-bar gadget — chosen
  Commodity, but the exact install location and whether it auto-starts from
  `WBStartup`/the Startup-Sequence is a graft decision.
- **No new W^X / RT-thread concern.** This feature adds no executable-memory generation
  and no new host thread; the host verb is a synchronous property get/set. The only
  inherited RT concern is the audio feature's, untouched here.
- **The graft, not a spike.** [VL5] (the real commodity in the AROS tree) depends on the
  muimaster/Commodities stack building for `aarch64-darwin` and on the CoreAudio
  sub-driver graft ([A5]). [VL1]–[VL4] are session-sized and stand alone on the host
  verbs + the AHI mixer pull.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- AHI public API: `workbench/devs/AHI/Include/SFD/ahi_lib.sfd`
  (`AHI_SetEffect` :31, `AHI_SetVol` :26, `AHI_AllocAudioA` :14, `AHI_Play` :56).
- Master-volume effect struct + effect codes:
  `workbench/devs/AHI/Include/C/devices/ahi.h` (`struct AHIEffMasterVolume` :94,
  `AHIET_MASTERVOLUME` :301, `AHIET_CANCEL` :300, `AHIST_S16S`).
- The control point in the device/mixer:
  `workbench/devs/AHI/Device/sound.c` (`_AHI_SetEffect` master-volume case :706–722, the
  autodoc range :552–572),
  `workbench/devs/AHI/Device/mixer.c` (`DoMasterVolume` :1080–1147, the post-proc gate
  :1045–1048),
  `workbench/devs/AHI/Device/effectinit.c` (`update_MasterVolume` V5 unity force :54–80),
  `workbench/devs/AHI/Device/devcommands.c` (`UpdateMasterVolume` audio.device mapping
  :1516, :1594–1632),
  `workbench/devs/AHI/Device/audioctrl.c` (default `ahiac_SetMasterVolume = 0x00010000`
  :192–193),
  `workbench/devs/AHI/Device/ahi_def.h` (`ahiac_SetMasterVolume`/`ahiac_MasterVolume`
  :219–221).
- The slider widget + MUI app skeleton: `workbench/libs/muimaster/classes/slider.c`
  (Slider over Numeric), `developer/debug/test/Zune/busy.c:43,53` (`SliderObject` +
  `MUIA_Numeric_Value` + `MUIM_Notify`), `workbench/system/SysMon/main.c:143,410`
  (`ApplicationObject`/`MUIM_Application_NewInput` loop, `<proto/muimaster.h>`),
  `workbench/devs/AHI/AHI/gui_mui.c:381–402` (the AHI prefs GUI's own volume sliders).
- The Commodities model: `compiler/include/libraries/commodities.h` (`struct NewBroker`
  :25, `NB_VERSION` :39, `NBU_UNIQUE`/`COF_*` :41–48, `CBERR_OK` :55),
  `workbench/tools/commodities/Exchange.c` (the broker-list app precedent),
  `compiler/include/workbench/workbench.h:294` (`AddAppIconA` tags — the rejected option).

This repo (`/Users/user/Source/aros-aarch64`):
- The CoreAudio shim to extend: `hosted/coreaudio/coreaudio_shim.{c,h}`
  (`ca_make_output_unit(kAudioUnitSubType_DefaultOutput,…)` :228, `ca_enable_live_output`
  :245, the `ca_*` ABI), `hosted/coreaudio/coreaudio.exports` (add the two new symbols).
- The verification harness reused: `docs/features/coreaudio-audio/{design,spec,README}.md`
  ([A1]–[A5], `ca_render_to_wav`, the WAV/RMS oracle), `make {hosted-coreaudio,
  coreaudio-abi,audio-smoke}`.
- The control harness for the [VL5] screenshot: `graft/aros-ctl`,
  `docs/features/control-harness/README.md`.
- The Mac-side counterpart: `docs/features/host-app-shell/design.md` (Sound tab,
  `CM_OPT_AUDIO_VOLUME`, the host-vs-guest ownership rule).
- The gap map: `docs/features/darwin-aarch64-port-inventory.md` (§7 Audio).

Apple frameworks (public API, `[PUB]` — link against, implement to the interface):
- CoreAudio `AudioObjectGetPropertyData` / `AudioObjectSetPropertyData`,
  `kAudioObjectSystemObject`, `kAudioHardwarePropertyDefaultOutputDevice`,
  `kAudioDevicePropertyVolumeScalar`, `kAudioDevicePropertyMute`,
  `kAudioObjectPropertyScopeOutput`, `kAudioObjectPropertyElementMain`; AudioToolbox
  `AudioHardwareServiceSetPropertyData` / `kAudioHardwareServiceDeviceProperty_VirtualMainVolume`.

External prior art (web, not in the AROS tree, no implementation source read):
- Every desktop OS ships a master-volume slider (macOS menu-bar Sound item; Linux panel
  applets); on classic Amiga a "volume commodity" was a common shareware shape — the
  Commodities broker pattern we implement to from the published `NewBroker` API.
- macOS master volume = the default output device's `kAudioDevicePropertyVolumeScalar`
  (Apple CoreAudio docs) — the property our host verb reads/writes; no entitlement.
