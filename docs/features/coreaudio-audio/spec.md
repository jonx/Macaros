# Implementation spec — CoreAudio-backed AHI sub-driver (host sound)

> Status: drafting · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent,
driver, or otherwise — was read, searched, or consulted in producing it, and any
resemblance to existing implementations is coincidental.** Implement only from
this spec + the approved sources cited by tag: `[PUB]` Apple framework docs /
POSIX / published standards, `[AROS]` in-tree AROS headers and drivers (paths
given), `[OURS]` this project's spikes (the H-series, `hosted/*`). `[DERIVED]`
items are independently-derived requirements flagged for extra verification; each
stands solely on its cited `[PUB]`/`[AROS]`/`[OURS]` justification — implement from
that justification, never from any reference. No identifier name, call sequence,
file layout, or buffer-management algorithm in this spec derives from any
third-party implementation.

## Scope

**In.** A hosted AHI **sub-driver** for `aarch64-darwin` that: registers a new
output target with AROS's AHI device; pulls mixed PCM from AHI's software mixer
through the standard sub-driver Hook contract on a high-priority AROS slave
Process; hands that PCM across the boundary to a macOS **CoreAudio AUHAL** render
callback running on CoreAudio's own real-time thread, via a **single-producer /
single-consumer lock-free ring** in host memory; and ships a parallel
**Filesave-style WAV writer** path so every behaviour can be asserted unattended
(RMS / dominant-frequency FFT / byte-checksum) with no human listening and no TCC
prompt.

**Decision (confirmed with the project owner).** **Hook AHI, not `audio.device`.**
The portable `audio.device` (`workbench/devs/audio/`) is, on every non-Amiga
target, already an AHI *client* — it opens `ahi.device` and submits `AHIRequest`s
(`workbench/devs/audio/audio_commands.c:810`, `audio_intern.h:146`) `[AROS]`; its
only real hardware backend is m68k Paula. So a CoreAudio AHI sub-driver gives both
AHI apps **and** classic Paula programs sound, with zero work on `audio.device`.
We therefore write **one** new sub-driver, `workbench/devs/AHI/Drivers/CoreAudio/`,
modelled on the in-tree **Alsa** driver, and reuse the AHI device + software mixer
unchanged. Rationale in full in [design.md](design.md) ("Background: AROS audio
contracts").

**Decision.** **AUHAL** (`kAudioUnitSubType_HALOutput`) for live output — the
standard low-latency output AudioUnit with a render callback `[PUB]`. AudioQueue is
a documented fallback (see "Open questions"), not the plan.

**Out (non-goals, this spec).** Recording / input (`AHISF_CANRECORD`); hardware
mixing / DSP acceleration (`AHIsub_SetVol`/`SetFreq`/`SetSound`/`SetEffect` — we
return defaults, AHI's software mixer does all DSP); HIFI 32-bit output at first
(stereo 16-bit only — see "Sample format"); a PID controller / dynamic resampler
(noted as the escape hatch, not built); multi-channel / surround; live on-speaker
verification by a human ear (we verify by render-to-WAV + numeric assert).

## Architecture

Three layers. The AROS sub-driver and the CoreAudio host shim are joined by a
**flat hand-written C ABI** (the ABI header is ours, ASCII, independently authored);
the sub-driver couples to AHI through the in-tree AHI sub-driver Hook contract.

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple toolchain)
┌──────────────────────────────────────┐             ┌────────────────────────────┐
│ ahi.device  +  software mixer  [AROS] │             │ libcoreaudio_shim.dylib    │
│   (reused unchanged)                  │             │  [OURS]                    │
│        │  CallHookPkt(MixerFunc)      │  hostlib +  │  · AudioComponent + AUHAL  │
│        ▼                              │  H3 host-   │  · AURenderCallback (RT!)  │
│ CoreAudio sub-driver  [AROS-shaped]   │  call ABI   │  · SPSC ring (host mem)    │
│   · AHIsub_* LVOs (Alloc/Start/Stop…) │ ──────────► │  · int16→float32 convert   │
│   · high-pri slave Process (producer) │   CA_* C ABI│  · ExtAudioFile (offline   │
│        │ writes frames                │ ◄────────── │    render, for [A4])       │
│        ▼                              │  ring fill  └────────────────────────────┘
│ SPSC ring  (consumer = RT thread) ────┼──── lives in host memory, owned by shim
│   · parallel WAV writer (verify) [AROS]│
└──────────────────────────────────────┘
```

- **Host shim** `[OURS]` — native arm64 C (`.c`), built with the **host** clang
  (NOT AROS crosstools), peer of `hosted/display.c`. It owns every CoreAudio object
  and the SPSC ring, and exposes the `CA_*` C ABI below. It pulls **no** AROS
  headers. Reached via `hostlib.resource` (`dlopen` of the dylib +
  `HostLib_GetPointer` per symbol) `[AROS]`, exactly as the Alsa bridge resolves
  `libasound` (`Drivers/Alsa/alsa-bridge/alsa_hostlib.c:85`) `[AROS]`.
- **AROS sub-driver** `[AROS]` — the AHIsub LVO set + the slave Process, modelled
  on `Drivers/Alsa/`. Host-agnostic AROS code; the only file naming CoreAudio
  symbols is the shim.
- Spike-phase paths: shim in `hosted/coreaudio/`; at graft, the AROS side lands in
  the proposed `workbench/devs/AHI/Drivers/CoreAudio/`.

## The C ABI (`coreaudio_shim.h`)

Hand-authored, neutral. Verbs mirror the *role* of the Alsa bridge's opaque
handle API (`ALSA_Open`/`SetHWParams`/`Write`/`Close`) `[AROS]` — that shape is the
in-tree precedent. `[PUB]` CoreAudio objects under the hood; the ring API
is `[OURS]` (SPSC theory). The shim is the **only** owner of the ring and the RT
thread.

```c
typedef struct CAContext CAContext;

/* Open: create the AudioComponent + AUHAL output unit, NOT started; allocate the
   SPSC ring sized for `ringFrames` stereo frames. Returns NULL on failure. */
CAContext *ca_open(int ringFrames);

/* Negotiate the output rate. Caller passes the requested rate; shim sets the
   AUHAL input-scope ASBD and writes back the rate actually accepted. The shim's
   input format is int16 stereo interleaved LE (see "Sample format"); the shim
   converts to CoreAudio's float32 internally. Returns 0 on success. */
int  ca_set_format(CAContext *, unsigned *inOutRateHz);

/* Start the RT render callback (AudioOutputUnitStart). After this returns, the
   CoreAudio RT thread may call into the ring at any time. The RT thread is born
   with all AROS signals masked (see Concurrency). Returns 0 on success. */
int  ca_start(CAContext *);

/* Stop the RT render callback (AudioOutputUnitStop). After this returns the RT
   thread will not touch the ring again. */
void ca_stop(CAContext *);
void ca_close(CAContext *);

/* PRODUCER side (called only by the AROS slave task — the single producer).
   Push up to `frames` stereo int16 frames from `src` (interleaved L,R LE) into the
   ring. Non-blocking. Returns the number of frames actually accepted (< frames
   when the ring is full). Never calls into CoreAudio, never blocks. */
int  ca_ring_push(CAContext *, const short *src, int frames);

/* PRODUCER side: frames of free space in the ring right now (advisory; the value
   may only grow after this returns, never shrink, since the producer is the only
   writer). Lets the slave decide whether to push or park. */
int  ca_ring_space(CAContext *);

/* Diagnostics for the unattended oracle: monotonic counters the shim maintains.
   underruns = times the RT callback found the ring empty and emitted silence;
   rtAROSCalls MUST stay 0 (guard: the RT path called no AROS LVO). */
typedef struct { unsigned long pushed, consumed, underruns, rtAROSCalls; } CAStats;
void ca_get_stats(CAContext *, CAStats *out);

/* OFFLINE render for headless verification [A4]: pull `frames` from the ring and
   render them through an offline AudioUnit / ExtAudioFile to `wavPath` instead of
   the live device. No hardware, no TCC. Returns frames written. */
int  ca_render_to_wav(CAContext *, const char *wavPath, int frames);
```

The header is shared source, hand-written, independent work. The shim must not
include AROS headers; the AROS side must not include CoreAudio headers. The `CA_*`
ABI is the only contact surface.

## Concurrency model — the RT-thread ↔ AROS SPSC ring (the load-bearing constraint)

This is the one genuinely new risk and the reason this feature earns a spec. Every
prior hosted feature was **pull** (AROS asks the host and blocks for a reply,
serialised under `HostLib_Lock`). Audio inverts it: CoreAudio spins up its **own
real-time host thread** and *calls us* when it wants samples. AROS's hosted
scheduler runs all AROS tasks on a **single underlying OS thread** (H4/H6) and, by
the H6 result, treats `Forbid`/`Permit` as a mere *compiler* barrier precisely
because no second thread races AROS state `[OURS]`. The RT thread violates that
assumption. The whole model below exists to make the two threads share **plain
host memory only**, never AROS state.

### The iron rules (the RT callback)

`[PUB]` Apple's AURenderCallback runs on a real-time render thread; Apple's own
contract forbids, inside it: blocking, locks/mutexes, memory allocation,
Objective-C messaging, and system calls. `[OURS]` AROS state is not thread-safe
against a second real thread. Both lead to the same three rules — **R-RT1..R-RT3
each stand on `[PUB]` + `[OURS]`, not on any third-party reference:**

- **R-RT1 — the RT callback calls NO AROS LVO.** No `AllocMem`, no AROS-path
  `Signal`/`Wait`, no `Forbid`/`Permit`, no exec call of any kind. It reads only
  the ring (plain host memory) and the AUHAL `ioData`. Justification: `[OURS]` the
  AROS scheduler/allocator are single-thread-safe only; calling an LVO from a
  foreign thread corrupts task/heap state. Independently, `[PUB]` Apple forbids
  system calls on the render thread anyway. A guard counter `rtAROSCalls` (asserted
  `== 0`) proves it — see [A3].
- **R-RT2 — the RT callback never blocks.** No mutex, no condition variable, no
  `Wait`. On ring-empty it writes silence into `ioData` and increments `underruns`,
  then returns `noErr`. Justification: `[PUB]` a blocked render callback ⇒ audible
  dropout / xrun; Apple forbids blocking on this thread.
- **R-RT3 — host-thread signal-mask hygiene.** The RT thread must be **born with
  all AROS signals (SIGALRM tick, SIGSEGV trap, the lot) masked**, or it will field
  AROS's scheduler/trap signals and corrupt the scheduler. Justification:
  `[AROS]` the in-tree Alsa bridge already does exactly this guard for the host
  audio library's spawned threads — `_prepare_kernel_for_new_host_pthread`
  `sigfillset` + `sigprocmask(SIG_SETMASK, full, &saved)` around the host-thread-
  creating call, restored after (`Drivers/Alsa/alsa-bridge/alsa.c:42`, applied
  around the device-open at `:129`). `[OURS]` H4's scheduler is driven by SIGALRM
  on the single AROS thread; a foreign thread inheriting that mask would steal the
  tick. **Requirement:** wrap `AudioOutputUnitStart` (which is what spawns/arms the
  RT thread) in `sigprocmask(SIG_BLOCK, all-signals, &saved)` / restore, so the RT
  thread inherits a fully-masked set. `[DERIVED]` we independently determined that
  a callback-driven host API is the right structural target; the masking
  requirement is restated wholly from the Alsa in-tree precedent and H4 —
  implement from those.

### The hand-off — a single-producer / single-consumer lock-free ring

`[PUB]` SPSC ring-buffer theory: with exactly one writer and one reader, a ring of
`N` frames with independent monotonic head/tail indices needs **no lock** — the
writer only advances `tail`, the reader only advances `head`, and each side
publishes its index to the other with a single atomic store / acquire-load. No
mutex is ever held across the thread boundary, so neither side can block the other,
and — crucially — the ring is **plain host memory, not AROS state**, which is what
lets R-RT1 hold.

- **Producer** = the AROS slave task (the single underlying AROS thread). After
  each `MixerFunc` pull it converts int16→… (see "Sample format") and
  `ca_ring_push`es the frames. One writer only.
- **Consumer** = the CoreAudio RT thread, inside the AURenderCallback. It
  `memcpy`s from the ring into `ioData` and advances `head`. One reader only.

**R-RING1 — memory ordering.** `[PUB]` Implement head/tail as
`_Atomic(uint32_t)` (or C11 `atomic_uint`). Producer: write data into the slots,
then `atomic_store_explicit(&tail, …, memory_order_release)`. Consumer:
`atomic_load_explicit(&tail, …, memory_order_acquire)` before reading, then
`atomic_store_explicit(&head, …, memory_order_release)` after. The release/acquire
pair guarantees the consumer sees the sample bytes that were written before the
`tail` publish. Index arithmetic is modulo `N`; size the ring to a power of two so
the wrap is a mask. (Single-word atomic loads/stores; no CAS, no fences beyond the
release/acquire on the two indices.)

**R-RING2 — back-pressure (producer side, AROS).** When `ca_ring_space()` is below
one pull's worth, the slave **parks** itself for one scheduler tick rather than
spinning — mirror the Alsa slave's VBlank wait `SmallDelay()`
(`Drivers/Alsa/alsa-playslave.c:56–70`, an `INTB_VERTB` IntServer + `Wait`)
`[AROS]` — and retries next pass. The slave **never** holds a lock the RT thread
wants (there is none). It re-checks `ca_ring_space` after the park (a `volatile`/
atomic re-read, same family as H9's `volatile` re-read of `tc_State`) `[OURS]`.

**R-RING3 — under-run (consumer side, RT).** Per R-RT2: ring-empty ⇒ silence +
`underruns++`. The RT thread **never** `Signal()`s the slave awake (that is an AROS
LVO — forbidden by R-RT1). The slave wakes on its own scheduler tick and re-checks
fill level. We therefore accept **tick-granularity** producer latency (≤ one
SIGALRM period) as the safe default; a `volatile` "low-water" flag the slave polls
is an allowed optimisation but not required for correctness.

### Ring sizing

**R-RING4 — depth.** Size the ring for a **tens-of-milliseconds** cushion, not a
single pull. `[DERIVED]` a ~**40 ms** host-buffer fill is a reasonable
latency/stability magnitude. The
independent restatement: `[OURS]` the producer is paced by AROS's SIGALRM tick, so
the ring must hold **at least several tick periods** of audio to ride out
scheduling jitter without under-run; `[PUB]` at the AUHAL device's callback period
(`inNumberFrames` per call, typically 256–1024 frames ≈ 5–23 ms at 44.1 kHz) the
ring must also hold **several callback periods** so a late producer pass doesn't
starve the callback. Both independently land at **tens of ms**. Concretely: default
`ringFrames` = the next power of two ≥ `0.05 × rate` (≈ 2048 frames at 44.1 kHz,
~46 ms), tunable. The exact depth is **UNVERIFIED** until measured against the
SIGALRM period in [A4]; under-run count is the empirical judge.

Because exactly one AROS task touches the producer side and exactly one RT thread
touches the consumer side, and the only shared object is a lock-free ring in host
memory, **no AROS lock and no host mutex is ever held across the boundary.**

## Sample format / latency / resampling — `[AROS]` + `[PUB]`

**What AHI hands us.** With the flags we return (next section) AHI selects
`AHIST_S16S` = **stereo, 16-bit signed, interleaved L,R**, value `3`
(`Include/C/devices/ahi.h:322`; selected at `Device/audioctrl.c:553–554` from the
`AHIACF_STEREO` flag) `[AROS]`. On AArch64 that is **little-endian** int16, frame
size **4 bytes**. This is the easy case (no HIFI). Sizing fields, all `[AROS]`:

- `ahiac_BuffSamples` = frames the mixer produces per pass =
  `MixFreq/PlayerFreq`, clamped to a WORD (`Device/audioctrl.c:77–83`).
- `ahiac_BuffSize` = bytes to `AllocVec` for the mix buffer =
  `BuffSamples × frameSize`, rounded up to a multiple of 8 + 80 pad bytes
  (`Device/audioctrl.c:87–97`). Allocate exactly this for the slave's mix buffer.
- `ahiac_MixFreq` = the actual output rate in Hz; `AHIsub_AllocAudio` reads the
  request here and writes back what the host negotiated (mirror
  `Drivers/Alsa/alsa-main.c:105,139`) `[AROS]`.

**Conversion.** Unlike ALSA (which takes `S16_LE` directly), CoreAudio's canonical
AudioUnit format is **float32** `[PUB]`. So the producer converts each int16 sample
to float by `x * (1.0f / 32768.0f)` right after the `MixerFunc` pull — the slot
where the OSS driver does its narrowing copy (`Drivers/OSS/OSS-playslave.c:169`)
`[AROS]`. Whether to convert in the slave (before `ca_ring_push`, ring stores
float) or in the shim (ring stores int16, RT callback converts) is an
implementation choice — **default: convert in the slave so the RT callback only
`memcpy`s** (keeps the RT path as trivial as possible, honouring R-RT2). Set the
AUHAL input-scope ASBD to `kAudioFormatLinearPCM` + `kAudioFormatFlagIsFloat`,
32-bit, interleaved (or non-interleaved if the chosen ASBD wants it) `[PUB]`.
Whether AUHAL also accepts int16 input directly on Apple Silicon (which would skip
the convert) is **UNVERIFIED**; the safe default is to convert to float ourselves.

**No resampling in the driver.** AHI's mixer already outputs at `ahiac_MixFreq`;
we negotiate that exact rate with CoreAudio (`ca_set_format`) and never resample.
If host and mixer rates ever diverge (e.g. CoreAudio forces 48 kHz and AHI gave
44.1 kHz), the *first* response is to re-negotiate `ahiac_MixFreq` to the rate the
device returned (write it back so the mixer produces at the device rate), **not**
to add a resampler. A PID-controlled dynamic resampler holding a target fill
is the documented escape hatch if drift bites — **noted, not built;
out of scope** (see Scope/Out). `[DERIVED]` we independently determined such
drift-absorption is the known fallback; we instead rely on rate-match + the
tens-of-ms ring cushion.

## AROS sub-driver binding — `[AROS]`, contract from [design.md](design.md)

A new directory `workbench/devs/AHI/Drivers/CoreAudio/`, files mirroring
`Drivers/Alsa/` (`coreaudio-main.c`, `coreaudio-playslave.c`, `coreaudio-init.c`,
`DriverData.h`, `Makefile.in`, the `CoreAudio.s` driver stub, and
`coreaudio-bridge/`). The driver implements only the **software-mixing subset** of
the AHIsub LVOs (Alsa, OSS, Filesave all implement exactly this subset); the
`SetVol`/`SetFreq`/`SetSound`/`SetEffect` family are no-ops returning defaults.

**The AHIsub LVO interface** (bias 30, register args from
`Include/SFD/ahi_sub_lib.sfd`) `[AROS]`. Each is a `[AROS]` contract:

- **`AHIsub_AllocAudio(tagList a1, AudioCtrl a2)` → ULONG flags.** Allocate a
  `CoreAudioData` into `ahiac_DriverData` (`MEMF_CLEAR|MEMF_PUBLIC`); set
  `slavesignal = -1`; `mastersignal = AllocSignal(-1)`;
  `mastertask = (struct Process *)FindTask(NULL)`; `ca_open(ringFrames)`; read the
  requested rate from `ahiac_MixFreq`, `ca_set_format(&rate)`, write
  `ahiac_MixFreq = rate`. **Return
  `AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO`** (the exact word Alsa returns,
  `Drivers/Alsa/alsa-main.c:143`; values `AHISF_MIXING=1<<1`, `AHISF_TIMING=1<<2`,
  `AHISF_KNOWSTEREO=1<<3`, `Include/C/libraries/ahi_sub.h:83–85`). `AHISF_MIXING`
  ⇒ "use your software mixer, populate the Hooks for me"; `AHISF_TIMING` ⇒ "I pace
  playback myself" (the slave/ring is the clock); `AHISF_KNOWSTEREO` (no
  `AHISF_KNOWHIFI`) ⇒ `AHIST_S16S`. On failure return `AHISF_ERROR` (`1<<0`).
- **`AHIsub_FreeAudio(AudioCtrl a2)`.** Free the ring (`ca_close`), free the master
  signal, free `DriverData`.
- **`AHIsub_Disable(AudioCtrl a2)` / `AHIsub_Enable(AudioCtrl a2)`** →
  `Forbid()` / `Permit()` (`Drivers/Alsa/alsa-main.c:174,188`).
- **`AHIsub_Start(Flags d0, AudioCtrl a2)`.** On `AHISF_PLAY`: `AllocVec(
  ahiac_BuffSize, MEMF_ANY|MEMF_PUBLIC)` for the mix buffer; spawn the slave via
  `CreateNewProc` at **high priority** (Alsa uses **127**,
  `Drivers/Alsa/alsa-main.c:209` — adopt 127 so the producer out-runs ordinary
  tasks; **decision**, justification = the producer must refill before the RT
  thread drains the ring); hand the slave `AudioCtrl` in
  `pr_Task.tc_UserData`; `ca_start()`; `Signal(slave, SIGF_SINGLE)` then
  `Wait(1<<mastersignal)` for the "slave alive" handshake (pattern at
  `Drivers/Alsa/alsa-main.c:220–228`).
- **`AHIsub_Update(Flags d0, AudioCtrl a2)`** — no-op, return 0.
- **`AHIsub_Stop(Flags d0, AudioCtrl a2)`.** `Signal(slave, 1<<slavesignal)` to ask
  it to die, `Wait(1<<mastersignal)` for the dying ack
  (`Drivers/Alsa/alsa-main.c:271–276`), `ca_stop()`, `FreeVec` the mix buffer.
- **`AHIsub_GetAttr(Attribute d0, Argument d1, DefValue d2, tagList a1,
  AudioCtrl a2)`.** Advertise (mirror `Drivers/Alsa/alsa-main.c:305–352`):
  `AHIDB_Bits` → 16; `AHIDB_Frequencies` → count of our rate table
  {8000, 11025, 22050, 44100, 48000}; `AHIDB_Frequency`/`AHIDB_Index` → table
  lookups; `AHIDB_Record` → FALSE; `AHIDB_Realtime` → TRUE; `AHIDB_Outputs` → 1.
  (`AHIDB_*` tag values in `Include/C/devices/ahi.h:203–218`.)
- **`AHIsub_HardwareControl(Attribute d0, Argument d1, AudioCtrl a2)`** — no-op,
  return 0.

**The mixer-pull contract (the heart) — `[AROS]`.** AHI installs the mixer Hook in
the `AudioCtrl` (`ahiac_MixerFunc->h_Entry = HookEntryPreserveAllRegs`,
`h_SubEntry = MixerFunc`, `Device/audioctrl.c:601–602`). The slave pulls one buffer
per pass through the standard Amiga Hook ABI:

```
/* per pass, in coreaudio-playslave.c — shape per Drivers/Device/device-playslave.c:142
   and Drivers/Alsa/alsa-playslave.c:132 */
CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);        /* advance song/timing */
CallHookPkt(AudioCtrl->ahiac_MixerFunc,  AudioCtrl, mixbuffer);   /* fill mixbuffer */
samplesready = AudioCtrl->ahiac_BuffSamples;                      /* frames this pass */
```

Hook ABI `[AROS]` (`compiler/include/utility/hooks.h:82–89`):
`CallHookPkt(hook, object, message)` routes **hook→A0, object→A2, message→A1**,
returns IPTR in D0. On AArch64 our `CallHookPkt`/`HookEntry` thunk must map
**object→arg2, message→arg1** accordingly. Critically, the mixer's `h_Entry` is
`HookEntryPreserveAllRegs` because the mixer can run from interrupt context
(`Device/audioctrl.c:601`, `Device/gateway.*`) — **the AArch64 thunk must save and
restore all caller registers**, not just the standard callee-saved set `[AROS]`.
`MixerFunc` returns `void` and fills the caller's `mixbuffer`
(`Device/mixer.c:82–85`).

**The slave loop (`coreaudio-playslave.c`) — `[AROS]` shape.** `AROS_UFH3
SlaveEntry → Slave(SysBase)` (`Drivers/Device/device-playslave.c:31`). Bring-up:
allocate `slavesignal = AllocSignal(-1)`, `Signal(master, 1<<mastersignal)`
("alive"). Per pass: the two `CallHookPkt`s above → convert int16→float32 →
`ca_ring_push` (parking via R-RING2 if `ca_ring_space` is short) → loop. **Also**
(verification path, always on for spikes) `Write()` the pulled int16 frames to the
parallel WAV (next section). Exit when `signals & (SIGBREAKF_CTRL_C |
(1<<slavesignal))` (`Drivers/Alsa/alsa-playslave.c:100`), then
`Signal(master, 1<<mastersignal)` ("dying").

**`DriverData.h`** mirrors `Drivers/Alsa/DriverData.h`: a `CoreAudioBase`
(`struct DriverBase` + `DosLibrary*`) and a `CoreAudioData` (`struct DriverData` +
`BYTE mastersignal, slavesignal; struct Process *mastertask, *slavetask; APTR
mixbuffer;` + `CAContext *cahandle`) `[AROS]`.

**`CoreAudio.s` driver stub** mirrors `Drivers/Alsa/ALSA.s`: a `FORM AHIM`
with an `AUDN` chunk (`.asciz "coreaudio"`) and an `AUDM` chunk advertising
`AHIDB_AudioID` (a fresh ID), `AHIDB_Volume TRUE`, `AHIDB_Panning TRUE`,
`AHIDB_Stereo TRUE`, `AHIDB_HiFi FALSE`, `AHIDB_Name "CoreAudio:16 bit stereo"`
`[AROS]`. The AudioID must be unique vs. the existing drivers — **UNVERIFIED**
which ID is free; pick one and confirm at graft.

**`coreaudio-bridge/`** — native arm64 C, the only file naming CoreAudio symbols.
`CA_HostLib_Init` opens `hostlib.resource` (`OpenResource("hostlib.resource")`),
`HostLib_Open`s `libcoreaudio_shim.dylib`, and fills a `CA_*` function-pointer table
via `HostLib_GetPointer` (mirror `Drivers/Alsa/alsa-bridge/alsa_hostlib.c:85–106`)
`[AROS]`. **Decision:** build a small native `libcoreaudio_shim.dylib` exporting the
`CA_*` verbs (which owns the AudioUnit + ring + RT-mask guard) and `dlopen` *that*,
rather than resolving raw `AudioComponent*`/`AudioOutputUnit*` framework symbols —
cleaner, and the Alsa bridge essentially is such a shim already.

## Verification (unattended — `[OURS]` H7 discipline)

No human listens; no TCC / Screen-Recording / Microphone prompt is ever hit
(output-only playback to a file needs no entitlement). The oracle is **render-to-WAV
+ numeric assertions on the samples**, never an ear. The driver always carries a
**parallel Filesave-style WAV writer** so every spike yields a file the agent reads.

**The WAV writer — `[AROS]`, reuse the in-tree Filesave path.** Pull via the same
`CallHookPkt(ahiac_MixerFunc, AudioCtrl, mixbuffer)`, then `Write()` the int16 PCM
to a file, and on close `Seek` back and patch the RIFF/`data` size fields — exactly
`Drivers/Filesave/filesave-playslave.c:309` (pull), `:334` (write), `:521–537`
(header patch). The WAV header struct (`RIFF`/`WAVE`/`fmt `/`data`,
`formatTag=WAVE_PCM`, `numChannels`, `samplesPerSec=ahiac_MixFreq`, `avgBytesPerSec`,
`blockAlign`, `bitsPerSample=16`) is `Drivers/Filesave/FileFormats.h:154–177`
`[AROS]` — fixed by the RIFF/WAVE standard, not authored by anyone.

**The assertions** (every marker asserts *values*, never "it didn't crash"):

- WAV header well-formed; `samplesPerSec == ahiac_MixFreq`; frame count == expected.
- **RMS** within tolerance of the known input's RMS (silence → ~0; a sine of
  amplitude A → A/√2). Catches dead / clipped / half-volume output.
- **single-bin FFT / Goertzel** at the known test frequency: the peak must sit at
  the input frequency and dominate. Catches wrong rate, channel swap, garbage.
- **bit-exact checksum** for the pure-copy boundary test (no DSP in the path).
- **under-run / RT-AROS-call counters** from `ca_get_stats`: `underruns == 0` over
  the run, `rtAROSCalls == 0` always.

A known **sine** is the test signal throughout (RMS + FFT pin down amplitude *and*
frequency, so one PASS/FAIL covers right-samples / right-rate / right-channels /
no-dropouts).

**Markers** (one host binary per marker, `[A?]` PASS/FAIL block via
`harness/run-hosted.sh`, clean-exit on PASS):

- **[A1] host sine → WAV, asserted.** Pure host probe (no AROS): generate a 441 Hz
  sine at 44100 Hz, write a 16-bit stereo WAV via the Filesave header layout, read
  it back, assert header / sample count / RMS / 1-bin FFT peak at 441 Hz. Grounds
  the file path + assert harness, like H7's `pngprobe`. `[A1]`.
- **[A2] AROS mixer → WAV matches.** Drive a real AHI mixer pull (the `MixerFunc`
  Hook contract) and write the pulled int16 frames to a WAV. PASS = WAV RMS/FFT
  match the known input sound — proves the AHI side through the *real* Hook ABI.
  The CoreAudio driver minus CoreAudio. `[A2]`.
- **[A3] SPSC ring across the thread boundary, file-checked.** Wire the AROS slave
  (producer) to a real second host pthread (consumer) through the lock-free ring;
  the consumer writes what it pulls to a file instead of CoreAudio. PASS = consumer
  file == producer input **bit-for-bit** (checksum), zero ring corruption,
  `rtAROSCalls == 0` (guard counter), across thousands of frames under SIGALRM
  preemption. Proves the *boundary* — the real risk — with no audio hardware.
  Mirrors H11's "re-read independently to confirm bytes landed". `[A3]`.
- **[A4] live AUHAL output, verified offline.** Replace A3's file-consumer with the
  real AUHAL render callback (`ca_open`/`set_format`/`start`). Headless verification
  via `ca_render_to_wav` (offline AudioUnit / `ExtAudioFile`) → assert RMS/FFT as in
  A1. PASS = rendered file matches the sine; `underruns == 0`. Live speaker output
  is a human-facing nicety verified separately, not a loop dependency (same stance
  as H7's deferred on-screen window). This marker also measures ring depth vs the
  SIGALRM period (R-RING4). `[A4]`.
- **[A5] graft: real `CoreAudio` sub-driver in the AROS tree.** Build the actual
  `workbench/devs/AHI/Drivers/CoreAudio/` and drive it through `ahi.device` +
  `AHI_AllocAudio`/`AHI_PlayA`, with the WAV writer as a *second* output unit for
  the assertion. PASS = a program playing a known sound through `ahi.device` yields
  a WAV whose RMS/FFT match. Full thesis end-to-end. Rides the crosstools graft, not
  a session-sized spike. `[A5]`.

## Build / integration

- Shim `libcoreaudio_shim.dylib` links `AudioToolbox, AudioUnit, CoreAudio,
  CoreFoundation` (and `ExtAudioFile` lives in AudioToolbox); built with host clang
  `-arch arm64`, codesigned (ad-hoc fine for spikes — confirm vs. the existing
  `harness/run.sh` signing path, **UNVERIFIED**), loaded via `hostlib.resource`.
- Spikes compile to Mach-O via the existing `Makefile` pattern (`make hosted-audio`
  → builds `build/host-audio*` → `harness/run-hosted.sh '[A?] …'` searches stdout
  for the marker, returns the uniform `result=(PASS|FAIL)` block) `[OURS]`. Add
  `[A1]–[A4]` to the `harness/test-hosted.sh` regression set.
- The C ABI header is shared, hand-written, independent work. The shim must not
  link or include AROS headers; the AROS side must not include CoreAudio headers.

## Open questions / UNVERIFIED

- Exact ring depth vs the SIGALRM tick period — default ≈ 2048 frames (~46 ms),
  measured in [A4]; under-run count is the judge (R-RING4).
- Whether AUHAL accepts int16 input directly on Apple Silicon (would skip the
  float32 convert) — default to converting ourselves.
- Whether a `volatile` low-water flag is worth it vs accepting tick-granularity
  producer latency (R-RING3) — safe default is tick-granularity.
- The free `AHIDB_AudioID` for the `CoreAudio.s` stub — pick one, confirm vs. the
  in-tree drivers at graft.
- Codesign / entitlements for a `dlopen`'d CoreAudio dylib in the hosted process —
  confirm vs. `harness/run.sh`.
- Whether to store float32 or int16 in the ring (convert in slave vs in shim) —
  default convert-in-slave so the RT callback only `memcpy`s.
- AudioQueue fallback (own a fixed buffer pool, recycle-callback sets a `volatile`
  free flag the slave polls) if the SPSC ring proves too subtle to verify first —
  coarser granularity, same no-cross-thread-AROS-call discipline.

## Provenance summary

`[PUB]` Apple `AURenderCallback` real-time-thread contract (no blocking / no alloc
/ no locks / no system calls; `OSStatus`/`AudioBufferList`/`inNumberFrames`),
`kAudioUnitProperty_SetRenderCallback`, AUHAL (`kAudioUnitSubType_HALOutput`),
`AudioStreamBasicDescription` / float32 PCM, `ExtAudioFile`; C11 atomics + SPSC
ring-buffer theory (release/acquire, single-word indices); POSIX `pthread_sigmask`;
the RIFF/WAVE container. ·
`[AROS]` `Include/SFD/ahi_sub_lib.sfd` (LVO bias/args),
`Include/C/libraries/ahi_sub.h` (`AHISF_*`, `AHIAudioCtrlDrv`),
`Include/C/devices/ahi.h` (`AHIST_*`, `AHIDB_*`), `Device/audioctrl.c` (Hook
install :601, BuffType :553, BuffSamples :77, BuffSize :87), `Device/mixer.c`
(`MixerFunc` :82), `compiler/include/utility/hooks.h` (Hook ABI :82),
`Drivers/Alsa/` (`alsa-main.c` AllocAudio/Start/Stop/GetAttr, `alsa-playslave.c`
pull :132 / `SmallDelay` :56, `alsa-bridge/alsa.c` signal-mask guard :42 /
`alsa_hostlib.c` :85), `Drivers/OSS/OSS-playslave.c:169` (int16 narrow),
`Drivers/Filesave/` (`filesave-playslave.c` pull :309 / write :334 / header patch
:521, `FileFormats.h:154` WAV layout), `Drivers/Device/device-playslave.c:31` (slave
entry), `workbench/devs/audio/` (audio.device is an AHI client),
`arch/all-unix/bootstrap/hostlib.h` + `arch/all-hosted/hostlib/` (`HostLib_Open/
GetPointer/Lock`). ·
`[OURS]` H3 (`hosted/abishim.S`, Apple variadic ABI), H4/H6 (single-thread
scheduler, `Forbid`-is-compiler-barrier), H7 (`hosted/display.c`, render-to-file
unattended-verify stance + `pngprobe`), H9/H10 (`hosted/signal.c`, `hosted/msgport.c`,
Wait/Signal + `volatile` re-read), H11 (`hosted/device.c`, IORequest→host I/O→reply),
`harness/run-hosted.sh` marker harness. ·
`[DERIVED]` independently-derived points flagged for extra verification:
(a) ~40 ms host-buffer target as the ring-sizing magnitude
[R-RING4], and (b) that a callback/event-driven host API is the right RT-thread
hand-off shape — both restated above from Apple's render-thread contract `[PUB]` +
SPSC theory `[PUB]` + the H4/H6 scheduler model `[OURS]` + the Alsa-bridge mask
precedent `[AROS]`. No third-party code, identifiers, or call sequence used.
