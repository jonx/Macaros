# CoreMIDI MIDI — host-backed MIDI for AROS

> Status: **planned (not started)** · Target: aarch64-darwin hosted · Drafted 2026-06-28

The MIDI sibling of [coreaudio-audio](../coreaudio-audio/README.md): bridge AROS's
MIDI stack to the Mac's MIDI framework so AROS programs can send and receive MIDI
through real macOS endpoints.

## What & why

AROS MIDI is `camd.library` (Commodore-Amiga MIDI) — a complete router (clusters,
links, the MIDI parser, SysEx reassembly, running-status handling) with a
**pluggable driver model**: each MIDI backend is a small loadable file in
`DEVS:Midi/` that exports the `MidiDeviceData` contract. What's missing on this port
is a driver that talks to **CoreMIDI**, Apple's userspace MIDI framework.

So this feature is **one new CAMD driver** (`coremidi`) plus a host shim
(`libcoremidi_shim.dylib`) that owns the CoreMIDI objects, reached through
`hostlib.resource` — the project thesis again: *macOS owns the drivers; AROS reaches
them via standard exec I/O.*

Two in-tree templates already exist — `workbench/devs/midi/debugdriver.c` (no-op) and
`arch/all-unix/devs/midi/hostmidi.c` (a hosted driver over `/dev/midi`). Both
implement only the **outbound** (AROS→host) half and drop camd's inbound
`receiverfunc`. The new and load-bearing part here is the **inbound** path:
CoreMIDI delivers incoming MIDI on **its own thread** via a read-callback, and
camd's `Receiver()` ends in `exec.Signal` — which a foreign host thread must not call
on darwin. We cross that boundary the same way the audio RT callback does: a
lock-free SPSC ring in host memory plus an AROS task that polls it and is the only
thing that calls into camd. See [host-wake-pattern.md](../host-wake-pattern.md).

## Status

Planning only. No code yet — design + spec drafted for owner review.

- The host CoreMIDI shim (`libcoremidi_shim.dylib`) — not started.
- The AROS `coremidi` CAMD driver — not started.
- Biggest open risk: whether camd's `LoadSeg`-and-scan-the-seglist driver discovery
  works for a natively-built aarch64 driver file (the native-`LoadSeg` gap). Second:
  the inbound poll-tick latency vs. live MIDI playability.

## Verification (planned)

Unattended, headless, no MIDI hardware, no TCC prompt: the shim creates a **virtual
CoreMIDI source + destination** in one client, sends a known MIDI byte sequence one
way and captures it the other, and asserts **exact byte equality**, ordering, SysEx
framing, zero inbound-ring overflow, and zero AROS calls from the read-proc thread.
Spikes are one host binary per marker with greppable `[MD1]`…`[MD5]` PASS/FAIL,
run through `harness/run-hosted.sh`, mirroring the audio `[A*]` set.

## Links

- [design.md](design.md) — the why, the grounded camd driver contract, the spike plan
- [spec.md](spec.md) — implementation spec (provenance-tagged) for a fresh implementer
- [coreaudio-audio](../coreaudio-audio/README.md) — the sibling feature (same boundary)
- [host-wake-pattern.md](../host-wake-pattern.md) — the host-thread→AROS-task contract
- [CLEANROOM.md](../CLEANROOM.md) — the independent-work process governing spec.md
