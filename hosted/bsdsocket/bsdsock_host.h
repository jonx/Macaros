/* bsdsock_host.h — the host-side ABI seam for bsdsocket.library on aarch64-darwin.
 *
 * Implemented from docs/features/bsdsocket-net/spec.md (the
 * "HostSockInterface", "The concurrency model" R-NONBLOCK/R-PUMP/R-PARK/
 * R-WAITSELECT/R-RACE, and "The descriptor table" sections). Independent work:
 * no third-party implementation source — emulator, agent, driver, or otherwise —
 * was read, searched, or consulted in producing it, and any resemblance to
 * existing implementations is coincidental. Implemented only from that spec +
 * POSIX/Apple man pages [PUB] + this project's own H9/H10/H11 spikes
 * (hosted/signal.c, hosted/msgport.c, hosted/device.c) [OURS].
 *
 * This header is the only contact surface the later AROS-side bsdsocket.library
 * (AROS crosstools) calls. The host shim pulls no AROS headers; the AROS side
 * pulls no macOS socket headers — the socket calls are POSIX [PUB] and the
 * pump-control surface below is [OURS] (tiny, ASCII, independently derived).
 *
 * THE READINESS-SIGNAL SEAM (the load-bearing design point of this spike):
 * the spec's pump "raises an AROS Signal" when an fd becomes ready (R-PUMP step
 * 2). There is no AROS side in this standalone proof, so the abstract "signal"
 * is stood in by a host primitive — a per-registrant self-pipe whose read end a
 * task Wait()s on (see ps_wait/ps_wake below). The wake is REAL (a byte is
 * written through a pipe and a blocked reader unblocks), never a busy-spin. When
 * the AROS bsdsocket.library is grafted, pump_set_signal_cb() lets the AROS side
 * replace this self-pipe wake with exec Signal(task, readySig) — the pump code
 * does not change, only the callback it invokes does. That is the seam that maps
 * onto WaitSelect/Signal (spec R-PUMP/R-PARK/R-WAITSELECT).
 */
#ifndef BSDSOCK_HOST_H
#define BSDSOCK_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 * Readiness directions (spec R-PUMP "(host fd, direction, target SocketBase)")
 * "want" = read for recv/accept, write for send/connect. A registration may
 * want both (e.g. a duplex socket parked on either edge).
 * ------------------------------------------------------------------------- */
#define PS_WANT_READ   (1u << 0)
#define PS_WANT_WRITE  (1u << 1)

/* ------------------------------------------------------------------------- *
 * The readiness-signal seam — the stand-in for AROS Signal/Wait.
 *
 * Each "SocketBase" in the spec maps here to a PumpSig: an opaque per-target
 * wake primitive. In the AROS graft, this becomes a (struct Task *, ULONG
 * readySig) pair and pump_wake() becomes Signal(); here it is a self-pipe.
 * pump_register() is keyed by the PumpSig so the pump knows whom to wake
 * (spec R-PUMP step 1 "keyed by target SocketBase").
 * ------------------------------------------------------------------------- */
typedef struct PumpSig PumpSig;

/* Create / destroy a wake primitive (one per "SocketBase"/task in the graft). */
PumpSig *ps_create(void);
void     ps_destroy(PumpSig *);

/* Park the calling task until the pump (or another waker) raises this PumpSig.
 * Returns >0 if woken by a wake, 0 on timeout (timeout_ms < 0 = wait forever).
 * This is the stand-in for exec Wait(readySig) (spec R-PARK step 2). The
 * "check before park" guarantee (spec R-RACE) is honoured: a wake raised before
 * ps_wait() is not lost — it is latched and ps_wait() returns immediately. */
int      ps_wait(PumpSig *, int timeout_ms);

/* Raise the wake. Safe to call from the pump thread (the second OS thread).
 * Stand-in for Signal(task, readySig) (spec R-PUMP step 2). Idempotent /
 * coalescing: many wakes before one wait collapse to one (spec R-RACE
 * "correctness never depends on the count of signals"). */
void     ps_wake(PumpSig *);

/* ------------------------------------------------------------------------- *
 * The pump-control surface (spec "The bridge" / "pump-control surface"). [OURS]
 * Behaviour specified; internals are ours. All are safe to call from AROS-side
 * tasks; the pump serialises its own shared state with a real pthread_mutex
 * (spec R-RACE: a true second OS thread needs a genuine lock, not Forbid).
 * ------------------------------------------------------------------------- */

/* Start the single kqueue pump thread. Idempotent. Returns 0 on success. */
int  pump_start(void);

/* Stop the pump thread and release its kqueue. Joins the thread cleanly so the
 * process can exit without hanging (spec "exit cleanly / watchdog"). */
void pump_stop(void);

/* Register a host fd for readiness on `want` directions, waking `sig` when it
 * becomes ready (spec R-PARK step 1, R-WAITSELECT step 1). Re-registering an fd
 * with the SAME `sig` replaces its wanted directions. Level-triggered (spec
 * R-PUMP "edge-or-level deliberately ... level-triggered readiness so a
 * readiness that arrives before the task parks is not lost"). Returns 0 on
 * success, -1 on failure.
 *
 * SINGLE-OWNER-FD CONTRACT (settled, host-wake fanout decision): a host fd is
 * owned by EXACTLY ONE `sig`. This is sound for bsdsocket because a socket
 * descriptor belongs to exactly one AROS task (spec "SocketBase — the per-task
 * open contract": dTable/errno/signal masks are per-task; a descriptor is
 * meaningful only to the SocketBase that created it). Registering an fd that a
 * DIFFERENT `sig` already owns is a contract violation and returns -1 — the fd's
 * kqueue filters are NOT shared, so this is guarded explicitly rather than left
 * to corrupt the other owner's registration. */
int  pump_register(int hostfd, unsigned want, PumpSig *sig);

/* Drop a host fd from the registration set for this `sig` (spec R-PARK step 3 /
 * R-WAITSELECT step 4 "pump_unregister every fd"). Owner-matched: only the `sig`
 * that registered the fd tears it down; an unregister with a non-owning `sig` is
 * a safe no-op (it never deletes another owner's kqueue filters). Returns 0. */
int  pump_unregister(int hostfd, PumpSig *sig);

/* Drain the readiness the pump stashed for this `sig` since the last drain
 * (spec R-WAITSELECT step 4 "read the readiness the pump stashed ... pump_drain").
 * Writes up to max entries into out_fds[]/out_ready[] ({fd, ready-direction
 * bitmask}); returns the count. The drain is the FAST path; the spec also allows
 * a zero-timeout re-probe as the source of truth (R-RACE), which the shim does
 * for correctness. */
typedef struct { int fd; unsigned ready; } PumpReady;
int  pump_drain(PumpSig *sig, PumpReady *out, int max);

/* Explicitly wake the pump's kevent() (e.g. to force it to pick up a new
 * registration or to shut down) — the self-pipe/EVFILT_USER trick (spec R-PUMP
 * "It must itself be wakeable"). pump_register/unregister/stop call this; it is
 * exposed for completeness. */
void pump_wake_kqueue(void);

#ifdef __cplusplus
}
#endif

#endif /* BSDSOCK_HOST_H */
