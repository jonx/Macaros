/* bsdsock_pump.c — the kqueue host pump thread + the readiness-signal seam.
 *
 * Implemented clean-room from docs/features/bsdsocket-net/spec.md (R-PUMP,
 * R-PARK, R-RACE, "The bridge"). No GPL emulator source (WinUAE/FS-UAE/Amiberry/
 * E-UAE/Janus-UAE/vAmiga) was read, searched, or consulted. POSIX/Apple man
 * pages [PUB] (kqueue(2)/kevent(2)/EVFILT_READ/EVFILT_WRITE, the self-pipe
 * trick, pthread) + this project's H9 Wait/Signal discipline [OURS] only.
 *
 * This is the only piece with no in-tree precedent (spec): one real OS pthread
 * that blocks in kevent() and, on readiness, raises a per-target wake (the
 * stand-in for an AROS Signal). It never touches socket DATA (no recv in the
 * pump) and never runs AROS code (spec R-PUMP), so it cannot perturb the
 * single-underlying-thread H4/H6 scheduler invariant.
 *
 * Synchronisation note (spec R-RACE): the pump is a genuine SECOND OS thread, so
 * the shared registration set + per-sig readiness stash are guarded by a real
 * pthread_mutex — the spec is explicit that the H6 "Forbid is just a compiler
 * barrier" shortcut does NOT cross this true-thread boundary.
 */
#include "bsdsock_host.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <sys/time.h>

/* ===== the per-target wake primitive (Signal stand-in) ===================== */
/* A self-pipe: ps_wait() reads the read end (blocks in the kernel, no spin),
 * ps_wake() writes one byte. A `latched` flag gives the spec R-RACE "check
 * before park" guarantee — a wake that races ahead of ps_wait() is still seen.
 * Coalescing: at most one byte is ever in flight, so N wakes collapse to one
 * (spec R-RACE: correctness never depends on the COUNT of signals). */
struct PumpSig {
    int  rfd, wfd;          /* self-pipe: read end, write end */
    int  latched;           /* 1 = a wake is pending (guarded by mtx) */
    pthread_mutex_t mtx;
};

PumpSig *ps_create(void) {
    PumpSig *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int fds[2];
    if (pipe(fds) != 0) { free(s); return NULL; }
    /* Non-blocking both ends so ps_wake() never blocks the pump thread and a
     * spurious double-read in ps_wait() can't hang. */
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    s->rfd = fds[0];
    s->wfd = fds[1];
    s->latched = 0;
    pthread_mutex_init(&s->mtx, NULL);
    return s;
}

void ps_destroy(PumpSig *s) {
    if (!s) return;
    close(s->rfd);
    close(s->wfd);
    pthread_mutex_destroy(&s->mtx);
    free(s);
}

void ps_wake(PumpSig *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mtx);
    if (!s->latched) {
        s->latched = 1;
        char b = 1;
        ssize_t n = write(s->wfd, &b, 1);   /* one byte; coalesced */
        (void)n;
    }
    pthread_mutex_unlock(&s->mtx);
}

int ps_wait(PumpSig *s, int timeout_ms) {
    if (!s) return 0;
    /* Check-before-park (spec R-RACE): if a wake already latched, consume it and
     * return WITHOUT sleeping. Mirrors exec Wait() testing tc_SigRecvd first. */
    check_latch:
    pthread_mutex_lock(&s->mtx);
    if (s->latched) {
        s->latched = 0;
        char drain[64];
        while (read(s->rfd, drain, sizeof drain) > 0) { /* empty the pipe */ }
        pthread_mutex_unlock(&s->mtx);
        return 1;
    }
    pthread_mutex_unlock(&s->mtx);

    /* Park in the kernel on the pipe read end (no busy-spin). select() with a
     * timeout is fine here — this is the WAITING task, not the underlying AROS
     * scheduler thread; the spike has no SIGALRM scheduler of its own. */
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s->rfd, &rs);
    struct timeval tv, *ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int r = select(s->rfd + 1, &rs, NULL, NULL, ptv);
    if (r < 0) {
        if (errno == EINTR) goto check_latch;  /* retry on signal */
        return 0;
    }
    if (r == 0) return 0;                       /* timeout, no wake */
    /* Readable: consume the latch + drain. Loop back through the latch check so
     * we report exactly "woken". */
    goto check_latch;
}

/* ===== the registration set + per-sig readiness stash ====================== */
/* One flat table of registrations; small N for the spike, linear scan is fine.
 * Guarded by reg_mtx because the pump thread reads it while AROS-side callers
 * mutate it (spec R-RACE: real lock across the true-thread boundary). */
struct Reg {
    int      fd;
    unsigned want;          /* PS_WANT_* the caller registered */
    unsigned ready;         /* PS_WANT_* the pump has observed ready (stash) */
    PumpSig *sig;
    int      in_use;
};

#define MAX_REGS 256

static struct Reg     g_regs[MAX_REGS];
static pthread_mutex_t reg_mtx = PTHREAD_MUTEX_INITIALIZER;

static int       g_kq = -1;
static int       g_ctl_r = -1, g_ctl_w = -1;   /* pump control self-pipe */
static pthread_t g_pump;

/* Pump-control flags shared across the true thread boundary (spec R-RACE: the
 * pump is a genuine SECOND OS thread, so the H6 "Forbid is just a compiler
 * barrier" shortcut does NOT apply — a plain int write here is a data race).
 *
 *  - g_pump_should_stop: written by the controlling pthread in pump_stop(),
 *    read by the pump thread in pump_main(). Made _Atomic so the store/load
 *    are sequenced (acquire/release via the default seq_cst ordering); the
 *    control-pipe wake still does the actual kevent() interruption, the atomic
 *    just guarantees the pump observes the stop request without a torn read.
 *  - g_pump_running: written by pump_start()/pump_stop() (controlling thread)
 *    and read by both; the pump thread never writes it, but it is read on the
 *    AROS-side fast paths, so atomic too for a clean cross-thread publish of
 *    "the pump exists / the kqueue is live". */
static _Atomic int g_pump_should_stop = 0;
static _Atomic int g_pump_running     = 0;

/* Find a registration by (fd, sig). Caller holds reg_mtx. */
static struct Reg *find_reg(int fd, PumpSig *sig) {
    for (int i = 0; i < MAX_REGS; i++)
        if (g_regs[i].in_use && g_regs[i].fd == fd && g_regs[i].sig == sig)
            return &g_regs[i];
    return NULL;
}

/* Find ANY registration for a host fd, regardless of owner. Caller holds
 * reg_mtx. Used to ENFORCE the single-owner-fd contract (see pump_register):
 * a host fd belongs to exactly one PumpSig, because a socket belongs to exactly
 * one AROS task (spec "SocketBase — the per-task open contract": the fd table is
 * per-task, a descriptor is meaningful only to the SocketBase that created it).
 * So if some OTHER sig already owns this fd, registering it is a contract
 * violation, not a fan-out request — and clearing the fd's kqueue filters on
 * either owner's unregister would starve the other (this is FINDING 4). */
static struct Reg *find_fd_owner(int fd) {
    for (int i = 0; i < MAX_REGS; i++)
        if (g_regs[i].in_use && g_regs[i].fd == fd)
            return &g_regs[i];
    return NULL;
}

/* Apply one filter change. Each change is its own kevent() call: kevent stops
 * processing the changelist at the FIRST erroring entry, and a benign EV_DELETE
 * on a filter that was never added returns ENOENT — issuing them separately
 * keeps one harmless delete from aborting a real add. Caller holds reg_mtx. */
static void set_filter(int fd, int16_t filter, int want, void *udata) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, want ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, udata);
    kevent(g_kq, &ev, 1, NULL, 0, NULL);        /* ENOENT on a stale delete is fine */
}

/* Apply a registration's wanted filters to the kqueue (level-triggered: no
 * EV_CLEAR). EVFILT_READ/EVFILT_WRITE are independent filters: enable the wanted
 * ones, delete the unwanted ones. udata carries the Reg* so the pump can route a
 * readiness back to its PumpSig. Caller holds reg_mtx. */
static void apply_filters(struct Reg *r) {
    set_filter(r->fd, EVFILT_READ,  r->want & PS_WANT_READ,  r);
    set_filter(r->fd, EVFILT_WRITE, r->want & PS_WANT_WRITE, r);
}

/* Remove both filters for an fd. Caller holds reg_mtx. */
static void clear_filters(int fd) {
    set_filter(fd, EVFILT_READ,  0, NULL);
    set_filter(fd, EVFILT_WRITE, 0, NULL);
}

void pump_wake_kqueue(void) {
    if (g_ctl_w >= 0) {
        char b = 1;
        ssize_t n = write(g_ctl_w, &b, 1);
        (void)n;
    }
}

/* The pump thread body (spec R-PUMP): block in kevent(); on a registered fd
 * ready, stash the direction keyed by target sig + raise that sig's wake; on the
 * control pipe, drain it and loop (to pick up new registrations / shut down). */
static void *pump_main(void *arg) {
    (void)arg;
    struct kevent evs[64];
    for (;;) {
        int n = kevent(g_kq, NULL, 0, evs, 64, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                              /* fatal kqueue error */
        }
        if (g_pump_should_stop) break;

        for (int i = 0; i < n; i++) {
            struct kevent *e = &evs[i];
            if (e->filter == EVFILT_READ && (int)e->ident == g_ctl_r) {
                /* control wake: drain the self-pipe, then re-loop. */
                char drain[64];
                while (read(g_ctl_r, drain, sizeof drain) > 0) { }
                continue;
            }
            /* A registered socket became ready. e->udata is its Reg*. Validate
             * under the lock (the Reg may have been unregistered meanwhile). */
            struct Reg *r = (struct Reg *)e->udata;
            unsigned dir = (e->filter == EVFILT_READ)  ? PS_WANT_READ  :
                           (e->filter == EVFILT_WRITE) ? PS_WANT_WRITE : 0;
            if (!dir) continue;

            PumpSig *sig = NULL;
            pthread_mutex_lock(&reg_mtx);
            if (r && r->in_use && r->fd == (int)e->ident && (r->want & dir)) {
                r->ready |= dir;                /* stash for pump_drain */
                sig = r->sig;
            }
            pthread_mutex_unlock(&reg_mtx);

            /* Raise the wake OUTSIDE the reg lock (spec R-PUMP step 2). */
            if (sig) ps_wake(sig);
        }
    }
    return NULL;
}

int pump_start(void) {
    if (g_pump_running) return 0;

    g_kq = kqueue();
    if (g_kq < 0) return -1;

    int fds[2];
    if (pipe(fds) != 0) { close(g_kq); g_kq = -1; return -1; }
    g_ctl_r = fds[0];
    g_ctl_w = fds[1];
    fcntl(g_ctl_r, F_SETFL, O_NONBLOCK);
    fcntl(g_ctl_w, F_SETFL, O_NONBLOCK);

    /* Register the control pipe's read end so kevent() wakes on a control byte
     * (the self-pipe trick, spec R-PUMP "It must itself be wakeable"). */
    struct kevent ctl;
    EV_SET(&ctl, g_ctl_r, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(g_kq, &ctl, 1, NULL, 0, NULL) != 0) {
        close(g_ctl_r); close(g_ctl_w); close(g_kq);
        g_ctl_r = g_ctl_w = g_kq = -1;
        return -1;
    }

    g_pump_should_stop = 0;
    if (pthread_create(&g_pump, NULL, pump_main, NULL) != 0) {
        close(g_ctl_r); close(g_ctl_w); close(g_kq);
        g_ctl_r = g_ctl_w = g_kq = -1;
        return -1;
    }
    g_pump_running = 1;
    return 0;
}

void pump_stop(void) {
    if (!g_pump_running) return;
    g_pump_should_stop = 1;
    pump_wake_kqueue();                         /* break the blocked kevent() */
    pthread_join(g_pump, NULL);
    g_pump_running = 0;

    close(g_ctl_r); close(g_ctl_w); close(g_kq);
    g_ctl_r = g_ctl_w = g_kq = -1;

    /* Forget any leftover registrations (test owns the fds + sigs). */
    pthread_mutex_lock(&reg_mtx);
    memset(g_regs, 0, sizeof g_regs);
    pthread_mutex_unlock(&reg_mtx);
}

int pump_register(int hostfd, unsigned want, PumpSig *sig) {
    if (!sig || g_kq < 0) return -1;
    pthread_mutex_lock(&reg_mtx);
    struct Reg *r = find_reg(hostfd, sig);
    if (!r) {
        /* Single-owner-fd contract (FINDING 4, option a): a host fd is owned by
         * exactly one PumpSig. If a DIFFERENT sig already owns this fd, reject —
         * this is a misuse (a socket belongs to one task, spec SocketBase), and
         * silently sharing the fd's kqueue filters would let one unregister
         * starve the other. Guard it explicitly rather than leaving it implicit. */
        struct Reg *owner = find_fd_owner(hostfd);
        if (owner) { pthread_mutex_unlock(&reg_mtx); return -1; }
        for (int i = 0; i < MAX_REGS; i++)
            if (!g_regs[i].in_use) { r = &g_regs[i]; break; }
        if (!r) { pthread_mutex_unlock(&reg_mtx); return -1; }
        r->in_use = 1;
        r->fd     = hostfd;
        r->sig    = sig;
        r->ready  = 0;
    }
    r->want = want;
    apply_filters(r);
    pthread_mutex_unlock(&reg_mtx);
    pump_wake_kqueue();
    return 0;
}

int pump_unregister(int hostfd, PumpSig *sig) {
    if (g_kq < 0) return -1;
    pthread_mutex_lock(&reg_mtx);
    /* Owner-matched: only the registering sig may tear its fd down. Matching on
     * (fd, sig) (not fd alone) means a stray unregister with the wrong sig is a
     * no-op rather than deleting another owner's filters (FINDING 4). Under the
     * single-owner-fd contract there is at most one owner, so clearing the fd's
     * kqueue filters here only ever affects that one owner. */
    struct Reg *r = find_reg(hostfd, sig);
    if (r) {
        clear_filters(hostfd);
        memset(r, 0, sizeof *r);
    }
    pthread_mutex_unlock(&reg_mtx);
    return 0;
}

int pump_drain(PumpSig *sig, PumpReady *out, int max) {
    if (!sig) return 0;
    int count = 0;
    pthread_mutex_lock(&reg_mtx);
    for (int i = 0; i < MAX_REGS && count < max; i++) {
        struct Reg *r = &g_regs[i];
        if (r->in_use && r->sig == sig && r->ready) {
            out[count].fd    = r->fd;
            out[count].ready = r->ready;
            r->ready = 0;                       /* consume the stash */
            count++;
        }
    }
    pthread_mutex_unlock(&reg_mtx);
    return count;
}
