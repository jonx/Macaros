# Shared pattern — a foreign host thread waking an AROS task (`host-wake`)

> Drafted 2026-06-24 · Cross-cutting contract for the hosted drivers. Referenced by
> `bsdsocket-net`, `clipboard-bridge`, `coreaudio-audio` (and any future host driver
> with a background host thread). Process: [CLEANROOM.md](CLEANROOM.md).

## Why this exists

Three of the host drivers run a **background host (pthread) thread** that must notify an
AROS task which lives on AROS's **single underlying scheduler thread** (`[OURS]` H6).
Each spike grew its own version of "host thread → wake AROS"; this doc is the one
contract they all conform to, so the rules (atomics, ownership, lost-wakeup, signal
delivery) are stated once and the graft can share a single primitive.

The recurring shape:

| Driver | Host thread | What it watches | Wakes |
|--------|-------------|-----------------|-------|
| [bsdsocket-net](bsdsocket-net/spec.md) | a **kqueue** pump | socket fd readiness | the task in `WaitSelect`/blocked I/O |
| [clipboard-bridge](clipboard-bridge/spec.md) | an NSPasteboard **`changeCount` poller** | host clipboard changes | the clipboard-sync task |
| [coreaudio-audio](coreaudio-audio/spec.md) | the CoreAudio **RT render callback** | the PCM ring level | *(decoupled — see "The RT exception")* |

## The contract

**R-W1 — Atomics, never `volatile`.** Every byte of state shared between the host thread
and the AROS thread is C11 `_Atomic` (acquire/release) or mutex-guarded. `volatile` is
**not** a thread-safety primitive, and the H6 `Forbid`/compiler-barrier shortcut orders
nothing across a *real second OS thread* — it only works because the scheduler is
single-threaded. A genuine host pthread crossing needs a real fence (`_Atomic`) or a
real lock (`pthread_mutex`). `[PUB]` C11 memory model · `[OURS]` H6.

**R-W2 — One wake seam, mapped to `exec.Signal`.** The host thread's only contact with
AROS is a single wake primitive: at graft, `Signal(task, sigbit)`; in the spikes, a
stand-in (a self-pipe `write()`, or a callback the test installs). Nothing else about a
driver changes between spike and graft — only this seam is swapped. `[AROS]` `exec.Signal`.

**R-W3 — Check-before-park / re-probe (no lost wakeup).** The AROS side checks its
received-signal state *before* it `Wait()`s, so a wake that races ahead of the park is
still seen (`[AROS]` H9: `Wait` checks `tc_SigRecvd` before blocking). For **readiness**
pumps (sockets) the stronger rule applies: **the wake is a hint, the non-blocking
syscall is the truth** — on wake, re-issue the non-blocking op and believe its result,
never act on the wake alone. This also makes a *coalesced* wake (one wake standing for
several events) correct.

**R-W4 — Declared ownership.** Every shared resource has a stated single-owner or
single-pattern concurrency contract, enforced (assert/error), not assumed:
- sockets — **single-owner-fd**: a host fd is registered with the pump by exactly one
  waiter; double-register is rejected, unregister is owner-matched.
- clipboard — a **single self-write token** (the `changeCount` returned by the host's own
  `set_text`); the poller never raises a change for that value.
- audio — **SPSC**: exactly one producer (the AHI slave task) and one consumer (the RT
  callback) touch the ring.

**R-W5 — Bounded, idempotent lifecycle.** Start is idempotent/restartable; stop is a
no-hang join (atomic stop flag read in the thread loop, woken via the control seam, then
`pthread_join`); a watchdog bounds any test so a stuck thread can never wedge the
unattended loop.

## Reference primitive (the seam)

A minimal handle the host thread raises and the AROS task waits on. Spike stand-in and
graft mapping side by side:

```c
/* host_wake.h — the one seam every host driver shares. */
typedef struct HostWake HostWake;        /* opaque */

/* graft:  bind {struct Task *, ULONG sigbit}    spike: wrap a self-pipe / callback */
HostWake *hw_bind(/* target */);
void      hw_raise(HostWake *);          /* host thread → graft: Signal(task,sig); spike: write(pipe) */
void      hw_wait (HostWake *);          /* AROS task → graft: Wait(sig); spike: select(pipe) */
void      hw_free (HostWake *);
```

Per-driver, the seam is the *only* thing that differs from the shipped form:

- **bsdsocket** `PumpSig` → `hw_*`: `ps_wake`=`hw_raise`, `ps_wait`=`hw_wait`; at graft
  `(struct Task*, AllocSignal()-bit)`, `hw_raise`→`Signal`, `hw_wait`→`Wait`. The kqueue
  pump body is unchanged.
- **clipboard** `PBSignalFn` → `hw_raise` of the sync task's bit; the `changeCount`
  poller calls it only when `count != selfWriteToken` (R-W4).
- **audio** — see below; the ring *is* the decoupling, so per-callback `hw_raise` is
  usually unnecessary.

## The RT exception (CoreAudio)

The audio RT render callback is the strictest case and deliberately does **not** use the
wake seam per call. The callback runs on a CoreAudio real-time thread and must obey two
hard rules (`[PUB]` Core Audio render-callback contract):

- **It calls no AROS LVO and never blocks or allocates** — it only `memcpy`s from the
  lock-free SPSC ring (R-W4).
- The host RT thread is born **signal-masked** (`pthread_sigmask(SIG_BLOCK, …)`) so
  AROS's `SIGALRM` scheduler tick never lands on it.

Pacing flows through the *ring*, not a wake: the AHI slave task (producer) fills ahead;
the callback (consumer) drains. A wake is only needed if the producer must block waiting
for ring space — and that wait belongs to the slave task on the AROS thread, using the
same `hw_*` seam, never to the RT thread.

## Status

This is a **design contract**, not yet a single shared compilation unit — each spike
currently implements it locally and conformantly (atomics in place, ownership enforced,
re-probe/`hw_*` seam exposed). At graft, the three drivers collapse onto one `host_wake`
implementation; until then, this doc is the checklist a reviewer holds each host-thread
driver to. Conformance today: bsdsocket ✓ (single-owner-fd, atomic pump, re-probe),
clipboard ✓ (atomics, self-write token), audio ✓ (SPSC, RT no-LVO, signal-masked).
