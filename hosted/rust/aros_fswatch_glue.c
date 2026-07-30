/* aros_fswatch_glue.c -- host-kqueue directory watches for the editor's file
 * watcher (the mechanism C:KqProbe proves).
 *
 * AROS cannot report file changes, but the host can: each watch is a host
 * open(O_EVTONLY) of the mapped host path plus an EVFILT_VNODE registration on
 * one shared kqueue, all reached through hostlib.resource. The watch id IS the
 * host fd. Every host call is made under HostLib_Lock and kevent is only ever
 * called with a zero timeout: AROS must never block inside the host.
 *
 * The caller (crates/fs aros watcher in the editor) registers one watch per
 * directory and owns the path bookkeeping; this file owns nothing but fds.
 *
 * darwin ABI declared locally, same rationale as the other glues: this
 * compiles against AROS headers only, and the kevent layout and constants are
 * stable syscall ABI.
 */
#include <proto/exec.h>
#include <proto/hostlib.h>
#include <exec/types.h>

struct host_kevent {
    IPTR  ident;
    WORD  filter;
    UWORD flags;
    ULONG fflags;
    IPTR  data;
    APTR  udata;
};
struct host_timespec { long tv_sec; long tv_nsec; };

#define H_EVFILT_VNODE  (-4)
#define H_EV_ADD        0x0001
#define H_EV_CLEAR      0x0020
#define H_NOTE_DELETE   0x0001
#define H_NOTE_WRITE    0x0002
#define H_NOTE_EXTEND   0x0004
#define H_NOTE_ATTRIB   0x0008
#define H_NOTE_LINK     0x0010
#define H_NOTE_RENAME   0x0020
#define H_O_EVTONLY     0x8000

typedef int (*h_kqueue_t)(void);
typedef int (*h_kevent_t)(int, const struct host_kevent *, int,
                          struct host_kevent *, int, const struct host_timespec *);
typedef int (*h_open_t)(const char *, int, ...);
typedef int (*h_close_t)(int);

/* Not static: proto/hostlib.h declares this exact global for its inline
 * call stubs, and this file provides it for the whole link. */
APTR              HostLibBase;
static void      *g_libc;
static h_kqueue_t g_kqueue;
static h_kevent_t g_kevent;
static h_open_t   g_open;
static h_close_t  g_close;
static int        g_kq = -1;

/* One-time setup; returns 0 when the host side is usable. Not thread-safe by
 * itself, so the first call must come from one place (the watcher's init). */
int aros_fsw_init(void)
{
    if (g_kq >= 0)
        return 0;

    if (!HostLibBase) {
        HostLibBase = OpenResource("hostlib.resource");
        if (!HostLibBase)
            return -1;
    }
    if (!g_libc) {
        g_libc = HostLib_Open("libSystem.dylib", NULL);
        if (!g_libc)
            return -1;
    }
    g_kqueue = (h_kqueue_t)HostLib_GetPointer(g_libc, "kqueue", NULL);
    g_kevent = (h_kevent_t)HostLib_GetPointer(g_libc, "kevent", NULL);
    g_open   = (h_open_t)  HostLib_GetPointer(g_libc, "open",   NULL);
    g_close  = (h_close_t) HostLib_GetPointer(g_libc, "close",  NULL);
    if (!g_kqueue || !g_kevent || !g_open || !g_close)
        return -1;

    HostLib_Lock();
    g_kq = g_kqueue();
    HostLib_Unlock();
    return g_kq >= 0 ? 0 : -1;
}

/* Watch one host path (a directory, or a single file). Returns the watch id,
 * or -1. The id is the host fd, so ids are unique while the watch lives. */
int aros_fsw_add(const char *host_path)
{
    struct host_kevent reg;
    struct host_timespec zero = { 0, 0 };
    int fd, r;

    if (g_kq < 0 || !host_path)
        return -1;

    HostLib_Lock();
    fd = g_open(host_path, H_O_EVTONLY);
    if (fd >= 0) {
        reg.ident  = (IPTR)fd;
        reg.filter = H_EVFILT_VNODE;
        reg.flags  = H_EV_ADD | H_EV_CLEAR;
        reg.fflags = H_NOTE_WRITE | H_NOTE_EXTEND | H_NOTE_ATTRIB |
                     H_NOTE_DELETE | H_NOTE_RENAME | H_NOTE_LINK;
        reg.data   = 0;
        reg.udata  = (APTR)0;
        r = g_kevent(g_kq, &reg, 1, (struct host_kevent *)0, 0, &zero);
        if (r < 0) {
            g_close(fd);
            fd = -1;
        }
    }
    HostLib_Unlock();
    return fd;
}

void aros_fsw_remove(int id)
{
    if (g_kq < 0 || id < 0)
        return;
    /* Closing the fd removes its kqueue registration with it. */
    HostLib_Lock();
    g_close(id);
    HostLib_Unlock();
}

/* Drain pending events. Fills ids[] with the watch id per event and flags[]
 * with the NOTE_* mask of what happened there; returns the count. Never
 * blocks (zero timeout). */
int aros_fsw_poll(int *ids, unsigned int *flags, int cap)
{
    struct host_kevent got[16];
    struct host_timespec zero = { 0, 0 };
    int n, i, out = 0;

    if (g_kq < 0 || !ids || !flags || cap <= 0)
        return 0;

    while (out < cap) {
        int want = cap - out;
        if (want > 16)
            want = 16;
        HostLib_Lock();
        n = g_kevent(g_kq, (const struct host_kevent *)0, 0, got, want, &zero);
        HostLib_Unlock();
        if (n <= 0)
            break;
        for (i = 0; i < n; i++) {
            ids[out]   = (int)got[i].ident;
            flags[out] = got[i].fflags;
            out++;
        }
        if (n < want)
            break;
    }
    return out;
}
