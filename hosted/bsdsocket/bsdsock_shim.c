/* bsdsock_shim.c — the non-blocking host socket op wrappers (the bsdsocket ABI).
 *
 * Implemented clean-room from docs/features/bsdsocket-net/spec.md (R-NONBLOCK,
 * R-PARK, the "socket/connect/send/recv/close" LVO surface rows 5/9/11/13/20).
 * No GPL emulator source (WinUAE/FS-UAE/Amiberry/E-UAE/Janus-UAE/vAmiga) was
 * read, searched, or consulted. POSIX/Apple man pages [PUB] (socket(2),
 * fcntl(2) O_NONBLOCK, connect(2) EINPROGRESS/SO_ERROR, recv(2)/send(2)
 * EWOULDBLOCK) + this project's H11 block-then-reply loop [OURS] only.
 *
 * Each wrapper maps one would-block host result into the spec's
 * register-then-Wait-then-retry park (R-PARK), driving the kqueue pump
 * (bsdsock_pump.c) and parking on the readiness-signal seam (PumpSig). The seam
 * is exactly where the AROS graft swaps the self-pipe ps_wait()/ps_wake() for
 * exec Wait(readySig)/Signal(task,readySig) — the wrapper logic is unchanged.
 *
 * The wrappers take a PumpSig* in place of the spec's per-task SocketBase: in
 * the standalone proof that is the wake target; in the graft it is the calling
 * task's SocketBase carrying readySig. This keeps the seam (which is what the
 * later AROS Signal integration drops into) visible in the signature.
 */
#include "bsdsock_shim.h"
#include "bsdsock_host.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>

/* Zero-timeout re-probe of an fd's readiness (spec R-RACE "re-probe after wake,
 * don't trust the wake" / R-WAITSELECT step 4 "non-blocking re-probe is the
 * source of truth"). want_write != 0 probes writability, else readability.
 * Returns 1 if ready now, 0 if not. The shared-PumpSig design means a wake meant
 * for one fd can surface in another op's ps_wait (a coalesced signal); this
 * re-probe is what makes that harmless — a spurious wake yields "not ready" and
 * the caller simply parks again. */
static int fd_ready_now(int fd, int want_write) {
    fd_set s;
    FD_ZERO(&s);
    FD_SET(fd, &s);
    struct timeval z = { 0, 0 };
    int r = want_write ? select(fd + 1, NULL, &s, NULL, &z)
                       : select(fd + 1, &s, NULL, NULL, &z);
    return (r > 0 && FD_ISSET(fd, &s)) ? 1 : 0;
}

/* A would-block deadline so a wrong server can never hang a wrapper forever
 * (defence in depth alongside the harness watchdog, spec "watchdog so it can
 * never hang the loop"). Generous vs. localhost latency. */
#define PARK_TIMEOUT_MS 5000

/* R-NONBLOCK: set O_NONBLOCK immediately. Returns 0 on success. [PUB] fcntl(2) */
int hs_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* socket() + force non-blocking (spec LVO 5 "create + O_NONBLOCK"). */
int hs_socket(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd < 0) return -1;
    if (hs_set_nonblock(fd) != 0) { close(fd); return -1; }
    return fd;
}

int hs_close(int fd, PumpSig *sig) {
    if (sig) pump_unregister(fd, sig);          /* spec LVO 20 pump-unregister */
    return close(fd);
}

/* connect() the non-blocking way (spec LVO 9 + R-PARK connect special case):
 * EINPROGRESS -> register for WRITE-readiness -> park -> read SO_ERROR. Returns
 * 0 on a fully-established connection, -1 with errno set otherwise. */
int hs_connect(int fd, const struct sockaddr *name, unsigned namelen, PumpSig *sig) {
    int r = connect(fd, name, namelen);
    if (r == 0) return 0;                        /* connected immediately */
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) return -1;

    /* Park on write-readiness via the pump (spec R-PARK connect case), then
     * RE-PROBE (spec R-RACE): a shared PumpSig means a wake meant for another fd
     * can surface here, so do not trust the wake — confirm the fd is genuinely
     * write-ready before reading SO_ERROR, and park again otherwise. */
    pump_register(fd, PS_WANT_WRITE, sig);
    int done = 0, rc = 0, spent = 0;
    while (!done) {
        if (!fd_ready_now(fd, 1)) {              /* not writable yet -> park */
            /* Park with a SHORT slice and re-probe (spec R-RACE): a shared
             * PumpSig can coalesce away a wake meant for this fd, so never trust
             * a single wait — bound each park and re-probe, capping total wait. */
            if (spent >= PARK_TIMEOUT_MS) { errno = ETIMEDOUT; rc = -1; break; }
            ps_wait(sig, 100);
            spent += 100;
            continue;                            /* re-probe after the slice */
        }
        /* Writable: SO_ERROR is the asynchronous-connect result [PUB]. */
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) { rc = -1; break; }
        if (soerr != 0) { errno = soerr; rc = -1; break; }
        done = 1;
    }
    pump_unregister(fd, sig);
    return rc;
}

/* send() with park-on-EWOULDBLOCK for write-readiness (spec LVO 11 + R-PARK).
 *
 * BSD send() semantics (FINDING 3): one LOGICAL send that may return a PARTIAL
 * byte count. bsdsocket.library must preserve single-call/partial-return
 * semantics — AROS apps depend on the count, so this does NOT loop to completion.
 * If the very first send would block, it parks once for write-readiness and
 * retries the single send (so a caller that catches the socket momentarily full
 * still makes progress); but once ANY bytes have been accepted it returns that
 * (possibly partial) count immediately, exactly like a non-blocking BSD send on
 * a buffer that drained partway. Returns the bytes sent (>=1 on success), 0 if
 * the peer closed, or -1 with errno.
 *
 * Callers that genuinely need every byte delivered must use hs_send_all(). */
long hs_send(int fd, const void *buf, unsigned len, int flags, PumpSig *sig) {
    int spent = 0;
    for (;;) {
        ssize_t n = send(fd, buf, len, flags);
        if (n >= 0) return (long)n;             /* one logical send; may be partial */
        if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
        /* Would block on the FIRST send: park for write-ready, then re-issue the
         * single send (the re-issue is the re-probe — spec R-RACE). Short park
         * slices + a total cap so a wake coalesced away on the shared PumpSig
         * re-probes promptly, not after the whole timeout. */
        pump_register(fd, PS_WANT_WRITE, sig);
        if (spent >= PARK_TIMEOUT_MS) { pump_unregister(fd, sig); errno = ETIMEDOUT; return -1; }
        ps_wait(sig, 100);
        spent += 100;
        pump_unregister(fd, sig);
    }
}

/* hs_send_all — write the ENTIRE buffer, parking on write-readiness between
 * partial sends (FINDING 3: the write-all helper kept SEPARATE from hs_send so
 * the single-call BSD semantics above stay intact). Built on hs_send so each
 * chunk preserves the non-blocking + park discipline. Returns `len` on full
 * delivery, 0 if the peer closed before all bytes went out, or -1 with errno.
 * Use this for test/caller code that wants guaranteed full delivery (NOT for the
 * bsdsocket.library send() LVO, which must surface partial counts). */
long hs_send_all(int fd, const void *buf, unsigned len, int flags, PumpSig *sig) {
    const char *p = (const char *)buf;
    unsigned left = len;
    while (left > 0) {
        long n = hs_send(fd, p, left, flags, sig);
        if (n < 0) return -1;
        if (n == 0) break;                      /* peer closed mid-write */
        p += n; left -= (unsigned)n;
    }
    return (long)(len - left);
}

/* recv() with park-on-EWOULDBLOCK for read-readiness (spec LVO 13 + R-PARK).
 * One non-blocking recv; if it would block, register/park/re-issue. Returns the
 * byte count (0 = peer closed), or -1 with errno. */
long hs_recv(int fd, void *buf, unsigned len, int flags, PumpSig *sig) {
    int spent = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, len, flags);
        if (n >= 0) return (long)n;
        if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
        /* Would block: park for read-ready, then RE-ISSUE — the re-issue (a
         * non-blocking recv) is the source of truth, so a spurious/coalesced
         * wake just loops (spec R-PARK/R-RACE). Short park slices + a total cap
         * so a wake coalesced away on the shared PumpSig re-probes promptly. */
        pump_register(fd, PS_WANT_READ, sig);
        if (spent >= PARK_TIMEOUT_MS) { pump_unregister(fd, sig); errno = ETIMEDOUT; return -1; }
        ps_wait(sig, 100);
        spent += 100;
        pump_unregister(fd, sig);
    }
}

/* A bare non-blocking recv that does NOT park — used by the [N-3] proof to
 * observe an EWOULDBLOCK directly before the pump later wakes the parking path.
 * Returns the recv() result verbatim (errno preserved). */
long hs_recv_nonblock(int fd, void *buf, unsigned len, int flags) {
    return (long)recv(fd, buf, len, flags);
}
