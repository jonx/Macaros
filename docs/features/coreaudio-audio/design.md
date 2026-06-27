# CoreAudio audio — host-backed sound for AROS (audio.device / AHI)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-24

## What & why

Give the hosted AROS real sound on the Mac. AROS already has a complete
retargetable audio stack (AHI) with a software mixer; what's missing is the last
hop — an output driver that takes the mixed PCM and hands it to macOS CoreAudio
(an `AudioUnit` render callback, or `AudioQueue`). We write that one driver.

Why it's the right shape for Phase 2: it's the audio instance of the standing
thesis — *"macOS owns the drivers; AROS reaches them via standard exec I/O."* The
Mac owns CoreAudio; AROS reaches it through the AHI sub-driver LVO interface
(`AHIsub_Start`/`AHIsub_Stop` + the mixer Hook). It re-uses the host-call boundary
already de-risked by H3 (the Apple-variadic ABI shim, `hosted/abishim.S`) and the
`hostlib.resource` symbol-resolution mechanism, and the slave-process + signal
handshake mechanism already proven by H9/H10 (`hosted/signal.c`, `hosted/msgport.c`).

The one genuinely *new* design point this feature forces — and the reason it earns
a doc — is the **RT-thread boundary**: CoreAudio is push/callback-driven (it spins
up its own real-time host thread and *calls us* when it wants samples), which
collides with AROS's single-underlying-thread, signal-driven scheduler model (H4/H6).
Every other hosted feature so far has been *pull* (AROS asks the host to do work and
blocks for a reply). Audio inverts that. The bulk of the Design section is about
crossing that boundary safely.

## Does it already exist?

No CoreAudio/AudioToolbox backend exists, in this repo or upstream. Evidence:

- This repo: the hosted spikes are `hosted/*.c` (H1–H12: foundation, ABI shim,
  scheduler, memory, display, library, signal, msgport, device, execboot). None
  touch audio. `grep -rniE 'coreaudio|audiotoolbox|audiounit|audioqueue|AHIsub|ahi'`
  over `/Users/user/Source/aros-aarch64` returns nothing.
- Upstream: `grep -rIn -i 'coreaudio|audiotoolbox|audiounit|audioqueue|auhal'` over
  `/Users/user/Source/aros-upstream` returns **only** unrelated USB-audio-class hits
  — `struct NepAudioUnit` in `rom/usb/classes/audio/usbaudio.h` (a USB descriptor
  type, nothing to do with Apple's AudioUnit). So AROS has never had a macOS audio
  backend.
- Upstream **does** ship every piece we mirror or reuse:
  - the AHI device + software mixer (`workbench/devs/AHI/Device/`);
  - the AHI sub-driver interface (`workbench/devs/AHI/Include/{C,SFD}/...`);
  - ~24 existing sub-drivers under `workbench/devs/AHI/Drivers/` including two that
    prove the *hosted* pattern (`Alsa/`, `OSS/` — they reach a host audio library),
    one that proves the *render-to-file* pattern we need for unattended verification
    (`Filesave/`), and a no-op reference (`Void/`).

So the work is: write **one new AHI sub-driver** (`CoreAudio`) plus its
host-call bridge. The AHI device, mixer, and the classic `audio.device` shim are
reused unchanged.

**External prior art (web-grounded, *not* in the AROS tree).** The web confirms the
gap and sharpens the placement; it surfaced no usable CoreAudio code, but three facts
matter:

- **No CoreAudio/macOS AHI driver exists anywhere — confirmed against upstream HEAD.**
  The current `aros-development-team/AROS` `workbench/devs/AHI/Drivers/` tree
  (github.com/aros-development-team/AROS) holds exactly:
  Alsa, Aura, CMI8738, EMU10kx, Envy24, Envy24HT, Filesave, HDAudio, OSS, Paula,
  PulseAudio, RPiHDMI, RPiPWM, SB128, SoundBlasterAWE, Toccata, VIA-AC97, Void,
  WASAPI, Wavetools, ac97, amiga-m68k. **No `CoreAudio`, no `Darwin`, no `SDL`.** This
  upgrades the gap claim from "absent in our clone" to "absent at upstream HEAD."
- **CoreAudio is the missing macOS member of an existing host-backed family.** Of that
  list the *host-OS-backed* (not hardware) drivers are **Alsa, OSS, PulseAudio**
  (Unix-hosted) and **WASAPI** (the Windows-hosted backend). So AROS already has a
  Linux backend *and* a Windows backend that do exactly what we propose — forward AHI's
  mixed PCM to the host's audio API. CoreAudio is simply the macOS sibling none of the
  hosted ports ever needed (the historical AROS-darwin ports — i386/x86_64/PPC, all
  listed "Working" — require X11 and ship **no audio at all**:
  aros.sourceforge.io/introduction/ports.html). The catch: the in-tree `WASAPI`
  driver is the closest *structural* analogue (callback/event-driven host API, like
  CoreAudio's RT callback — unlike Alsa's blocking-write model); but it is Win32
  code, no help for the Apple-Silicon ABI/signal-mask plumbing.
- **"SDL-audio AHI driver" is a category error — the dependency runs the other way.** On
  Amiga-family systems SDL is a *client of* AHI: SDL2's audio backend opens `ahi.device`
  to get sound out (os4depot.net SDL2; the AmigaPorts SDL2 tree). There is no AHI
  sub-driver written in SDL, and writing one would be inverted. An "SDL alternative" to
  this feature therefore means a different *host bridge* — let **SDL2's own CoreAudio
  backend** do the macOS hand-off instead of our hand-written AUHAL bridge — not a
  portable AHI driver. The catch: it trades one well-understood framework dependency
  (CoreAudio direct) for a heavier one (all of SDL2) to save the ~one callback we'd
  write anyway, and still leaves the RT-thread-vs-scheduler boundary (the real risk)
  exactly where it is. Noted as a fallback, not the plan.

## Background: AROS audio contracts (grounded)

AROS has **two** audio layers. We must hook the right one.

### Layer 1 — classic `audio.device` (Paula, 4 channels) — NOT the hook point

The generic, portable `audio.device` lives at `workbench/devs/audio/`. Its public
API is in `compiler/include/devices/audio.h`: `struct IOAudio` (`ioa_Data`,
`ioa_Length`, `ioa_Period`, `ioa_Volume`, `ioa_Cycles`) + `CMD_WRITE` and
`ADCMD_ALLOCATE`/`FREE`/`SETPREC`/`FINISH`/`PERVOL`/`LOCK`/`WAITCYCLE`.

Crucially this generic device **produces no sound itself** — it is a Paula-API
emulation shim *on top of AHI*. It opens `ahi.device` and submits `AHIRequest`s:
`OpenDevice("ahi.device", ...)` (`workbench/devs/audio/audio_commands.c:810`),
`io_Command = CMD_WRITE` / `ahir_Type = AHIST_M8S` (`:991`); its unit struct is
built around `struct AHIRequest eu_ahireq` (`audio_intern.h:146`). Its
`mmakefile.src` excludes m68k ("do not override ROM Paula audio.device").

The *only* real hardware backend for `audio.device` is m68k-Amiga, poking Paula
registers at `0xdff000` (`arch/m68k-amiga/devs/audio/audio_hardware.c`:
`custom->aud[ch].ac_ptr/ac_len/ac_per/ac_vol`, `custom->dmacon`) — no abstraction
seam, not portable. Likewise the AHI "Paula" sub-driver
(`workbench/devs/AHI/Drivers/Paula/`) is pure m68k PhxAss asm (`smakefile` says
`MACHINE 68020`).

**Verdict: do not touch `audio.device`.** On a non-Amiga target it is already an AHI
client; if AHI has working output, classic Paula programs get sound *for free*
through this shim.

### Layer 2 — AHI (`ahi.device` + sub-drivers) — THE hook point

AHI is the retargetable layer. It has a software mixer and a clean low-level driver
interface; vendors plug in a "sub-driver" per output target. This is exactly where
ALSA/OSS/PulseAudio/Filesave plug in, and exactly where CoreAudio belongs.

**The sub-driver LVO interface** is defined in
`workbench/devs/AHI/Include/SFD/ahi_sub_lib.sfd` (and the C decls in
`workbench/devs/AHI/Include/C/libraries/ahi_sub.h`). The functions a sub-driver
implements (bias 30, register args in `()`):

```
AHIsub_AllocAudio(tagList, AudioCtrl)            (a1,a2)  -> ULONG flags
AHIsub_FreeAudio(AudioCtrl)                       (a2)
AHIsub_Disable(AudioCtrl) / AHIsub_Enable(AudioCtrl)  (a2)   -> Forbid()/Permit()
AHIsub_Start(Flags, AudioCtrl)                    (d0,a2)  -> spawn playback slave
AHIsub_Update(Flags, AudioCtrl)                   (d0,a2)
AHIsub_Stop(Flags, AudioCtrl)                     (d0,a2)  -> tear slave down
AHIsub_SetVol/SetFreq/SetSound/SetEffect/...      (HW-accel; optional)
AHIsub_GetAttr(Attribute, Argument, DefValue, tagList, AudioCtrl) (d0,d1,d2,a1,a2)
AHIsub_HardwareControl(Attribute, Argument, AudioCtrl)  (d0,d1,a2)
```

A software-mixing sub-driver (which is what we are — CoreAudio gives us no DSP) only
needs a handful: `AllocAudio`, `FreeAudio`, `Disable`, `Enable`, `Start`, `Update`,
`Stop`, `GetAttr`, `HardwareControl`. Filesave, Alsa, and OSS all implement exactly
this subset; the `SetVol`/`SetFreq`/`SetSound` family is for hardware mixers and we
return defaults.

**The capability handshake.** `AHIsub_AllocAudio` returns a flags word
(`ahi_sub.h:81`): we return `AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO`
(Alsa returns exactly this — `Drivers/Alsa/alsa-main.c:143`; OSS adds
`AHISF_KNOWHIFI`). `AHISF_MIXING` tells AHI *"use your software mixer; populate the
mixer/player Hooks for me"*. `AHISF_TIMING` says *"I pace playback myself"* (we do —
the slave loop / RT callback is the clock). `AHISF_ERROR` is the failure return.

**The mixer-callback contract** — the heart of it. AHI installs a `struct Hook
*ahiac_MixerFunc` in the `AHIAudioCtrlDrv` (set at
`workbench/devs/AHI/Device/audioctrl.c:596`, body `MixerFunc` at
`workbench/devs/AHI/Device/mixer.c:81`). The sub-driver's playback slave *pulls* one
buffer of mixed PCM per pass by calling it through the standard Amiga Hook ABI:

```c
/* per pass, in the slave loop — Drivers/Device/device-playslave.c:142, Alsa:132 */
CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);          /* advance song/timing */
CallHookPkt(AudioCtrl->ahiac_MixerFunc,  AudioCtrl, mixbuffer);     /* fill mixbuffer */
samplesready = AudioCtrl->ahiac_BuffSamples;                        /* frames produced this pass */
```

Hook ABI: `CallHookPkt(hook, object, message)` → `object`→A2 (the `AudioCtrl`),
`message`→A1 (the destination buffer). `MixerFunc` returns `void` and fills the
caller's buffer. On AArch64 our `CallHookPkt`/`HookEntry` marshalling must route
`object→arg2`, `message→arg1`; and note the mixer's `h_Entry` is
`HookEntryPreserveAllRegs` (it can run from interrupt context), so the thunk must
save all regs.

**The buffer format** (chosen by AHI from the flags we returned —
`Device/audioctrl.c:548`), values from `Include/C/devices/ahi.h:319`:

| BuffType   | value | layout                          | frame bytes |
|------------|-------|---------------------------------|-------------|
| AHIST_M16S | 1     | mono, 16-bit signed             | 2           |
| AHIST_S16S | 3     | stereo, 16-bit signed (L,R int) | 4           |
| AHIST_M32S | 8     | mono, 32-bit signed (HIFI)      | 4           |
| AHIST_S32S | 10    | stereo, 32-bit signed (HIFI)    | 8           |

Samples are **signed native-endian integers, interleaved L,R**. On AArch64 that's
little-endian. With `AHISF_KNOWSTEREO` (no HIFI) we get **`AHIST_S16S`** — 16-bit
stereo interleaved, the easy case. Sizes:
`ahiac_BuffSamples` = frames per pass = `MixFreq / PlayerFreq` (clamped to a WORD;
`audioctrl.c:77`); `ahiac_BuffSize` = the byte size to allocate for the mix buffer
(`ahi_sub.h:62`, worst-case `MaxBuffSamples * frameSize`, 8-byte-aligned + 80 pad,
`audioctrl.c:87`). `ahiac_MixFreq` is the actual output rate in Hz (e.g. 44100);
`AllocAudio` reads the request in this field and writes back what the host negotiated
(`alsa-main.c:105,139`).

**The slave-process pattern.** `AHIsub_Start` allocates the mix buffer
(`AllocVec(ahiac_BuffSize)`) and spawns a slave Process at high priority via
`CreateNewProc` (`Drivers/Alsa/alsa-main.c:205`, pri 127; Filesave uses pri -1),
passing the `AudioCtrl` in `tc_UserData`. Slave entry is the `AROS_UFH3` wrapper
`SlaveEntry`→`Slave(SysBase)` (`Drivers/Device/device-playslave.c:31`). A two-way
signal handshake (allocated in `AllocAudio`: `mastersignal` owned by the master,
`slavesignal` owned by the slave) brings it up ("slave alive" → master returns) and
tears it down (`AHIsub_Stop` `Signal`s the slave to die, then `Wait`s for the
"dying" ack — `alsa-main.c:268`). The slave checks `SIGBREAKF_CTRL_C | slavesignal`
each pass.

**Why AHI, restated.** Both hosted precedents (Alsa, OSS) and the file precedent
(Filesave) are AHI sub-drivers using exactly this contract. The classic `audio.device`
is already an AHI client. So a CoreAudio output driver belongs as an AHI sub-driver —
and classic Paula programs reach it transparently through the eaudio shim.

### Reference points already de-risked in this repo

- **Host-call boundary**: H3 proved Apple's variadic-args-on-stack ABI and built the
  marshaller (`hosted/abishim.S`). Any varargs host call goes through it.
- **Host symbol resolution**: AROS's hosted ports reach host code via
  `hostlib.resource` — `Host_HostLib_Open`/`GetPointer`
  (`arch/all-unix/bootstrap/hostlib.h`, impl `arch/all-hosted/hostlib/`). The Alsa
  bridge uses exactly this to `dlopen` `libasound.so.2` and fill a function-pointer
  table (`Drivers/Alsa/alsa-bridge/alsa_hostlib.c:61`); the CoreAudio bridge does the
  same against the AudioToolbox/CoreAudio frameworks (or a small native `.dylib` shim).
- **I/O path shape**: H11 (`hosted/device.c`) ran a real exec IORequest → device task
  → real macOS syscall → reply on a switched task stack under preemption. Audio reuses
  the slave-task + signal machinery (H9/H10) but inverts the direction (host calls us);
  see The bridge.

## Design

### Host side (CoreAudio render callback)

Use **AUHAL** (the `kAudioUnitSubType_HALOutput` AudioUnit) for live output — it's the
standard low-latency output path and exposes a render callback. The bridge (native
arm64 C, reached via `hostlib.resource` like `alsa-bridge/`) exposes opaque verbs
mirroring `alsa-bridge/alsa.h` (`ALSA_Open/SetHWParams/Write/Avail/Prepare/Close`):

```
CA_Open()                      -> APTR handle   (AudioComponent + AudioUnit, not started)
CA_SetFormat(handle, &freq)    -> BOOL          (set AudioStreamBasicDescription; write back rate)
CA_Start(handle)               -> BOOL          (AudioOutputUnitStart — RT thread begins)
CA_Stop(handle)                                 (AudioOutputUnitStop)
CA_Close(handle)
```

Two output flavours, chosen at build/run time (see The bridge for why this matters):

1. **AUHAL push-via-callback** — CoreAudio runs an `AURenderCallback` on its own
   real-time thread asking for N frames. We service it from a lock-free ring buffer
   the AROS slave fills. Lowest latency; the hard mode for the scheduler boundary.
2. **AudioQueue** — `AudioQueueNewOutput` + a pool of `AudioQueueBuffer`s; we enqueue
   filled buffers and CoreAudio drains them, calling our buffer-recycle callback when
   one is free. Slightly higher latency, simpler back-pressure (a finite buffer pool
   we own), and the recycle callback is the natural place to signal AROS.

**Format.** AHI gives us `AHIST_S16S` = int16 stereo interleaved LE. CoreAudio's
canonical format is float32 (interleaved or non-interleaved). So unlike ALSA (which
takes `SND_PCM_FORMAT_S16_LE` directly) we **must convert int16→float32** (`x /
32768.0f`), and possibly interleaved→planar. The conversion goes right after the
`MixerFunc` pull, the slot where OSS does its narrowing copy
(`Drivers/OSS/OSS-playslave.c:169`). Or, set the AUHAL input ASBD to `kAudioFormat
LinearPCM` with `kAudioFormatFlagIsSignedInteger`, 16-bit, and let CoreAudio convert
— simplest, verify empirically that AUHAL accepts int16 input on Apple Silicon
(**UNVERIFIED**; the safe default is to convert to float32 ourselves).

**Offline / file output for verification** — the most important host path for this
project. Two options, both headless and TCC-free:
- a **Filesave-style WAV writer** entirely in AROS (no CoreAudio at all): the slave
  pulls `MixerFunc` and `Write()`s a WAV, exactly as
  `Drivers/Filesave/filesave-playslave.c:334` does. This is the [A1]/[A2] ground truth.
- an **`ExtAudioFile` / offline AudioUnit** host path that renders to a `.caf`/`.wav`
  on disk and never opens a hardware device. Useful to test the *bridge* without audio
  hardware, but the AROS-only WAV writer is simpler and has no host dependency, so it's
  the primary verification path.

### AROS side (the AHI sub-driver / audio backend)

A new sub-driver directory `workbench/devs/AHI/Drivers/CoreAudio/` modelled on
`Drivers/Alsa/`, which is almost entirely host-agnostic AROS code:

- `coreaudio-init.c` — `DriverInit`/`DriverCleanup`: open dos/utility/stdc; init the
  bridge (`CA_HostLib_Init` ≈ `ALSA_HostLib_Init`, `alsa_hostlib.c:84`).
- `coreaudio-main.c` — the AHIsub LVOs:
  - `AHIsub_AllocAudio`: alloc `DriverData` into `ahiac_DriverData`, alloc the master
    signal, record `FindTask(NULL)`, open the bridge handle (`CA_Open`), negotiate
    rate (`CA_SetFormat(&freq)`, write `ahiac_MixFreq = freq`), return
    `AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO`.
  - `AHIsub_Start(AHISF_PLAY)`: `AllocVec(ahiac_BuffSize)` for the mix buffer, spawn
    the slave via `CreateNewProc` (pri high), hand it `AudioCtrl` in `tc_UserData`,
    `Wait` for "slave alive".
  - `AHIsub_Stop`: signal slave to die, `Wait` for the ack, `CA_Stop`, free buffers.
  - `AHIsub_Disable`/`Enable` → `Forbid()`/`Permit()`.
  - `AHIsub_GetAttr`: advertise `AHIDB_Bits=16`, a frequencies table (8000/11025/
    22050/44100/48000), names, `AHIDB_Realtime=TRUE` — copy `alsa-main.c:308`.
  - `AHIsub_Update`/`HardwareControl`: no-ops (return 0).
- `coreaudio-playslave.c` — the `Slave(SysBase)` loop (`AROS_UFH3 SlaveEntry`). Per
  pass: `CallHookPkt(ahiac_PlayerFunc,…,NULL)` → `CallHookPkt(ahiac_MixerFunc,…,
  mixbuffer)` → convert int16→float32 → hand to host (ring-buffer write or
  AudioQueue enqueue) → flow-control wait. Exit on `SIGBREAKF_CTRL_C | slavesignal`.
- `coreaudio-bridge/` — native arm64 C, the *only* file that names CoreAudio symbols;
  exposes the opaque `CA_*` verbs; resolved through `hostlib.resource`.
- `DriverData.h`, `Makefile.in`, `CoreAudio.s` (the driver stub/header) — copy from
  `Drivers/Alsa/`.

### The bridge (buffer hand-off across the RT-thread / AROS-scheduler boundary)

This is the new, load-bearing piece. AROS's hosted model (H4/H6) is **one underlying
OS thread** running AROS tasks cooperatively+preemptively, with a SIGALRM tick; the
H6 lesson was that `Forbid`/`Permit` need only a *compiler* barrier because there is
no second core/thread racing AROS state. **CoreAudio breaks that assumption**: its
render callback runs on a *separate, real* host RT thread that AROS's scheduler does
not know about and must never block.

Hard rules for that thread, grounded in how the Alsa bridge already guards host
threads:
- The RT callback **must not call any AROS LVO** (no `AllocMem`, no `Signal` via the
  AROS path, no `Forbid`). AROS state is not thread-safe against a second real thread.
- The RT callback **must not block** (no mutex held by AROS, no `Wait`). A blocked
  render callback = audible dropout / glitch.
- The RT host thread must not inherit AROS's signal mask, or it will field AROS's
  SIGALRM/SIGSEGV and corrupt the scheduler/trap path. The Alsa bridge already does
  exactly this guard for PulseAudio's spawned threads —
  `_prepare_kernel_for_new_host_pthread` masks all signals around host-thread creation
  (`Drivers/Alsa/alsa-bridge/alsa.c:42`, applied around `snd_pcm_open` at `:129`). The
  CoreAudio bridge must `pthread_sigmask(SIG_BLOCK, all)` around `AudioOutputUnitStart`
  so the RT thread is born with AROS signals blocked.

The hand-off is therefore a **single-producer / single-consumer lock-free ring
buffer** living in the bridge (host memory), touched by:
- **producer** = the AROS slave task (one underlying thread), writing int16/float32
  frames after each `MixerFunc` pull;
- **consumer** = the CoreAudio RT thread, reading frames in the render callback.

SPSC with atomic read/write indices needs no lock, so neither side blocks the other,
and it sidesteps the AROS-not-thread-safe problem because the ring is plain host
memory, not AROS state. Back-pressure: when the ring is full the AROS slave parks
itself (a short `Wait`, like the Alsa slave's `SmallDelay()` VBlank wait —
`alsa-playslave.c:45`) and resumes next tick; on under-run (ring empty) the callback
emits silence and we count it. **The RT thread never calls `Signal()` (an AROS LVO)**
— the slave wakes on the scheduler tick and checks the ring's fill level, or polls a
`volatile` flag the bridge sets (same family as H9's `volatile` re-read of
`tc_State`). No AROS lock is ever held across the thread boundary.

Fallback if the SPSC ring proves too subtle to verify first: the **AudioQueue**
flavour — own a fixed pool of N buffers, fill from the slave, enqueue; the recycle
callback sets a `volatile` "buffer free" flag the slave polls. Same
no-cross-thread-AROS-call discipline, coarser granularity, easier to reason about.

## Plan — spikes in the loop

Each marker is a standalone host binary (one-binary-per-marker, like `hosted/*.c`)
with a single PASS/FAIL verdict the agent reads — no human listens.

- **[A1] host sine → WAV, asserted.** A pure host probe (no AROS): generate a known
  441 Hz sine at 44100 Hz, write it as a 16-bit stereo WAV via the same writer the
  Filesave path uses. The agent reads the WAV back and asserts: header well-formed,
  sample count correct, RMS within tolerance of the expected `1/√2 · amplitude`, and
  a 1-bin FFT peak at 441 Hz. Proves the file path + assertion harness before any AROS
  is involved. *(grounds the verification, like H7's `pngprobe`.)*
- **[A2] AROS mixer → WAV matches.** Stand up a miniature AHI mixer pull in a hosted
  binary (or the real AHI device once grafted): a sound is `AHI_LoadSound`'d, the
  slave loop calls `CallHookPkt(ahiac_MixerFunc, AudioCtrl, buf)` exactly as
  `Drivers/Filesave/filesave-playslave.c:309`, and the pulled int16 frames are written
  to a WAV. PASS = the WAV's RMS/FFT match the known input sound (a sine sample), i.e.
  AROS's mixer produced the samples we expect through the *real* Hook contract. This is
  the CoreAudio driver minus CoreAudio — the AHI-side correctness, fully unattended.
- **[A3] SPSC ring across the thread boundary, file-checked.** Wire the AROS slave
  (producer) to a real second host pthread (consumer) through the lock-free ring, but
  have the consumer write what it pulls to a file instead of CoreAudio. PASS = the
  consumer's file equals the producer's input bit-for-bit (checksum), zero ring
  corruption, zero AROS LVO called from the consumer thread (asserted by a guard
  counter), across thousands of frames under the SIGALRM preemption. This proves the
  *boundary* — the genuinely new risk — without audio hardware. *(mirrors H11's
  "re-read independently to confirm bytes landed".)*
- **[A4] live AUHAL output.** Replace A3's file-consumer with the real AUHAL render
  callback (`CA_Open/SetFormat/Start`). Verification stays headless via a **loopback**:
  request CoreAudio's *default output* be an aggregate/loopback or, simpler, render
  through an **offline AudioUnit / `ExtAudioFile`** to a `.wav` and assert RMS/FFT as in
  A1. PASS = the rendered file matches the sine. Live speaker output is a human-facing
  nicety verified separately, not a loop dependency (same stance as H7's deferred
  on-screen window).
- **[A5] graft: real `CoreAudio` sub-driver in the AROS tree.** Build the actual
  `workbench/devs/AHI/Drivers/CoreAudio/` and drive it through `ahi.device` +
  `AHI_AllocAudio`/`AHI_PlayA`, with the Filesave-style WAV writer as a *second* output
  unit for the assertion. PASS = a program playing a known sound through `ahi.device`
  yields a WAV whose RMS/FFT match. This is the full thesis end-to-end.

Build/run them in the existing harness style (`make hosted-audio` → `[A?]` markers),
clean-exit on PASS.

## How we verify it unattended

No human listens; no TCC/Screen-Recording/Microphone prompt is ever hit (output-only
playback to a file needs no entitlement). Verification = **render-to-WAV +
numeric assertions on the samples**:

1. **Primary path: AROS-only WAV.** The AHI Filesave technique already exists
   (`Drivers/Filesave/filesave-playslave.c`): the slave pulls `MixerFunc` and `Write()`s
   a WAV with a patched header. We reuse it as a parallel output unit so every spike
   produces a file the agent reads. No CoreAudio dependency in the assertion path.
2. **Assertions** (no listening):
   - WAV header well-formed, sample-rate == `ahiac_MixFreq`, frame count == expected.
   - **RMS** of the buffer within tolerance of the known input's RMS (silence → ~0;
     a sine of amplitude A → A/√2). Catches dead/clipped/half-volume output.
   - **single-bin FFT / Goertzel** at the known test frequency: the peak must be at the
     input frequency and dominate. Catches wrong sample-rate, channel swap, garbage.
   - **bit-exact checksum** for the pure-copy boundary test [A3] (no DSP in the path).
   - **under-run / glitch counter** from the bridge must be 0 over the run.
3. **Live output [A4]** is verified by **loopback/offline render to a file**, never by a
   microphone or a human ear — render through an offline AudioUnit/`ExtAudioFile` to
   disk and run the same RMS/FFT assertions. This keeps the live path inside the
   unattended loop and TCC-free.

A known **sine** is the test signal throughout: trivial to generate, and RMS+FFT pin
down amplitude *and* frequency, so a single PASS/FAIL covers "right samples, right
rate, right channels, no dropouts".

## Risks & open questions

- **RT thread vs single-thread scheduler (the headline risk).** AROS's hosted model
  assumes one underlying thread (H6: `Forbid` is only a compiler barrier). CoreAudio's
  render callback is a *real* second RT thread. Mitigation = SPSC lock-free ring in
  host memory + the iron rule *the RT thread calls no AROS LVO and never blocks*, and
  is born with AROS signals masked (the Alsa-bridge precedent, `alsa.c:42`). [A3]
  exists specifically to prove this before audio hardware is in the loop. **Open:** is
  a `volatile` flag enough to wake the slave promptly, or do we accept tick-granularity
  latency (≤ one SIGALRM period)? Tick-granularity is the safe default.
- **Latency / under-runs.** Pull granularity is `ahiac_BuffSamples` =
  `MixFreq/PlayerFreq` frames; if the slave can't refill before the RT callback drains
  the ring, we glitch. The AudioQueue fallback trades latency for a buffer pool we own.
  Under-run count is an explicit assertion. **Open:** ring depth vs SIGALRM period —
  needs measurement in [A4]. *Design data point (independently derived):* a producer
  need not be paced to a hard sample clock — a PID controller that tweaks
  playback frequency to hold a target buffer fill (~40 ms default) can absorb
  producer/consumer drift via dynamic resampling rather than tight scheduling. For
  us this argues: size the ring for a
  tens-of-ms cushion (not single-pass), accept tick-granularity latency, and let the
  ring's fill level — not a precise clock — be the back-pressure signal; a PID/resampler
  is overkill at this stage but is the known escape hatch if drift bites.
- **Sample-rate / format conversion.** AHI gives int16 LE interleaved
  (`AHIST_S16S`); CoreAudio is float32. Conversion is one multiply per sample after the
  pull (OSS-narrowing slot, `OSS-playslave.c:169`). **Open/UNVERIFIED:** whether AUHAL
  accepts int16 input directly on Apple Silicon (would skip the conversion); default to
  converting ourselves.
- **`audio.device` vs AHI — decided.** Hook **AHI** (a `CoreAudio` sub-driver). The
  classic `audio.device` is, on non-Amiga, an AHI client (`workbench/devs/audio/` →
  `ahi.device`); its only hardware backend is m68k Paula. So a CoreAudio sub-driver
  gives both AHI apps *and* classic Paula apps sound, with zero work on `audio.device`.
- **Apple variadic ABI on host calls.** CoreAudio's APIs are mostly fixed-arg, but any
  varargs path (logging) must go through the H3 shim (`hosted/abishim.S`). Low risk,
  already de-risked.
- **`hostlib.resource` vs framework symbols.** Alsa `dlopen`s one `.so`; CoreAudio is
  a framework bundle. **Open:** resolve `AudioComponent*`/`AudioOutputUnit*` directly
  via `Host_HostLib_GetPointer` against the framework binary, or build a tiny native
  `coreaudio_shim.dylib` exporting the `CA_*` verbs and `dlopen` *that* (cleaner — the
  Alsa bridge essentially is such a shim already). Lean toward the shim.
- **The graft, not a spike.** [A5] (the real driver in the AROS tree) depends on the
  AROS `aarch64-darwin` crosstools + `mmake` producing modules — the same large-scale
  integration the boot milestone began. [A1]–[A4] are session-sized spikes that stand
  alone; [A5] rides the graft.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- AHI sub-driver interface: `workbench/devs/AHI/Include/SFD/ahi_sub_lib.sfd`,
  `workbench/devs/AHI/Include/C/libraries/ahi_sub.h` (`struct AHIAudioCtrlDrv`,
  `AHISF_*`, `ahiac_MixerFunc`/`PlayerFunc`/`BuffSamples`/`BuffSize`/`MixFreq`).
- AHI device + mixer: `workbench/devs/AHI/Device/audioctrl.c` (Hook install :596,
  BuffType select :548, BuffSize calc :77/:87, Start trigger :1008),
  `workbench/devs/AHI/Device/mixer.c` (`MixerFunc` :81),
  `workbench/devs/AHI/Include/C/devices/ahi.h` (`AHIST_*` :319, `AHIA_*` tags).
- Hosted sub-driver precedents: `workbench/devs/AHI/Drivers/Alsa/`
  (`alsa-main.c` Start :205 / Stop :268 / AllocAudio :99, `alsa-playslave.c` mixer
  pull :132, `alsa-bridge/alsa_hostlib.c` HostLib :61, `alsa-bridge/alsa.c`
  signal-mask guard :42), `workbench/devs/AHI/Drivers/OSS/` (format narrowing
  `OSS-playslave.c:169`).
- Render-to-file precedent: `workbench/devs/AHI/Drivers/Filesave/`
  (`filesave-playslave.c` mixer pull :309, `Write()` :334, WAV header `FileFormats.h`),
  `workbench/devs/AHI/Drivers/Device/device-playslave.c` (slave-entry `AROS_UFH3` :31,
  pull loop :142).
- Classic `audio.device`: `workbench/devs/audio/` (`audio_commands.c` AHI client :810,
  `audio_intern.h` `eu_ahireq` :146), `compiler/include/devices/audio.h` (`IOAudio`,
  `ADCMD_*`), `arch/m68k-amiga/devs/audio/audio_hardware.c` (Paula HW, m68k-only).
- Host-symbol mechanism: `arch/all-unix/bootstrap/hostlib.h`
  (`Host_HostLib_Open`/`GetPointer`), `arch/all-hosted/hostlib/`.

This repo (`/Users/user/Source/aros-aarch64`):
- `NOTES.md` — H3 (Apple variadic ABI / `hosted/abishim.S`), H4/H6 (scheduler,
  single-thread `Forbid` barrier), H7 (render-to-PNG unattended-verify stance),
  H9/H10 (Wait/Signal, message ports), H11 (`hosted/device.c` — IORequest → device
  task → real macOS syscall → reply).
- `hosted/host.c` (host-process foundation), `hosted/abishim.S` (variadic shim),
  `hosted/signal.c`, `hosted/msgport.c`, `hosted/device.c`.

External prior art (web, not in the AROS tree):
- `github.com/aros-development-team/AROS` → `workbench/devs/AHI/Drivers/` — authoritative
  upstream-HEAD driver list; confirms no `CoreAudio`/`Darwin`/`SDL` driver and that
  `WASAPI` (Windows-hosted) + `PulseAudio`/`Alsa`/`OSS` (Unix-hosted) are the existing
  host-backed family CoreAudio would join. The in-tree `WASAPI` driver is the closest
  structural analogue (callback-driven host API), though it is Win32.
- `aros.sourceforge.io/introduction/ports.html` — the historical AROS-darwin hosted
  ports (i386/x86_64/PPC, "Working"); require X11, ship no audio — confirms macOS AROS
  never had sound.
- `arp2.sourceforge.net/ahi/` — Martin Blom's AHI home page: "AROS (ALSA/OSS)" is the
  only hosted backend it lists for AROS; no CoreAudio/SDL/WASAPI mention.
- SDL-on-Amiga (for contrast — SDL is a *client* of AHI, not an AHI driver):
  `os4depot.net` SDL2, `github.com/amigaports` — SDL2's Amiga audio backend opens
  `ahi.device`. An "SDL alternative" means SDL2's own CoreAudio backend as the host
  bridge, not an AHI driver written in SDL.
- Latency/back-pressure design note (independently derived): a host-buffer fill
  target of ~40 ms held by a PID-controlled dynamic resampler is a known approach to
  absorbing producer/consumer drift without a hard sample clock.
