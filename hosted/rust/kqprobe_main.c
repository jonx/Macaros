/* kqprobe_main.c -- C:KqProbe, the host-kqueue directory-watch check.
 *
 * The question this answers: can AROS code watch a HOST directory for changes
 * through hostlib.resource, with events arriving at poll cadence rather than
 * at tree-walk cadence? This is the mechanism the editor's file watcher wants
 * (per-directory events instead of walking the whole project every 2 s), so
 * prove it in a command that builds in seconds before wiring it into Rust.
 *
 * Usage: KqProbe <host-dir> [seconds]
 *   e.g. KqProbe /Users/jkn/AROS/Shared/lsptest 30
 * then touch/create files under that directory from the Mac and watch the
 * events print.
 *
 * Same hostlib pattern as C:CPUInfo's darwin branch: OpenResource, HostLib_Open
 * libSystem, HostLib_GetPointer, calls under HostLib_Lock. kevent is called
 * with a zero timeout only -- AROS must never block inside the host.
 *
 * darwin ABI declared locally (stable syscall ABI, same rationale as the other
 * glues): struct kevent is 32 bytes on 64-bit darwin, EVFILT_VNODE = -4,
 * O_EVTONLY = 0x8000 (an open mode that takes no reference that would block
 * unmounts -- fitting, since we only want the event stream).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/hostlib.h>
#include <exec/types.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- darwin bits, declared for the same reason the other glues declare
 *     what they use: this compiles against AROS headers only. --- */
struct host_kevent {
    IPTR  ident;          /* uintptr_t: the watched fd */
    WORD  filter;         /* int16_t  */
    UWORD flags;          /* uint16_t */
    ULONG fflags;         /* uint32_t: NOTE_* of what happened */
    IPTR  data;           /* intptr_t */
    APTR  udata;          /* void*: our watch cookie */
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
typedef int (*h_kevent_t)(int kq, const struct host_kevent *changes, int nch,
                          struct host_kevent *events, int nev,
                          const struct host_timespec *timeout);
typedef int (*h_open_t)(const char *path, int flags, ...);
typedef int (*h_close_t)(int fd);

int main(int argc, char **argv)
{
    APTR HostLibBase;
    void *libc;
    h_kqueue_t h_kqueue;
    h_kevent_t h_kevent;
    h_open_t   h_open;
    h_close_t  h_close;
    struct host_kevent reg, got[8];
    struct host_timespec zero = { 0, 0 };
    const char *dir;
    int seconds = 30;
    int kq = -1, dfd = -1, n, i, ticks;
    int events_seen = 0;

    if (argc < 2) {
        printf("usage: KqProbe <host-dir> [seconds]\n");
        return 20;
    }
    dir = argv[1];
    if (argc > 2)
        seconds = atoi(argv[2]);

    HostLibBase = OpenResource("hostlib.resource");
    if (!HostLibBase) { printf("[KQ] FAIL no hostlib.resource\n"); return 20; }

    libc = HostLib_Open("libSystem.dylib", NULL);
    if (!libc) { printf("[KQ] FAIL cannot open libSystem\n"); return 20; }

    h_kqueue = (h_kqueue_t)HostLib_GetPointer(libc, "kqueue", NULL);
    h_kevent = (h_kevent_t)HostLib_GetPointer(libc, "kevent", NULL);
    h_open   = (h_open_t)  HostLib_GetPointer(libc, "open",   NULL);
    h_close  = (h_close_t) HostLib_GetPointer(libc, "close",  NULL);
    if (!h_kqueue || !h_kevent || !h_open || !h_close) {
        printf("[KQ] FAIL missing symbols\n");
        return 20;
    }

    HostLib_Lock();
    kq  = h_kqueue();
    dfd = kq >= 0 ? h_open(dir, H_O_EVTONLY) : -1;
    if (dfd >= 0) {
        memset(&reg, 0, sizeof(reg));
        reg.ident  = (IPTR)dfd;
        reg.filter = H_EVFILT_VNODE;
        reg.flags  = H_EV_ADD | H_EV_CLEAR;
        reg.fflags = H_NOTE_WRITE | H_NOTE_EXTEND | H_NOTE_ATTRIB |
                     H_NOTE_DELETE | H_NOTE_RENAME | H_NOTE_LINK;
        reg.udata  = (APTR)0x1;
        n = h_kevent(kq, &reg, 1, NULL, 0, &zero);
    } else {
        n = -1;
    }
    HostLib_Unlock();

    if (kq < 0)  { printf("[KQ] FAIL kqueue() -> %d\n", kq); return 20; }
    if (dfd < 0) { printf("[KQ] FAIL host open(%s, O_EVTONLY) -> %d\n", dir, dfd); return 20; }
    if (n < 0)   { printf("[KQ] FAIL registering the watch\n"); return 20; }

    printf("[KQ] watching %s for %d s (kq fd %d, dir fd %d)\n", dir, seconds, kq, dfd);

    /* Poll at 250 ms: Delay(12) plus a zero-timeout kevent drain. */
    for (ticks = 0; ticks < seconds * 4; ticks++) {
        Delay(12);
        HostLib_Lock();
        n = h_kevent(kq, NULL, 0, got, 8, &zero);
        HostLib_Unlock();
        for (i = 0; i < n; i++) {
            events_seen++;
            printf("[KQ] event: fflags=0x%x%s%s%s%s%s%s (tick %d)\n",
                   (unsigned)got[i].fflags,
                   (got[i].fflags & H_NOTE_WRITE)  ? " WRITE"  : "",
                   (got[i].fflags & H_NOTE_EXTEND) ? " EXTEND" : "",
                   (got[i].fflags & H_NOTE_ATTRIB) ? " ATTRIB" : "",
                   (got[i].fflags & H_NOTE_DELETE) ? " DELETE" : "",
                   (got[i].fflags & H_NOTE_RENAME) ? " RENAME" : "",
                   (got[i].fflags & H_NOTE_LINK)   ? " LINK"   : "",
                   ticks);
        }
    }

    HostLib_Lock();
    h_close(dfd);
    h_close(kq);
    HostLib_Unlock();
    HostLib_Close(libc, NULL);

    if (events_seen > 0) {
        printf("KQPROBE PASS (%d events)\n", events_seen);
        return 0;
    }
    printf("KQPROBE: no events (did anything change under the dir?)\n");
    return 5;
}
