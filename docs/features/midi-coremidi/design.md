# CoreMIDI — host-backed MIDI for AROS (camd.library driver)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS real MIDI I/O on the Mac. AROS already has a complete
retargetable MIDI stack — `camd.library` (the Commodore-Amiga MIDI library) with
its software router (clusters, links, parser, running-status reassembly, SysEx
handling) — and a **pluggable driver model**: each MIDI hardware/host target is a
small loadable file that exports a `MidiDeviceData` contract. What's missing is the
last hop: a driver that takes camd's outbound byte stream to a real macOS MIDI
endpoint, and feeds inbound macOS MIDI back into camd. We write that one driver,
backed by **CoreMIDI**.

This is the MIDI sibling of [coreaudio-audio](../coreaudio-audio/design.md): the
same shape (a host-backed AROS subsystem reached through a flat C ABI shim resolved
via `hostlib.resource`), the same hard design point (a **host thread → AROS**
callback boundary — CoreMIDI delivers incoming MIDI on *its own* thread via a
read-proc, exactly as the CoreAudio RT render callback drains the audio ring on its
own thread), and the same unattended-verification stance (in-process loopback +
byte assertions, no human, no TCC prompt).

Why it's the right shape for this project: it's the MIDI instance of the standing
thesis — *"macOS owns the drivers; AROS reaches them via standard exec I/O."* The
Mac owns CoreMIDI; AROS reaches it through the camd driver contract (the
`MidiDeviceData` `OpenPort`/`ClosePort` + `Transmitter`/`Receiver` callback ABI). It
re-uses the host-call boundary de-risked by H3 (`hosted/abishim.S`) and the
`hostlib.resource` symbol-resolution mechanism, and the slave-process + signal
machinery proven by H9/H10 (`hosted/signal.c`, `hosted/msgport.c`).

The genuinely *new* design point — and the reason this earns a doc — is the
**inbound callback boundary**. The two camd drivers that already exist in the tree
(`debugdriver`, `hostmidi`) only ever implement the **outbound** (AROS→host) half:
each stores the `transmitfunc` and throws the `receiverfunc` away
(`workbench/devs/midi/debugdriver.c:146`,
`arch/all-unix/devs/midi/hostmidi.c:210`). CoreMIDI's read-proc runs on a CoreMIDI
host thread and must hand bytes *into* camd via the `Receiver()` callback — which
is `exec.Signal`-driven (`workbench/libs/camd/midifromdriver.c:469`). That crosses
the same host-thread→AROS-Signal seam the audio RT thread faces, and on darwin the
[host-wake-pattern](../host-wake-pattern.md) DARWIN caveat applies: a foreign host
thread must **not** call `exec.Signal` here. The bulk of the Design section is about
crossing that boundary safely.

## Does it already exist?

**Partly — the *outbound* skeleton exists; CoreMIDI and the *inbound* path do not.**

- This repo: `grep -rniE 'coremidi|camd|MIDIClient|MIDIReceived|MIDIPacket'` over
  `.` returns nothing. No CoreMIDI shim, no MIDI work.
- Upstream: `grep -rniE 'coremidi|MIDIClientCreate|MIDIReceived'` over
  `../aros-upstream` returns nothing — AROS has never had a macOS
  MIDI backend.
- Upstream **does** ship every piece we mirror or reuse:
  - the whole router: `workbench/libs/camd/` (clusters, links, the inbound parser
    `midifromdriver.c`, the outbound serialiser `miditodriver.c`, the receiver
    process `receiverproc.c`, driver discovery/load `init.c`/`drivers.c`/
    `openmididevice.c`);
  - the public driver-side contract: `compiler/include/midi/camddevices.h`
    (`struct MidiDeviceData`, `struct MidiPortData`, `MDD_Magic`);
  - **two driver templates** — `workbench/devs/midi/debugdriver.c` (a no-op
    reference) and **`arch/all-unix/devs/midi/hostmidi.c`** (a *hosted* driver that
    opens `/dev/midi` through `unixio.hidd` and writes outbound bytes). `hostmidi`
    is the closest structural precedent: a hosted CAMD driver. But it is
    OSS-`/dev/midi`-only (no macOS), **transmit-only** (drops `receiverfunc`,
    `hostmidi.c:210`), and **single-port** (`NUMPORTS 1`).

So the work is: write **one new CAMD driver** (`CoreMIDI`) plus its host shim
`libcoremidi_shim.dylib`, and — the new part — wire the **inbound** read-proc path
that neither in-tree driver implements. The camd router, parser, and discovery are
reused unchanged.

**External prior art (web-grounded, *not* in the AROS tree).** CoreMIDI is Apple's
standard userspace MIDI framework (`MIDIClientCreate`, virtual endpoints via
`MIDISourceCreate`/`MIDIDestinationCreate`, input via
`MIDIInputPortCreate`/`MIDIPortConnectSource`, output via `MIDISend`/
`MIDISendEventList`, delivery via the `MIDIReadProc`/`MIDIReceiveBlock`). The
historical AROS-darwin hosted ports (i386/x86_64/PPC) require X11 and ship no MIDI
backend; CoreMIDI is the macOS sibling none of them needed. No usable third-party
CoreMIDI-for-AROS code exists; this is independent work. (See CLEANROOM.md.)

## Background: the AROS MIDI driver contract (grounded)

A CAMD driver is **not** a device or a library — it is a plain loadable file whose
seglist contains a `struct MidiDeviceData` blob. Everything below is read from the
tree.

### How camd discovers and loads a driver `[AROS]`

`InitCamd` (`workbench/libs/camd/init.c:40–55`) does, at library init:

```c
lock = Lock("devs:Midi", ACCESS_READ);          /* the DEVS:Midi/ drawer */
Examine(lock, &fib);
while (ExNext(lock, &fib)) {
    mysprintf(..., "devs:Midi/%s", fib.fib_FileName);
    LoadDriver(temp, CamdBase);                   /* one file = one driver */
}
```

So **drivers live in `DEVS:Midi/`** and are discovered by directory scan — there is
no config file listing them (the existing drivers build into `$(AROS_DEVS)/Midi`,
`workbench/devs/midi/mmakefile.src:5`, `arch/all-unix/devs/midi/mmakefile.src:4`).

`LoadDriver` (`drivers.c:226`) → `OpenMidiDevice(name)` (`openmididevice.c:26`)
`LoadSeg`s the file and walks the seglist hunk bytes looking for a `MidiDeviceData`
whose `Magic == MDD_Magic` (`'MDEV'`, `camddevices.h:62`), whose function pointers
all lie inside the seglist (`isPointerInSeglist`, `openmididevice.c:210`), and whose
`Name` matches the filename (`openmididevice.c:104`). On AROS the driver **must**
set `Flags & 1` (the "new format" flag) or it is rejected (`drivers.c:240–244`).
`LoadDriver` then calls `mididevicedata->Init(SysBase)` (`drivers.c:251`, register
`a6`) and `AllocDriverData` (`drivers.c:74`) — one `DriverData` per port, each with
an in-cluster (`<name>.in.<port>`) and out-cluster (`<name>.out.<port>`).

**Driver-discovery requirement:** the driver file's basename, its
`mididevicedata.Name`, and the version `IDString` must agree, and the file must land
in `DEVS:Midi/`. (See risk: the seglist-scan loader is m68k-hunk-shaped; **whether
`LoadSeg`-of-a-native-driver + the seglist walk work on aarch64-darwin is
UNVERIFIED** — same native-`LoadSeg` gap the JIT/boot work tracks.)

### The `MidiDeviceData` contract `[AROS]` (`compiler/include/midi/camddevices.h:22`)

```c
struct MidiDeviceData {
    ULONG Magic;                 /* = MDD_Magic ('MDEV') */
    char *Name;                  /* must equal the filename ("coremidi") */
    char *IDString;              /* "$VER: ..." */
    UWORD Version, Revision;
    BOOL  (ASM *Init)(REG(a6) APTR SysBase);          /* after LoadSeg */
    void  (*Expunge)(void);                            /* before UnLoadSeg */
    struct MidiPortData *(ASM *OpenPort)(              /* per port, on open */
        REG(a3) struct MidiDeviceData *data,
        REG(d0) LONG portnum,
        REG(a0) ULONG (* ASM transmitfunc)(APTR REG(a2) userdata),
        REG(a1) void  (* ASM receivefunc)(UWORD REG(d0) input, APTR REG(a2) userdata),
        REG(a2) APTR userdata);
    void  (ASM *ClosePort)(REG(a3) struct MidiDeviceData *data, REG(d0) LONG portnum);
    UBYTE NPorts;                /* how many ports this driver exposes */
    UBYTE Flags;                /* MUST be 1 on AROS (new format) */
};
struct MidiPortData { void (* ASM ActivateXmit)(APTR REG(a2) userdata, ULONG REG(d0) portnum); };
```

The **register-arg ABI is m68k** (`REG(a3)`, `REG(d0)`, `REG(a0)` …). On AArch64
those `REG()` annotations collapse to the standard AAPCS64 register order (the
crosstools macro maps them) — this is the same situation the audio `HookEntry`
thunk handles; we follow the in-tree driver shape verbatim and let the crosstools
emit the call. (The shim never sees these — they are AROS-internal; the shim sees
only our flat C ABI.)

### Outbound (AROS → host) — the easy *pull* direction `[AROS]`

When an AROS program `PutMidi`s/`PutSysEx`s/`ParseMidi`s a message destined for our
driver's out-cluster, camd serialises it into the driver's ring and calls
`ActivateXmit(userdata, portnum)` (`miditodriver.c:297`, the `MidiPortData` hook the
driver returned from `OpenPort`). `ActivateXmit` then **pulls** bytes by repeatedly
calling the `transmitfunc` camd handed us at `OpenPort` until it returns the
sentinel `0x100` ("buffer empty") — exactly the loop in `hostmidi.c:177–189` and
`debugdriver.c:121–127`:

```c
for (;;) {
    ULONG data = TransmitFunc(userdata);   /* one byte, or 0x100 = done */
    if (data == 0x100) return;
    /* hand `data` (a MIDI byte) to the host */
}
```

camd's serialiser handles running-status expansion and realtime-message interleave
(`miditodriver.c` `Transmit_Status`/`Transmit_Datas`/`Transmit_SysEx`); the driver
just relays the byte stream. So outbound is a **synchronous pull on an AROS task** —
no thread boundary, no host callback. This is the trivial half (and the only half
`hostmidi`/`debugdriver` implement).

### Inbound (host → AROS) — the *new* push direction `[AROS]`

camd hands the driver, at `OpenPort`, a `receivefunc`:
`void Receiver(UWORD input, APTR userdata)` (register `d0`, `a2`). The driver calls
it **one MIDI byte at a time** as bytes arrive from the hardware. Inside camd,
`Receiver()` (`midifromdriver.c:458`) writes the byte into the driver's `re_*` ring
and `Signal()`s the per-driver **ReceiverProc** (`midifromdriver.c:463–469`):

```c
SAVEDS void ASM Receiver(REG(d0) UWORD input, REG(a2) struct DriverData *driverdata){
    *driverdata->re_write = input; driverdata->re_write++;       /* wrap at re_end */
    driverdata->unpicked++;
    Signal(&driverdata->ReceiverProc->pr_Task, 1L<<driverdata->ReceiverSig);
}
```

The ReceiverProc (`receiverproc.c:16`, a `CreateNewProcTags` process at priority 36)
`Wait()`s on that signal and drains the ring through camd's MIDI state machine
(`Receiver_first` → `Input_Treat`, running-status reassembly, SysEx capture) into
the in-cluster's midinodes. **So camd's own design already expects the driver to be
the thing that calls `Receiver()` from wherever its bytes physically arrive.** The
two existing drivers never do (they have no input). Ours must — and the bytes
physically arrive on a **CoreMIDI host thread**.

That is the crux: **`Receiver()` ends in `exec.Signal`**, which a foreign host
thread must not call on darwin (host-wake-pattern DARWIN caveat). The Design's
inbound section is entirely about getting CoreMIDI's thread's bytes to camd's
`Receiver()` *on an AROS task*, never from the CoreMIDI thread directly.

### Reference points already de-risked in this repo `[OURS]`

- **Host-call boundary** — H3 proved Apple's variadic-args-on-stack ABI and built
  the marshaller (`hosted/abishim.S`). Any varargs host call goes through it
  (CoreMIDI's API is fixed-arg, so this is low-touch).
- **Host symbol resolution** — `hostlib.resource`
  (`Host_HostLib_Open`/`GetPointer`, `arch/all-unix/bootstrap/hostlib.h`). The
  CoreMIDI bridge `dlopen`s `libcoremidi_shim.dylib` and fills a `CM_*`
  function-pointer table, exactly as the bsdsocket/coreaudio shims do
  (`hosted/coreaudio/`, `hosted/bsdsocket/`).
- **Host thread → AROS wake** — the shared contract
  [host-wake-pattern.md](../host-wake-pattern.md): atomics not `volatile`, declared
  ownership, re-probe, and the darwin caveat (poll a `Delay()` tick, do **not**
  `Signal` from the host thread). The CoreAudio RT callback is the strictest
  sibling; CoreMIDI's read-proc is the same shape.

## Design

### Host side (the CoreMIDI shim, `libcoremidi_shim.dylib`)

Native arm64 C (built with host clang, **not** AROS crosstools), peer of
`hosted/coreaudio/coreaudio_shim.c`. It owns every CoreMIDI object and a pair of
lock-free SPSC ring buffers, and exposes a flat `CM_*` C ABI (full list in
[spec.md](spec.md)). It pulls **no** AROS headers. Reached via `hostlib.resource`.

CoreMIDI objects the shim creates:

- **`MIDIClientCreate`** once per shim — the process's MIDI client `[PUB]`.
- For **verification and for app integration**, a pair of **virtual endpoints**:
  `MIDISourceCreate` (a virtual *source* AROS publishes — outbound AROS MIDI appears
  here to other Mac apps, and to our own loopback) and `MIDIDestinationCreate` (a
  virtual *destination* AROS subscribes to — inbound MIDI other apps send to AROS
  lands here) `[PUB]`. Virtual endpoints need no hardware and no TCC prompt — this
  is what makes the loopback test fully in-process and headless.
- For **real hardware/other-app I/O**, an **input port** (`MIDIInputPortCreate`,
  with a read-proc) connected to external sources (`MIDIPortConnectSource` over
  `MIDIGetNumberOfSources`/`MIDIGetSource`), and an **output port**
  (`MIDIOutputPortCreate`) to send to external destinations. Optional for the first
  cut; the virtual-endpoint pair is the core.

**Outbound (shim ← AROS).** `cm_send(port, bytes, len)` is called on the AROS task
(from `ActivateXmit`'s pull loop, batched — see "running status" below). The shim
packs the bytes into a `MIDIPacketList`/`MIDIEventList` and calls `MIDIReceived`
(for the virtual source) and/or `MIDISend` (to a connected destination) `[PUB]`.
This is a synchronous host call under `HostLib_Lock` — the easy direction.

**Inbound (shim → AROS) — the read-proc.** CoreMIDI invokes the shim's
`MIDIReadProc`/`MIDIReceiveBlock` **on a CoreMIDI-owned thread** whenever MIDI
arrives at our virtual destination or a connected input source `[PUB]`. The
read-proc **must not** call any AROS code. It only **un-packs** the
`MIDIPacketList`/`MIDIEventList` into raw MIDI bytes and **pushes them into an
inbound SPSC ring** (host memory, owned by the shim) — `cm_inbound_push`,
internal — then returns. That is all it does. The AROS side drains the ring (below).

### AROS side (the `CoreMIDI` camd driver)

A new driver directory **`workbench/devs/midi/CoreMIDI/`** (or a single
`coremidi.c` beside `debugdriver.c`), built to `DEVS:Midi/coremidi`. Modelled on
`hostmidi.c`, which is the in-tree hosted-driver template. It is host-agnostic AROS
code except that its backend calls the `CM_*` ABI instead of `unixio.hidd`:

- **`mididevicedata`** — the `MDD_Magic` blob: `Name="coremidi"`, `Flags=1`,
  `NPorts` = the number of MIDI ports we expose (start at 1; raise once multi-port
  is proven), `Init`, `Expunge`, `OpenPort`, `ClosePort`.
- **`Init(SysBase)`** — open `hostlib.resource`, `HostLib_Open` the shim, fill the
  `CM_*` table, `cm_open()` (creates the MIDI client + virtual endpoints). Mirror
  `hostmidi.c:105` but against the shim, not `unixio.hidd`.
- **`OpenPort(...)`** — store **both** `transmitfunc` *and* `receivefunc` +
  `userdata` for this port (the latter is what `hostmidi`/`debugdriver` throw away),
  return our `MidiPortData{ActivateXmit}`. Spawn / arm the **inbound drain task**
  (below) for this port if not already running.
- **`ActivateXmit(userdata, portnum)`** — the outbound pull loop:
  `data = TransmitFunc(userdata)` until `0x100`, accumulating bytes into a small
  stack buffer, then `cm_send(port, buf, n)` once per drained batch (one host call
  per `ActivateXmit`, not per byte — see "running status").
- **`ClosePort` / `Expunge`** — stop the drain task, `cm_close()`, close the shim.

**The inbound drain task (the boundary, darwin-shaped).** A high-priority AROS
Process (one per open driver, like camd's own ReceiverProc) whose body is:

```
loop:
    n = cm_inbound_pop(port, buf, sizeof buf)   /* non-blocking host call, drains the shim's inbound ring */
    for (i = 0; i < n; i++)
        Receiver(buf[i], userdata);             /* camd LVO — runs on THIS AROS task, legal */
    if (n == 0)
        Delay(ticks)                            /* darwin host-wake caveat: POLL, don't Wait-on-host-Signal */
    if (SIGBREAKF_CTRL_C) break;
```

The drain task is an **AROS task**, so calling `Receiver()` (→ `exec.Signal` of
camd's ReceiverProc) is legal — the Signal originates on the AROS thread, never on
the CoreMIDI thread. This is the host-wake-pattern darwin seam: the **CoreMIDI
read-proc** (host thread) sets data into the SPSC ring + an `_Atomic` "data ready"
flag; the **drain task** (AROS thread) polls that flag on a `timer.device`
`Delay()` tick (the clipboard bridge polls at ~5 Hz, input at ~50 Hz — pick a
MIDI-appropriate rate; see risks) and, when set, drains and feeds `Receiver()`. No
host thread ever touches an AROS LVO.

### The bridge (the two SPSC rings across the CoreMIDI-thread boundary)

This is the load-bearing piece, and it is the audio spec's R-RT/R-RING rules applied
to MIDI bytes instead of PCM frames. Two independent single-producer/single-consumer
rings in shim host memory:

- **Inbound ring** — producer = the CoreMIDI **read-proc thread**; consumer = the
  AROS **drain task**. Carries raw MIDI bytes (with packet/timestamp framing —
  see SysEx). This is the genuinely new direction.
- **Outbound** is *not* a cross-thread ring: `cm_send` runs synchronously on the
  AROS task under `HostLib_Lock` and calls `MIDIReceived`/`MIDISend` inline. (A ring
  would only be needed if we wanted to decouple send from a host send-thread; we do
  not — CoreMIDI's send is non-blocking enough.)

Hard rules for the CoreMIDI read-proc thread, mirroring the audio R-RT rules
(host-wake-pattern "The RT exception"):

- **R-MD-RT1 — the read-proc calls NO AROS LVO.** It only un-packs CoreMIDI packets
  and writes the inbound ring (plain host memory). Justification: `[OURS]` AROS
  scheduler/allocator are single-thread-safe only; `[PUB]` and CoreMIDI's read
  thread is a foreign thread. A guard counter (`rtAROSCalls`, must stay 0) proves it.
- **R-MD-RT2 — the read-proc never blocks.** No mutex, no `Wait`. On ring-full it
  drops/counts overflow (a `recvOverflow` counter; camd has its own `CMEF_RecvOverflow`
  but ours is the host-side guard). Justification: `[PUB]` a CoreMIDI read-proc must
  return promptly.
- **R-MD-RT3 — signal-mask hygiene.** Any thread CoreMIDI spawns must be born with
  AROS signals masked so AROS's `SIGALRM` scheduler tick / `SIGSEGV` trap never land
  on it — the same `pthread_sigmask(SIG_BLOCK, all)` guard the audio bridge applies
  and the Alsa bridge precedent uses. CoreMIDI creates its own thread internally; we
  cannot wrap its birth, so the guard is applied where we *can*: we never let the
  read-proc do anything that could take an AROS signal, and we do **not** rely on
  masking the CoreMIDI thread (it is Apple's). Restated as: the read-proc does only
  ring writes; it never enters AROS, so a stray AROS signal has nothing to corrupt.
  (**UNVERIFIED**: whether CoreMIDI's read thread inherits the process signal mask
  on Apple Silicon — flagged; the no-AROS-work rule makes it moot for correctness.)

SPSC ordering (R-MD-RING, the audio R-RING1 precedent `[PUB]`): `_Atomic` head/tail
indices, release on publish / acquire on read, power-of-two capacity so wrap is a
mask, no CAS, no lock held across the boundary.

### SysEx and running status (the MIDI-specific subtleties)

- **Outbound running status.** camd's serialiser already expands running status and
  computes message lengths (`miditodriver.c` `GetMsgLen`, `Transmit_Status`) — the
  driver receives a clean per-byte stream from `transmitfunc`. We batch one
  `ActivateXmit` drain into one `cm_send` so CoreMIDI gets whole messages per packet
  where possible. (CoreMIDI packets want complete messages; mid-message split across
  packets is legal but we avoid it by draining to `0x100` first.)
- **SysEx (long messages).** Outbound SysEx arrives byte-by-byte through the same
  pull loop (camd's `Transmit_SysEx`, `miditodriver.c:92`), `0xF0 … 0xF7`. We
  accumulate to `0xF7` and emit as one (or chunked) `MIDIPacket` — CoreMIDI packets
  cap at 65536 bytes, so a long dump must be **chunked across packets** with care
  not to split a packet mid-running-status. Inbound SysEx arrives in the read-proc
  possibly **split across several `MIDIPacket`s / callbacks**; the inbound ring
  carries the raw bytes in order and camd's own `Receiver_SysEx` state machine
  (`midifromdriver.c:195`) reassembles `0xF0 … 0xF7` — so the driver does **not**
  need to reassemble SysEx itself, it just preserves byte order into `Receiver()`.
  That is a real win: camd owns SysEx reassembly; we own only byte transport.
- **Realtime bytes** (`0xF8`–`0xFF`) may interleave anywhere; both camd directions
  already handle them (`miditodriver.c:196` realtime buffer, `midifromdriver.c:438`
  `Receiver_RealTime`). The driver preserves them inline in the byte stream.

### Timestamps

CoreMIDI packets/events carry a host timestamp (`mach_absolute_time` units) `[PUB]`.
camd's `Receiver()` ABI is **byte-only — it carries no timestamp** (the timestamp a
midinode sees comes from camd's own `mi_TimeStamp`, `camd.h:147`, set by the
ReceiverProc when it drains). So inbound CoreMIDI timestamps are **dropped at the
driver boundary** for the first cut (camd timestamps on arrival into its ring). This
matches every existing camd driver. Outbound, `cm_send` may pass `mach_absolute_time`
"now" (immediate delivery) — sequencing/scheduling ahead is out of scope. **Risk:**
jitter from the poll-tick drain (below) vs. CoreMIDI's precise timestamps — accept
for v1, note as the precision ceiling.

## Plan — spikes in the loop

Each marker is a standalone host binary (one-binary-per-marker, like `hosted/*.c`
and the audio `[A*]` set) with a single greppable `[MD*]` PASS/FAIL verdict the
agent reads — no human, no MIDI hardware, no TCC. Verification is **in-process
CoreMIDI loopback**: the shim creates a virtual source + virtual destination,
connects them (or drives both ends itself), sends a known MIDI byte sequence and
asserts the captured bytes match.

- **[MD1] host loopback, asserted (pure shim, no AROS).** `cm_open()` creates a
  virtual source + virtual destination in the same client and connects the input
  port to the source. Send a known sequence (Note On, CC, a short SysEx, a realtime
  byte) via `cm_send`; the read-proc captures it into the inbound ring; the test
  pops and asserts **exact byte equality**, correct ordering, SysEx framing intact,
  `recvOverflow == 0`, `rtAROSCalls == 0`. Proves the CoreMIDI round-trip + the SPSC
  ring + the assertion harness before any AROS is involved. (Grounds the loop, like
  the audio `[A1]`.)
- **[MD2] ABI through the dylib boundary.** Build `build/libcoremidi.dylib`, link
  [MD1] through `dlopen`/`HostLib_GetPointer`-style symbol resolution, verify the
  exported `CM_*` surface and re-run the [MD1] loopback across the dylib boundary.
  (Mirrors `coreaudio-abi`.)
- **[MD3] outbound: AROS pull-loop → host, asserted.** Stand up the driver's
  `ActivateXmit` pull-loop shape against a stub `transmitfunc` that yields a known
  message then `0x100`; assert the shim's virtual source emitted exactly those
  bytes (captured via a loopback input port). Proves the easy direction end-to-end
  through the real driver code path.
- **[MD4] inbound: host read-proc → AROS `Receiver()` across the boundary,
  file/counter-checked.** Wire the inbound ring (producer = a real second host
  pthread standing in for the CoreMIDI read-proc) to a consumer that calls a
  **stub `Receiver()`** (counts bytes, checksums them) on the *consumer* thread.
  PASS = consumer's captured bytes equal the producer's input **bit-for-bit**
  (checksum), zero ring corruption, `rtAROSCalls == 0` (the producer thread never
  called the stub `Receiver`), across thousands of bytes under load. Proves the
  *boundary* — the genuinely new risk — without AROS. (Mirrors audio `[A3]`.)
- **[MD5] graft: real `CoreMIDI` driver in the AROS tree.** Build
  `workbench/devs/midi/CoreMIDI/` → `DEVS:Midi/coremidi`, boot AROS, open
  `camd.library`, create a midinode, `AddMidiLink` to `coremidi.out.0` /
  `coremidi.in.0`, `PutMidi` a message and assert (via the shim's loopback capture,
  re-read from the Mac side) it reached CoreMIDI; inject a message host-side and
  assert `GetMidi`/`WaitMidi` returns it on the AROS midinode. Full thesis
  end-to-end. Rides the crosstools graft + native-`LoadSeg`-for-drivers, not a
  session-sized spike.

Build/run them in the existing harness (`make hosted-coremidi` → `[MD*]` markers via
`harness/run-hosted.sh`), clean-exit on PASS.

## How we verify it unattended

No human listens; no TCC prompt is ever hit. Virtual CoreMIDI endpoints need no
hardware and no entitlement (they are ordinary process-level MIDI objects). The
oracle is **in-process loopback + byte assertions**:

1. **Primary path: CoreMIDI virtual source ⇄ virtual destination** in one client.
   Every spike sends a known byte sequence one way and captures it the other, and
   asserts:
   - **exact byte equality** of the captured stream (the only correct oracle for
     MIDI — every byte matters);
   - **ordering** preserved (running status / realtime interleave intact);
   - **SysEx framing** intact (`0xF0 … 0xF7`, possibly across packets);
   - **`recvOverflow == 0`** (the inbound ring never dropped) and
     **`rtAROSCalls == 0`** (the read-proc thread never entered AROS) from the shim's
     stat counters.
2. **The boundary test [MD4]** uses a real second host pthread + checksum, exactly
   as audio `[A3]` proves the SPSC ring without hardware.
3. **The graft [MD5]** drives `camd.library` from a booted AROS via the control
   harness, and the agent re-reads the captured bytes on the Mac side — the
   two-sided assertion proven by H11 / the host-volume spikes.

A known **short, fixed MIDI sequence** (Note On + CC + a small SysEx + a realtime
byte) is the test vector throughout — it exercises channel-message length handling,
running status, SysEx framing, and realtime interleave in one shot, so a single
PASS/FAIL covers "right bytes, right order, right framing, no drops".

## Risks & open questions

- **Inbound thread boundary (the headline risk).** CoreMIDI's read-proc is a real
  foreign thread, and camd's `Receiver()` ends in `exec.Signal`, which a host thread
  must not call on darwin (host-wake-pattern DARWIN caveat,
  `arch/all-darwin/hidd/cocoa/cocoa_input.c:546`). Mitigation = inbound SPSC ring in
  host memory + the iron rule *the read-proc calls no AROS LVO* + an AROS drain task
  that polls the ring on a `Delay()` tick and is the only thing that calls
  `Receiver()`. [MD4] exists specifically to prove this. **Open:** poll rate vs.
  latency — MIDI wants low latency (sub-10 ms feels tight); the clipboard's 5 Hz is
  far too slow for live playing. A faster tick (e.g. 200–500 Hz) or, if
  host-context `Signal` ever proves safe on this port, a real wake, is the escape
  hatch. Measured in [MD5]. **UNVERIFIED.**
- **Native-driver `LoadSeg` on aarch64-darwin (the graft gating).** camd discovers
  drivers by `LoadSeg`-ing the file and **scanning the seglist bytes** for the
  `MidiDeviceData` magic (`openmididevice.c:70–155`) — an m68k-hunk-shaped loader.
  Whether a natively-built aarch64 driver file produces a seglist this scan can walk
  is **UNVERIFIED** — the same native-`LoadSeg` gap the boot/JIT work tracks. If it
  fails, the driver may need to be a resident/co-built module rather than a
  `DEVS:Midi/` file. Flagged as the biggest graft risk.
- **SysEx chunking.** Outbound long SysEx must be chunked across `MIDIPacket`s
  (≤ 65536 B each) without splitting running status; inbound SysEx arrives split
  across callbacks and is reassembled by camd, not us — so the driver must preserve
  byte order exactly and never coalesce/drop a partial packet. Inbound ring sizing
  must hold a worst-case burst (a full SysEx dump) without overflow; size it to tens
  of KB. **Open:** ring depth vs. burst size — measured in [MD1]/[MD4].
- **Timestamp loss.** camd's `Receiver()` carries no timestamp, so CoreMIDI's
  precise per-event host timestamps are dropped at the driver edge and camd
  timestamps on drain (jittered by the poll tick). Acceptable for v1 (matches all
  existing drivers); the precision ceiling, documented. **Open** whether a
  later timestamp-preserving side channel is worth it.
- **`hostlib.resource` vs framework symbols.** Like CoreAudio, build a small native
  `libcoremidi_shim.dylib` exporting the `CM_*` verbs and `dlopen` *that*, rather
  than resolving raw `MIDIClientCreate`/`MIDIReceived` framework symbols — cleaner,
  and matches the coreaudio/bsdsocket precedent. Decided: the shim.
- **Multi-port.** camd allocates one `DriverData` (with in/out clusters) per
  `NPorts`. Start with `NPorts=1` (one virtual source + destination) and raise once
  the single-port path is proven; each extra port = another endpoint pair. Low risk,
  staged.
- **Apple variadic ABI.** CoreMIDI's API is fixed-arg, so the H3 variadic shim is
  only needed for any logging path. Low risk, already de-risked.

## References

AROS upstream (`../aros-upstream`):
- Driver contract: `compiler/include/midi/camddevices.h` (`MidiDeviceData`,
  `MidiPortData`, `MDD_Magic` :62), `compiler/include/midi/camd.h`
  (`MidiMsg`/`MidiNode`/`MidiCluster`/`MidiLink`, `CMEF_*`),
  `compiler/include/midi/mididefs.h`.
- Driver discovery/load: `workbench/libs/camd/init.c` (`DEVS:Midi/` scan :40–55),
  `workbench/libs/camd/drivers.c` (`LoadDriver` :226, `AllocDriverData` :74,
  `OpenDriver`/`CreateReceiverProc` :24), `workbench/libs/camd/openmididevice.c`
  (`LoadSeg` + seglist magic scan :70–155, `isPointerInSeglist` :210).
- Outbound path: `workbench/libs/camd/miditodriver.c` (`ActivateXmit` pull,
  `Transmit_*` :92–179, `GetMsgLen` :36, `Midi2Driver_internal` :265),
  `workbench/libs/camd/midi2driver.c`, `workbench/libs/camd/goodputmidi.c`,
  `workbench/libs/camd/putsysex.c` (:17).
- Inbound path: `workbench/libs/camd/midifromdriver.c` (`Receiver` :458 →
  `Signal` :469, the `Receiver_*` state machine, `Receiver_SysEx` :195,
  `Receiver_RealTime` :388), `workbench/libs/camd/receiverproc.c`
  (`ReceiverFunc`/`CreateReceiverProc` :16/:51), `workbench/libs/camd/parsemidi.c`.
- `DriverData` / driver-private structs: `workbench/libs/camd/camd_intern.h`
  (`struct DriverData` :94, the `re_*` ring :154, `ReceiverProc`/`ReceiverSig`
  :151–152, `transmitfunc`/`Input_Treat` :134/:143).
- Driver templates: `workbench/devs/midi/debugdriver.c` (no-op reference,
  drops `receiverfunc` :146), `arch/all-unix/devs/midi/hostmidi.c` (hosted driver
  over `unixio.hidd`, transmit-only :210, `NUMPORTS 1`),
  `workbench/devs/midi/mmakefile.src` + `arch/all-unix/devs/midi/mmakefile.src`
  (build to `$(AROS_DEVS)/Midi`).
- Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h`
  (`Host_HostLib_Open`/`GetPointer`), `arch/all-hosted/hostlib/`.

This repo (`.`):
- Sibling feature: `docs/features/coreaudio-audio/{design.md,spec.md,README.md}`
  (the host-thread-callback boundary precedent, the SPSC R-RT/R-RING rules, the
  shim shape), `hosted/coreaudio/{coreaudio_shim.c,coreaudio_shim.h}`.
- Shared boundary contract: `docs/features/host-wake-pattern.md` (atomics, ownership,
  re-probe, the darwin "poll a `Delay()` tick, don't host-`Signal`" caveat).
- Host shim precedents: `hosted/bsdsocket/` (host pump, `hostlib` wiring,
  `bsdsock_host.h`), `hosted/clipboard/` (NSPasteboard poll on a `Delay()` tick).
- Grounding: `NOTES.md` (H3 `hosted/abishim.S` variadic ABI; H9/H10
  `hosted/signal.c`/`hosted/msgport.c` Wait/Signal + ports; `hostlib.resource`
  `HostLib_Open`/`GetPointer`), `docs/features/darwin-aarch64-port-inventory.md`
  (the gap map; MIDI joins audio/sockets as a host-backed subsystem).

External prior art (web, not in the AROS tree):
- Apple CoreMIDI framework: `MIDIClientCreate`, `MIDISourceCreate`/
  `MIDIDestinationCreate` (virtual endpoints), `MIDIInputPortCreate` /
  `MIDIPortConnectSource`, `MIDIReceived` / `MIDISend` / `MIDISendEventList`,
  `MIDIPacketList` / `MIDIEventList`, the `MIDIReadProc` / `MIDIReceiveBlock`
  read-callback contract (delivery on a CoreMIDI thread). MIDI 1.0 message grammar
  (status/data byte ranges, running status, SysEx `0xF0…0xF7`, realtime
  `0xF8…0xFF`).
- Historical AROS-darwin ports (i386/x86_64/PPC): X11-bound, ship no MIDI backend —
  confirms macOS AROS never had MIDI.
