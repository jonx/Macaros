# Volume / mixer gadget

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

A desktop **sound-level control** for hosted AROS — the missing volume gadget. A small
Commodities commodity with a MUI/Zune slider that, by default, drives the **AROS AHI
master volume** (so every AROS sound scales together) and, optionally, the **macOS
default output-device volume** (so it can master the whole machine).

## What & why

The hosted port has a complete AHI audio stack and — once the
[CoreAudio AHI sub-driver](../coreaudio-audio/README.md) lands — real output to the Mac's
speakers, but no one-touch level control on the desktop. This feature adds it, and
grounds the *exact* control point in the real mixer source so the slider actually changes
the sound:

- **AROS volume control point (default):** the AHI **master-volume effect** —
  `AHI_SetEffect(AHIEffMasterVolume{AHIET_MASTERVOLUME, ahiemv_Volume})`
  (`workbench/devs/AHI/Include/SFD/ahi_lib.sfd:31`,
  `Include/C/devices/ahi.h:94`), which lands in `ahiac_SetMasterVolume`
  (`Device/sound.c:709`) and is applied per-sample by `DoMasterVolume`
  (`Device/mixer.c:1080`). `0x10000` = unity, `0x8000` = half. The one real caveat —
  `DoMasterVolume` is gated on `AHIACF_POSTPROC` (`mixer.c:1045`) — has a guaranteed
  fallback: apply the gain in our own CoreAudio sub-driver, below every `AudioCtrl`.
- **Host volume control point (optional):** the macOS default output device's
  `kAudioDevicePropertyVolumeScalar`, added as a new verb
  (`ca_set_host_volume`/`ca_get_host_volume`) in the existing CoreAudio shim. No
  entitlement, no TCC.
- **The gadget:** a Commodities commodity (`NewBroker`/`CxBroker`) with a hotkey and a
  small MUI Slider — the smallest always-available desktop applet; the Zune Slider +
  volume idiom is already in-tree (`workbench/devs/AHI/AHI/gui_mui.c:381`).

This is the project thesis once more: *macOS owns the drivers; AROS reaches them via
standard exec I/O.* It **depends on** the
[CoreAudio audio](../coreaudio-audio/README.md) feature (the AHI sub-driver) and reuses
its render-to-WAV + RMS verification harness; it does not re-design the sub-driver.

## Verification (unattended, no TCC)

The numeric oracle is the audio feature's WAV-RMS harness, reused: set the gadget to a
volume V → render the AHI mixer output to a WAV → assert the RMS scales linearly
(**RMS at 50% ≈ half of RMS at 100%**, RMS at 0% ≈ 0). The optional host target is proven
by reading the macOS output-volume property back after setting it. The live gadget's
paint/track is corroborated by a control-harness screenshot (secondary). Marker family
**`[VL*]`** — one host/AROS binary per marker, build → run → one PASS/FAIL.

## Links

- [design.md](design.md) — the why, the grounded AHI/CoreAudio/Commodities contracts,
  the gadget-shape decision, the `[VL*]` spike plan.
- [spec.md](spec.md) — the implementation spec (provenance-tagged) for a fresh
  implementer: the host ABI extension, the commodity, `volume_apply`, the gain fallback.
- Foundation: [coreaudio-audio](../coreaudio-audio/README.md) — the CoreAudio-backed AHI
  sub-driver this builds on.
- Mac-side counterpart: [host-app-shell](../host-app-shell/design.md) — the Sound
  settings tab / menu-bar volume item (`CM_OPT_AUDIO_VOLUME`) that mirrors this gadget on
  the host boundary.
- Screenshot proof: [control-harness](../control-harness/README.md).
- Gap map: [darwin-aarch64 port inventory](../darwin-aarch64-port-inventory.md) (§7 Audio).
