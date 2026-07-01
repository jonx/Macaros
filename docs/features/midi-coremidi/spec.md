# Implementation spec — CoreMIDI-backed camd.library driver (host MIDI)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from this
spec + the approved sources cited by tag: `[PUB]` Apple CoreMIDI framework docs /
MIDI 1.0 specification / POSIX / published standards, `[AROS]` in-tree AROS headers
and drivers (paths given; APL/LGPL — ours), `[OURS]` this project's spikes (the
H-series, `hosted/*`, the coreaudio/bsdsocket shims). `[DERIVED]` items are
independently-derived requirements flagged for extra verification; each stands
solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification — implement from that
justification, never from any reference. No identifier name, call sequence, file
layout, or buffer-management algorithm in this spec derives from any third-party
implementation.

## Scope

**In.** A hosted **CAMD MIDI driver** for `aarch64-darwin` that: is discovered and
loaded by AROS's `camd.library` from the `DEVS:Midi/` drawer; exports the
`MidiDeviceData` driver contract; relays AROS-outbound MIDI bytes (pulled
synchronously on an AROS task via camd's `transmitfunc`) to a macOS **CoreMIDI**
endpoint; and feeds host-inbound MIDI bytes — delivered on CoreMIDI's own read
thread — back into camd's `Receiver()` callback **via a single-producer /
single-consumer lock-free inbound ring** in host memory drained by an AROS task, so
the host thread never calls an AROS LVO. The host CoreMIDI code lives in a native
`libcoremidi_shim.dylib` reached through `hostlib.resource`. Verification is
**in-process CoreMIDI loopback** (virtual source ⇄ virtual destination) + exact-byte
assertions, headless and TCC-free.

**Decision.** **Hook `camd.library`'s pluggable driver model, not AHI / a new
device.** AROS MIDI *is* camd; a MIDI backend is a `DEVS:Midi/` driver file, exactly
as the in-tree `debugdriver` and `hostmidi` are (`[AROS]`). We write **one** new
driver, `coremidi`, modelled on the hosted `hostmidi` template, and reuse the camd
router/parser/discovery unchanged.

**Decision.** **CoreMIDI virtual endpoints** (`MIDISourceCreate` +
`MIDIDestinationCreate`) are the core surface — they need no hardware and no TCC, so
the unattended loopback test is fully in-process `[PUB]`. Real-hardware input/output
ports (`MIDIInputPortCreate`/`MIDIPortConnectSource`, `MIDIOutputPortCreate`) are a
documented extension, not the first cut.

**Out (non-goals, this spec).** MIDI 2.0 / UMP (we target MIDI 1.0 byte streams);
timestamp-accurate scheduling / send-ahead (immediate delivery only — camd's
`Receiver()` carries no timestamp); MTC/MMC sync; multi-port beyond a staged raise of
`NPorts`; a host *send* thread (outbound is a synchronous pull, no ring needed);
preserving CoreMIDI per-event host timestamps into camd (dropped at the driver edge,
matching every existing camd driver).

## Architecture

Three layers. The AROS driver and the CoreMIDI host shim are joined by a **flat
hand-written C ABI** (the ABI header is ours, ASCII, independently authored); the
driver couples to camd through the in-tree `MidiDeviceData` contract.

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple toolchain)
┌──────────────────────────────────────┐             ┌────────────────────────────┐
│ camd.library  (router/parser)  [AROS] │             │ libcoremidi_shim.dylib     │
│   (reused unchanged)                  │             │  [OURS]                    │
│   · DEVS:Midi/ scan → LoadDriver      │  hostlib +  │  · MIDIClientCreate        │
│   · Receiver() ←─ ReceiverProc Signal │  H3 host-   │  · virtual source + dest   │
│        ▲                              │  call ABI   │  · MIDIReadProc (HOST       │
│ CoreMIDI driver  [AROS-shaped]        │ ──────────► │      THREAD!)              │
│   · MidiDeviceData (Init/OpenPort/…)  │   CM_* C ABI│  · inbound SPSC ring       │
│   · ActivateXmit pull → cm_send  ─────┼──────────►  │  · cm_send → MIDIReceived/ │
│   · inbound DRAIN TASK (AROS)         │ ◄────────── │      MIDISend              │
│        │ cm_inbound_pop, Receiver()   │ inbound ring│  · loopback capture (test) │
│        ▼                              │             └────────────────────────────┘
│ camd in-cluster midinodes  [AROS]     │  inbound ring lives in host memory, owned by shim
└──────────────────────────────────────┘
```

- **Host shim** `[OURS]` — native arm64 C (`.c`), built with the **host** clang
  (NOT AROS crosstools), peer of `hosted/coreaudio/coreaudio_shim.c`. It owns every
  CoreMIDI object and the inbound SPSC ring, and exposes the `CM_*` C ABI below. It
  pulls **no** AROS headers. Reached via `hostlib.resource` (`HostLib_Open` of the
  dylib + `HostLib_GetPointer` per symbol) `[AROS]`, exactly as the coreaudio /
  bsdsocket bridges resolve their shims (`hosted/coreaudio/`, `hosted/bsdsocket/`).
- **AROS driver** `[AROS]` — the `MidiDeviceData` blob + `Init`/`OpenPort`/
  `ClosePort` + `ActivateXmit` + the inbound drain task, modelled on
  `arch/all-unix/devs/midi/hostmidi.c`. Host-agnostic AROS code; the only file
  naming CoreMIDI symbols is the shim.
- Spike-phase paths: shim in `hosted/coremidi/`; at graft, the AROS side lands in
  the proposed `workbench/devs/midi/CoreMIDI/` building to `DEVS:Midi/coremidi`.

## The C ABI (`coremidi_shim.h`)

Hand-authored, neutral. Verbs mirror the *role* of the coreaudio shim's opaque
handle API (`ca_open`/`ca_set_format`/`ca_start`/`ca_stop`/`ca_close` + ring verbs +
`ca_get_stats`) `[OURS]` — that shape is the in-tree precedent. `[PUB]` CoreMIDI
objects under the hood; the ring API is `[OURS]` (SPSC theory). The shim is the
**only** owner of the inbound ring and the only caller of CoreMIDI.

```c
typedef struct CMContext CMContext;

/* Open: MIDIClientCreate + one virtual source + one virtual destination, allocate
   the inbound SPSC ring sized for `ringBytes`. For the loopback test the read-proc
   is attached so bytes sent to the source reappear on the inbound ring. Returns
   NULL on failure. */
CMContext *cm_open(int nPorts, int ringBytes);

/* Optional: connect to real external endpoints (MIDIInputPortCreate +
   MIDIPortConnectSource over MIDIGetSource; MIDIOutputPortCreate). Off by default so
   the proof stays in-process. Returns 0 on success. */
int  cm_connect_hardware(CMContext *, int enable);

/* OUTBOUND (called only on the AROS task, from ActivateXmit's pull loop — the single
   sender). Send `len` MIDI bytes for `port` (a complete drained batch: one or more
   whole messages, running-status-expanded by camd). The shim packs them into a
   MIDIPacketList/MIDIEventList and calls MIDIReceived (virtual source) and/or
   MIDISend (connected destination). Non-blocking. Returns 0 on success. */
int  cm_send(CMContext *, int port, const unsigned char *bytes, int len);

/* INBOUND (called only on the AROS drain task — the single consumer). Pop up to
   `max` MIDI bytes for `port` from the inbound ring into `buf`, in arrival order.
   Non-blocking. Returns the number of bytes popped (0 when the ring is empty).
   Never calls into CoreMIDI. */
int  cm_inbound_pop(CMContext *, int port, unsigned char *buf, int max);

/* INBOUND advisory: bytes available right now (may only grow after this returns,
   never shrink, since the drain task is the only reader). Lets the drain task decide
   whether to drain now or park one tick. */
int  cm_inbound_avail(CMContext *, int port);

/* Diagnostics for the unattended oracle: monotonic counters the shim maintains.
     sent        — bytes accepted by cm_send over the run.
     received    — bytes the read-proc wrote into the inbound ring.
     popped      — bytes cm_inbound_pop handed to the drain task.
     recvOverflow— times the read-proc found the inbound ring full and dropped bytes
                   (MUST be 0 for a PASS over a correctly-sized run).
     rtAROSCalls — MUST stay 0: a guard the read-proc increments if it ever calls an
                   AROS LVO (it never does; structurally 0). */
typedef struct { unsigned long sent, received, popped, recvOverflow, rtAROSCalls; } CMStats;
void cm_get_stats(CMContext *, CMStats *out);

void cm_close(CMContext *);
```

The header is shared source, hand-written, independent work. The shim must not
include AROS headers; the AROS side must not include CoreMIDI headers. The `CM_*`
ABI is the only contact surface.

## The portable camd contracts this binds to (grounded, `[AROS]`)

Restated from the headers and the live source so the implementer needs no
third-party source.

### Discovery & load — `[AROS]`

- **R-DISC1 — drawer + name.** camd discovers drivers by scanning the `DEVS:Midi/`
  drawer (`Lock("devs:Midi")` + `Examine`/`ExNext`, `workbench/libs/camd/init.c:40–
  55`) `[AROS]`. The driver file basename, the `mididevicedata.Name` field, and the
  filename passed to `OpenMidiDevice` **must be the same string** — the loader
  rejects a mismatch (`mystrcmp(findonlyfilename(name), mididevicedata->Name)`,
  `openmididevice.c:104`). **Requirement:** ship the file as `DEVS:Midi/coremidi` with
  `mididevicedata.Name = "coremidi"`.
- **R-DISC2 — the magic blob.** `OpenMidiDevice` `LoadSeg`s the file and scans the
  seglist bytes for a `struct MidiDeviceData` with `Magic == MDD_Magic`
  (`'M'<<24|'D'<<16|'E'<<8|'V'`, `camddevices.h:62`) all of whose function pointers
  lie inside the seglist (`isPointerInSeglist`, `openmididevice.c:99–155`) `[AROS]`.
  **Requirement:** declare the blob as a single `const struct MidiDeviceData` so it
  and the functions it points at land in the loaded image — mirror
  `hostmidi.c:82–95`.
- **R-DISC3 — new-format flag.** On AROS the driver **must** set `Flags = 1`
  (new format) or `LoadDriver` rejects it (`drivers.c:240–244`) `[AROS]`.
- **UNVERIFIED (graft-gating):** whether a natively-built aarch64 driver file yields
  a seglist the byte-scan loader can walk on this port — the native-`LoadSeg` gap.
  If it fails, the driver becomes a resident/co-built module; flagged in design.md.

### The `MidiDeviceData` contract — `[AROS]` (`compiler/include/midi/camddevices.h:22`)

The driver exports exactly this struct and these entry points (register args are the
m68k `REG()` annotations the header dictates; the AArch64 crosstools map them to
AAPCS64 — same situation as the audio HookEntry thunk; follow `hostmidi.c` verbatim):

- **R-MDD1 — `Init(REG(a6) APTR SysBase)` → BOOL.** Called once after `LoadSeg`
  (`drivers.c:251`). Store `SysBase`; open `hostlib.resource`; `HostLib_Open`
  `libcoremidi_shim.dylib`; fill the `CM_*` function-pointer table via
  `HostLib_GetPointer`; `cm_open(NPorts, ringBytes)`. Return FALSE (closing what was
  opened) on any failure — mirror `hostmidi.c:105–145` but against the shim, not
  `unixio.hidd`.
- **R-MDD2 — `OpenPort(data a3, portnum d0, transmitfunc a0, receivefunc a1,
  userdata a2)` → `MidiPortData *`.** Called per port when a client uses the driver.
  **Store BOTH `transmitfunc` AND `receivefunc` AND `userdata` for this port** — the
  existing drivers store only `transmitfunc` and drop `receivefunc`
  (`hostmidi.c:210`, `debugdriver.c:146`); ours needs `receivefunc` for inbound.
  Start/arm the inbound drain task (R-IN) for this port. Return the static
  `MidiPortData{ActivateXmit}` (`hostmidi.c:192`). `portnum` is **1-based** at this
  edge (the existing drivers index `UserData[portnum-1]`, `hostmidi.c:213`).
- **R-MDD3 — `ClosePort(data a3, portnum d0)`.** Stop this port's drain task; mark
  the port closed. (camd may reopen.) Mirror `hostmidi.c:224` (a no-op there because
  it has no per-port state to tear down; we tear down the drain task).
- **R-MDD4 — `Expunge(void)`.** Stop all drain tasks; `cm_close()`; `HostLib_Close`
  the shim; close `hostlib.resource`. Called before `UnLoadSeg` (`init.c:95`).
- **R-MDD5 — `ActivateXmit(userdata a2, portnum d0)`** (the `MidiPortData` hook).
  See R-OUT.

### Outbound (AROS → host) — the pull direction, `[AROS]`

camd serialises an outbound message into the driver's ring and calls `ActivateXmit`
(`miditodriver.c:297`). running-status expansion, length computation, and
realtime/SysEx interleave are **camd's job** (`miditodriver.c` `Transmit_Status`
:127 / `Transmit_Datas` :105 / `Transmit_SysEx` :92, `GetMsgLen` :36) — the driver
receives a clean per-byte stream.

- **R-OUT1 — the pull loop.** In `ActivateXmit`, loop `data = transmitfunc(userdata)`
  until `data == 0x100` (the "buffer empty" sentinel, `miditodriver.c:220`),
  accumulating each low byte (`data & 0xff`) into a stack buffer. Exactly the loop
  shape of `hostmidi.c:177–189` / `debugdriver.c:121–127` `[AROS]`.
- **R-OUT2 — batch one drain into one host call.** Call `cm_send(port, buf, n)` once
  per drained batch (after the loop hits `0x100`), not once per byte. `[DERIVED]`
  justification: `[PUB]` a CoreMIDI packet wants complete messages; draining to the
  sentinel first yields whole messages, and one host call per `ActivateXmit`
  minimises `HostLib_Lock` churn (`[AROS]` the host-call discipline). Implement from
  that justification. (Per-byte `cm_send` is also correct but wasteful; batching is
  the requirement.)
- **R-OUT3 — host-call discipline.** `cm_send` is a host call; bracket it
  `HostLib_Lock … AROS_HOST_BARRIER … HostLib_Unlock` per the established hosted
  discipline — **EXCEPT** confirm the darwin-aarch64 barrier rule: per the calling-
  host-libc memory note, **no `AROS_HOST_BARRIER` on aarch64** (it is a no-op /
  omitted on this port). `[OURS]` Follow whatever the coreaudio/bsdsocket shims do on
  this exact target. **UNVERIFIED** until matched against those; default to the
  coreaudio bridge's bracketing.

### Inbound (host → AROS) — the new push direction, `[AROS]` + `[PUB]` + `[OURS]`

This is the load-bearing constraint and the reason this feature earns a spec. camd
hands the driver a `receivefunc` (`Receiver(UWORD input, APTR userdata)`, register
`d0`/`a2`) and expects the driver to call it **one MIDI byte at a time** as bytes
arrive. Inside camd, `Receiver()` writes the byte into the driver's `re_*` ring and
`Signal()`s the per-driver ReceiverProc (`midifromdriver.c:458–471`) `[AROS]`. So
`Receiver()` **ends in `exec.Signal`** — which a foreign host thread must not call on
darwin (host-wake-pattern DARWIN caveat,
`arch/all-darwin/hidd/cocoa/cocoa_input.c:546`) `[OURS]`.

CoreMIDI delivers inbound MIDI via a `MIDIReadProc`/`MIDIReceiveBlock` **on a
CoreMIDI-owned thread** `[PUB]`. The whole model below keeps that thread and the AROS
task sharing **plain host memory only**, never AROS state.

#### The iron rules (the CoreMIDI read-proc) — each on `[PUB]` + `[OURS]`

- **R-IN-RT1 — the read-proc calls NO AROS LVO.** It only un-packs the
  `MIDIPacketList`/`MIDIEventList` into raw MIDI bytes and writes them into the
  inbound ring (plain host memory) via the shim's internal push. No `Receiver()`, no
  `Signal`, no allocation, no exec call. Justification: `[OURS]` the AROS scheduler/
  allocator are single-thread-safe only — an LVO from a foreign thread corrupts
  task/heap state; `[PUB]` a CoreMIDI read-proc is a callback thread that must do
  minimal work and return. A guard counter `rtAROSCalls` (asserted `== 0`) proves it
  ([MD4]).
- **R-IN-RT2 — the read-proc never blocks.** No mutex, no condition variable, no
  `Wait`. On inbound-ring-full it drops the overflow bytes and increments
  `recvOverflow`, then returns. Justification: `[PUB]` a blocked read-proc stalls
  CoreMIDI delivery for the whole client. (camd also has `CMEF_RecvOverflow`,
  `camd.h:246`, raised later on the AROS side; the shim counter is the host-side
  guard.)
- **R-IN-RT3 — signal-mask hygiene / no AROS entry.** CoreMIDI owns its read thread;
  we cannot wrap its birth with `pthread_sigmask` the way the audio bridge masks a
  thread it spawns. The requirement therefore degrades to: the read-proc does
  **only** ring writes and **never enters AROS**, so a stray AROS signal landing on
  the CoreMIDI thread has nothing to corrupt. `[DERIVED]` justification: `[OURS]` H4's
  scheduler is SIGALRM-driven on the single AROS thread, and the audio R-RT3 mask
  guard exists to keep that tick off foreign threads; since we cannot mask Apple's
  thread, we instead guarantee it touches no AROS state. **UNVERIFIED:** whether the
  CoreMIDI read thread inherits the process signal mask on Apple Silicon — flagged;
  the no-AROS-work rule makes it moot for correctness.

#### The hand-off — a single-producer / single-consumer inbound ring

`[PUB]` SPSC ring theory: with exactly one writer and one reader, a ring with
independent monotonic head/tail indices needs **no lock** — the writer only advances
`tail`, the reader only advances `head`, each publishes its index with a single
atomic store / acquire-load. The ring is **plain host memory, not AROS state**, which
is what lets R-IN-RT1 hold.

- **Producer** = the CoreMIDI **read-proc thread**, writing raw MIDI bytes in arrival
  order. One writer only.
- **Consumer** = the AROS **inbound drain task** (an AROS Process), reading bytes via
  `cm_inbound_pop` and feeding them to `Receiver()`. One reader only.

- **R-IN-RING1 — memory ordering.** `[PUB]` head/tail are `_Atomic(uint32_t)`.
  Producer: write bytes into slots, then `atomic_store_explicit(&tail, …, release)`.
  Consumer: `atomic_load_explicit(&tail, …, acquire)` before reading, then
  `atomic_store_explicit(&head, …, release)` after. Power-of-two capacity → wrap is a
  mask. No CAS, no fence beyond the release/acquire on the two indices.
- **R-IN-RING2 — the drain task (the only caller of `Receiver()`).** A high-priority
  AROS Process per open driver (like camd's own ReceiverProc, `receiverproc.c:76`,
  pri 36 — adopt a similar high priority). Body:

  ```
  loop:
      n = cm_inbound_pop(port, buf, sizeof buf);     /* non-blocking host call */
      for (i = 0; i < n; i++)
          (*receivefunc)(buf[i], userdata);          /* camd Receiver() — runs on THIS AROS task */
      if (n == 0)
          Delay(POLL_TICKS);                          /* DARWIN: poll, never Wait on a host Signal */
      if (CheckSignal(SIGBREAKF_CTRL_C)) break;
  ```

  Because the drain task is an **AROS task**, calling `Receiver()` (→ `exec.Signal`
  of camd's ReceiverProc) is legal — the Signal originates on the AROS thread `[AROS]`.
- **R-IN-RING3 — darwin wake = poll, not host-Signal (R-DARWIN-WAKE).** The CoreMIDI
  read-proc **never** wakes the drain task by `exec.Signal` (forbidden on darwin per
  the host-wake-pattern caveat) `[OURS]`. The read-proc sets an `_Atomic` "data
  ready" flag (or simply advances the ring tail, which the drain task observes via
  acquire-load); the drain task **polls** on a `timer.device` `Delay()` tick and
  re-checks `cm_inbound_avail` after each park (R-W3 re-probe). The `Signal` mapping
  stays valid on hosts where host-context `Signal` is safe; only the *delivery*
  degrades to polling on darwin. (host-wake-pattern R-W2/R-W3 + DARWIN caveat.)
- **R-IN-RING4 — poll rate / latency.** `[DERIVED]` MIDI wants low latency; the
  clipboard's ~5 Hz and input's ~50 Hz are both too slow for live playing.
  Justification: `[PUB]` perceptible MIDI latency is roughly ≤ 10 ms, so the poll
  tick should be a few ms (≈ 200–500 Hz) — but `[OURS]` the drain task competes with
  the whole single-thread scheduler, so the tick is a tunable, and the *correctness*
  oracle (byte equality, `recvOverflow == 0`) is independent of latency. Default to
  the fastest `Delay()` granularity the timer supports; measure jitter in [MD5].
  **UNVERIFIED** until measured; the escape hatch is a real `Signal` wake if/when
  host-context `Signal` is proven safe on this port.
- **R-IN-RING5 — ring sizing.** Size the inbound ring to hold a worst-case burst —
  a full SysEx dump arriving across back-to-back read-proc callbacks. `[DERIVED]`
  default = tens of KB (e.g. 64 KB, power of two); `recvOverflow` is the empirical
  judge. Justification: `[PUB]` SysEx dumps are unbounded in principle but typically
  ≤ a few KB; sizing to 64 KB rides bursts without overflow. **UNVERIFIED**, measured
  in [MD1]/[MD4].

Because exactly one CoreMIDI thread writes the inbound ring and exactly one AROS task
reads it, and the only shared object is a lock-free ring in host memory, **no AROS
lock and no host mutex is ever held across the boundary.** Outbound needs no ring:
`cm_send` runs synchronously on the AROS task (R-OUT).

## MIDI byte semantics — `[PUB]` (MIDI 1.0) + `[AROS]`

- **R-MIDI1 — byte transport, not parsing.** The driver transports raw MIDI 1.0
  bytes in both directions and does **not** parse channel messages. camd owns the
  state machine in both directions: outbound serialisation (`miditodriver.c`),
  inbound reassembly (`midifromdriver.c` `Receiver_*`). `[AROS]`
- **R-MIDI2 — SysEx (long messages).** `[PUB]` SysEx is `0xF0 … 0xF7` with all
  intermediate bytes `≤ 0x7F`. **Outbound:** accumulate from `0xF0` to `0xF7` (camd's
  `Transmit_SysEx` yields the bytes through the pull loop, `miditodriver.c:92`) and
  emit as a `MIDIPacket`; a SysEx longer than CoreMIDI's per-packet cap (65536 B,
  `[PUB]`) is **chunked across packets** preserving byte order, never splitting a
  realtime byte's surrounding running status. **Inbound:** CoreMIDI may split a SysEx
  across several read-proc callbacks `[PUB]`; the driver pushes the raw bytes into the
  inbound ring in order and **camd's `Receiver_SysEx` reassembles** them
  (`midifromdriver.c:195`) — the driver does **not** reassemble SysEx itself. `[AROS]`
- **R-MIDI3 — realtime bytes.** `[PUB]` system-realtime bytes (`0xF8`–`0xFF`) may
  appear anywhere, including inside a SysEx. The driver preserves them inline in the
  byte stream in both directions; camd handles them (`miditodriver.c:196` realtime
  buffer, `midifromdriver.c:438` `Receiver_RealTime`). `[AROS]`
- **R-MIDI4 — timestamps dropped at the edge.** `[AROS]` camd's `Receiver()` ABI
  carries no timestamp (the midinode timestamp comes from camd's own `mi_TimeStamp`,
  `camd.h:147`, set on drain). So inbound CoreMIDI host timestamps are **dropped**;
  camd timestamps on arrival into its ring. Outbound `cm_send` uses "now"
  (`mach_absolute_time`, `[PUB]`) for immediate delivery. `[DERIVED]` justification:
  matching the camd ABI (no timestamp channel) is the only faithful option for v1 and
  matches every existing camd driver; send-ahead scheduling is out of scope.

## AROS-side binding — `[AROS]`, contract from [design.md](design.md)

A new directory `workbench/devs/midi/CoreMIDI/` (or a single `coremidi.c` beside
`debugdriver.c`), building to `DEVS:Midi/coremidi`, files mirroring `hostmidi.c`:

- **`coremidi.c`** — the `MidiDeviceData` blob (R-DISC2/R-DISC3), `Init` (R-MDD1),
  `OpenPort`/`ClosePort`/`Expunge` (R-MDD2/3/4), `ActivateXmit` (R-OUT), the inbound
  drain task (R-IN-RING2), and the per-port `{transmitfunc, receivefunc, userdata,
  drainproc}` table.
- **`coremidi-bridge`** (or inline) — opens `hostlib.resource`
  (`OpenResource("hostlib.resource")`), `HostLib_Open`s `libcoremidi_shim.dylib`, and
  fills the `CM_*` table via `HostLib_GetPointer` — mirror the coreaudio/bsdsocket
  `hostlib` wiring `[AROS]`/`[OURS]`. **Decision:** build a small native
  `libcoremidi_shim.dylib` exporting the `CM_*` verbs (owning the CoreMIDI client,
  endpoints, inbound ring) and `dlopen` *that*, rather than resolving raw
  `MIDIClientCreate`/`MIDIReceived` framework symbols — cleaner, and matches the
  coreaudio precedent.
- **`mmakefile.src`** — `%build_prog … targetdir=$(AROS_DEVS)/Midi files=coremidi
  usestartup=no`, mirror `arch/all-unix/devs/midi/mmakefile.src` `[AROS]`.

## Verification (unattended — `[OURS]` H7/H11 discipline)

No human listens; no TCC prompt is ever hit (CoreMIDI virtual endpoints are
process-level objects needing no hardware and no entitlement). The oracle is
**in-process CoreMIDI loopback + exact-byte assertions**, never an ear or a human.

**The loopback rig — `[PUB]`.** The shim creates a virtual *source* and a virtual
*destination* in one `MIDIClient`; an input port connects to the source so anything
`cm_send`/`MIDIReceived` emits reappears at the read-proc and lands on the inbound
ring. The test sends a known sequence one way and pops it the other.

**The test vector** (one fixed sequence exercising every path): Note On
(`0x90 0x3C 0x40`), running-status Note On (`0x3E 0x40`), a Control Change
(`0xB0 0x07 0x7F`), a short SysEx (`0xF0 0x7D 0x01 0x02 0x03 0xF7`), and a realtime
clock byte (`0xF8`) interleaved. `[PUB]` MIDI 1.0.

**The assertions** (every marker asserts *bytes*, never "it didn't crash"):

- **exact byte equality** of the captured stream vs. the sent vector (the only
  correct MIDI oracle — every byte matters);
- **ordering** preserved (running status + realtime interleave intact);
- **SysEx framing** intact (`0xF0 … 0xF7`, even when split across packets inbound);
- **`recvOverflow == 0`** (inbound ring never dropped) and **`rtAROSCalls == 0`**
  (read-proc thread never entered AROS) from `cm_get_stats`;
- **byte checksum** for the pure-copy boundary test [MD4].

**Markers** (one host binary per marker, `[MD?]` PASS/FAIL via
`harness/run-hosted.sh`, clean-exit on PASS):

- **[MD1] host loopback, asserted.** Pure shim (no AROS): `cm_open` → loopback rig →
  `cm_send` the test vector → read-proc captures → `cm_inbound_pop` → assert exact
  bytes / order / SysEx framing / `recvOverflow==0` / `rtAROSCalls==0`. Grounds the
  CoreMIDI round-trip + SPSC ring + assert harness, like audio `[A1]`. `[MD1]`.
- **[MD2] ABI through the dylib boundary.** Build `build/libcoremidi.dylib`; link
  [MD1] through `HostLib_Open`/`HostLib_GetPointer`-style resolution; verify the
  exported `CM_*` surface and re-run the loopback across the dylib boundary. Mirrors
  `coreaudio-abi`. `[MD2]`.
- **[MD3] outbound pull-loop → host, asserted.** Drive the driver's `ActivateXmit`
  pull-loop shape (R-OUT) against a stub `transmitfunc` yielding the test vector then
  `0x100`; assert the shim's virtual source emitted exactly those bytes (captured via
  the loopback input port). Proves the easy direction through the real driver path.
  `[MD3]`.
- **[MD4] inbound boundary, checksum-checked.** Producer = a real second host pthread
  (standing in for the CoreMIDI read-proc) writing the inbound ring; consumer = a
  drain loop calling a **stub `Receiver()`** (counts + checksums bytes) on the
  consumer thread. PASS = consumer bytes == producer input **bit-for-bit**, zero ring
  corruption, `rtAROSCalls == 0` (producer thread never called the stub), across
  thousands of bytes under load. Proves the *boundary* — the real risk — with no AROS.
  Mirrors audio `[A3]`. `[MD4]`.
- **[MD5] graft: real `CoreMIDI` driver in the AROS tree.** Build
  `workbench/devs/midi/CoreMIDI/` → `DEVS:Midi/coremidi`; boot AROS; `OpenLibrary
  ("camd.library")`; `CreateMidi`; `AddMidiLink` to `coremidi.out.0` / `coremidi.in.0`;
  `PutMidi` the vector and assert (via the shim loopback capture, re-read Mac-side) it
  reached CoreMIDI; inject host-side and assert `GetMidi`/`WaitMidi` returns it on the
  midinode. Also measures the drain-task poll latency (R-IN-RING4). Full thesis
  end-to-end; rides the crosstools graft + native-`LoadSeg`-for-drivers (R-DISC2
  UNVERIFIED). `[MD5]`.

## Build / integration

- Shim `libcoremidi_shim.dylib` links `CoreMIDI, CoreFoundation`; built with host
  clang `-arch arm64`, codesigned (ad-hoc fine for spikes — confirm vs. the existing
  coreaudio/bsdsocket signing path, **UNVERIFIED**), loaded via `hostlib.resource`.
- Spikes compile to Mach-O via the existing `Makefile` pattern
  (`make hosted-coremidi` → builds `build/host-coremidi*` →
  `harness/run-hosted.sh '[MD?] …'` searches stdout for the marker, returns the
  uniform `result=(PASS|FAIL)` block) `[OURS]`. `make coremidi-abi` builds + proves
  `build/libcoremidi.dylib` like `make coreaudio-abi`.
- Deployment mirrors coreaudio: `graft/run-window.sh` / `graft/aros-ctl` copy
  `build/libcoremidi.dylib` to `~/lib/`; `graft/make-aros-app.sh` bundles it in
  `Macaros.app/Contents/Frameworks/` when present.
- The C ABI header is shared, hand-written, independent work. The shim must not link
  or include AROS headers; the AROS side must not include CoreMIDI headers.

## Open questions / UNVERIFIED

- **Native-driver `LoadSeg` on aarch64-darwin** — camd's seglist byte-scan loader
  (R-DISC2) is m68k-hunk-shaped; whether a natively-built driver file is discoverable
  this way is the biggest graft risk. If not, ship `coremidi` as a resident/co-built
  module. (design.md risks.)
- **Drain-task poll rate vs. MIDI latency** (R-IN-RING4) — default to the fastest
  `Delay()` granularity; measure jitter in [MD5]; escape hatch is a real `Signal`
  wake if host-context `Signal` becomes safe on this port.
- **Inbound ring depth vs. SysEx burst** (R-IN-RING5) — default 64 KB;
  `recvOverflow` is the judge, measured in [MD1]/[MD4].
- **CoreMIDI read-thread signal-mask inheritance on Apple Silicon** (R-IN-RT3) —
  cannot mask Apple's thread; mooted by the no-AROS-work rule, but flagged.
- **`AROS_HOST_BARRIER` on aarch64** (R-OUT3) — confirm whether the barrier is a
  no-op/omitted on this port; match the coreaudio/bsdsocket shims.
- **Codesign / entitlements for a `dlopen`'d CoreMIDI dylib** — confirm vs. the
  coreaudio path.
- **Multi-port** — start `NPorts=1`; each extra port = another virtual source/dest
  pair and another drain task; staged raise.
- **SysEx outbound chunking edge** — confirm CoreMIDI's exact per-packet cap and the
  safe split rule on this OS version.

## Provenance summary

`[PUB]` Apple CoreMIDI framework (`MIDIClientCreate`, `MIDISourceCreate`/
`MIDIDestinationCreate` virtual endpoints, `MIDIInputPortCreate`/
`MIDIPortConnectSource`, `MIDIReceived`/`MIDISend`/`MIDISendEventList`,
`MIDIPacketList`/`MIDIEventList`, the `MIDIReadProc`/`MIDIReceiveBlock` read-thread
contract, `mach_absolute_time` timestamps); MIDI 1.0 message grammar (status/data
byte ranges, running status, SysEx `0xF0…0xF7` with the 65536-byte packet cap,
realtime `0xF8…0xFF`); C11 atomics + SPSC ring-buffer theory (release/acquire,
single-word indices); POSIX threads/`pthread_sigmask`. ·
`[AROS]` `compiler/include/midi/camddevices.h` (`MidiDeviceData`/`MidiPortData`,
`MDD_Magic` :62), `compiler/include/midi/camd.h` (`MidiMsg`/`MidiNode`/
`MidiCluster`/`MidiLink`, `mi_TimeStamp` :147, `CMEF_RecvOverflow` :246),
`workbench/libs/camd/init.c` (`DEVS:Midi/` scan :40–55), `drivers.c`
(`LoadDriver` :226, new-format flag :240, `AllocDriverData` :74),
`openmididevice.c` (`LoadSeg`+magic scan :70–155, name match :104, `isPointerIn
Seglist` :210), `miditodriver.c` (`ActivateXmit` pull :297, `Transmit_*` :92–179,
`GetMsgLen` :36), `midifromdriver.c` (`Receiver` :458 → `Signal` :469,
`Receiver_SysEx` :195, `Receiver_RealTime` :388), `receiverproc.c`
(`ReceiverFunc`/`CreateReceiverProc` :16/:51), `camd_intern.h` (`DriverData` :94,
`re_*` ring :154), `workbench/devs/midi/debugdriver.c` (drops `receiverfunc` :146),
`arch/all-unix/devs/midi/hostmidi.c` (hosted driver template, transmit-only :210,
`mmakefile.src` build-to-`Midi`), `arch/all-darwin/hidd/cocoa/cocoa_input.c:546`
(the host-context `Signal` hazard), `arch/all-unix/bootstrap/hostlib.h` +
`arch/all-hosted/hostlib/` (`HostLib_Open`/`GetPointer`). ·
`[OURS]` the sibling `docs/features/coreaudio-audio/{design.md,spec.md}` (the
host-thread-callback boundary, the SPSC R-RT/R-RING rules, the shim shape) and
`hosted/coreaudio/{coreaudio_shim.c,coreaudio_shim.h}`; `docs/features/host-wake-
pattern.md` (atomics-not-volatile, ownership, re-probe, the darwin poll-don't-Signal
caveat R-W1..R-W5); `hosted/bsdsocket/` (host pump + `hostlib` wiring),
`hosted/clipboard/` (Delay-tick poll); H3 (`hosted/abishim.S`, Apple variadic ABI),
H9/H10 (`hosted/signal.c`/`hosted/msgport.c`, Wait/Signal + ports);
`harness/run-hosted.sh` marker harness; the calling-host-libc memory note (no
`AROS_HOST_BARRIER` on aarch64). ·
`[DERIVED]` independently-derived points flagged for extra verification: (a) batch
one `ActivateXmit` drain into one `cm_send` [R-OUT2]; (b) the read-proc-touches-no-
AROS-state rule substitutes for masking Apple's unmaskable read thread [R-IN-RT3];
(c) a few-ms poll tick targets ≤10 ms MIDI latency while the byte-equality oracle is
latency-independent [R-IN-RING4]; (d) a ~64 KB inbound ring rides SysEx bursts
[R-IN-RING5]; (e) dropping CoreMIDI timestamps at the edge is the only faithful
option for camd's timestamp-less `Receiver()` ABI [R-MIDI4]. Each rests wholly on its
cited `[PUB]`/`[AROS]`/`[OURS]` justification above. No third-party code,
identifiers, or call sequence used.
