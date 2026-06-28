/* bsdsock_abi_test.c — dlopen-based ABI conformance for libbsdsockhost.dylib ([NABI]).
 *
 * Models the HostLib_Open(dlopen) + HostLib_GetPointer(dlsym) path the AROS-side
 * bsdsocket.library (arch/all-unix/bsdsocket) uses to reach the host pump: dlopen
 * the dylib, resolve every bsdsock.exports symbol (errcount must be 0), then drive
 * the pump through the GRAFT SEAM — ps_create_cb() with a wake callback (the
 * stand-in for Signal(task, readySig)) — to prove readiness travels across the
 * dlopen boundary, not just within a statically-linked test. Hermetic: a local
 * AF_UNIX socketpair, no network, no entitlement. Plain C, links none of the
 * pump/shim .c — the REAL load boundary.
 *
 * Independent work: no third-party implementation source — emulator, agent,
 * driver, or otherwise — was read, searched, or consulted; any resemblance is
 * coincidental. Built from bsdsock_host.h + POSIX/Apple man pages only.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include "bsdsock_host.h"

/* The wake callback the AROS side would back with Signal(): here it just records
 * that the pump fired, from the pump thread, across the dlopen boundary. */
static _Atomic int g_woke = 0;
static void on_wake(void *cookie) { (void)cookie; atomic_store(&g_woke, 1); }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1]
                     : getenv("BSDSOCK_DYLIB") ? getenv("BSDSOCK_DYLIB")
                     : "build/libbsdsockhost.dylib";
    printf("[NABI] dlopen-based conformance test against %s\n", path);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[NABI] FAIL dlopen: %s\n", dlerror()); return 1; }

    /* Every exported contract symbol must resolve (HostLib_GetInterface == 0). */
    static const char *names[] = {
        "pump_start", "pump_stop", "pump_register", "pump_unregister",
        "pump_drain", "pump_wake_kqueue",
        "ps_create", "ps_create_cb", "ps_destroy", "ps_wait", "ps_wake",
        "hs_set_nonblock", "hs_socket", "hs_close", "hs_connect",
        "hs_send", "hs_send_all", "hs_recv", "hs_recv_nonblock",
    };
    int n = (int)(sizeof(names) / sizeof(names[0])), errc = 0;
    for (int i = 0; i < n; i++) {
        void *s = dlsym(h, names[i]);
        if (!s) { errc++; printf("[NABI]   MISSING  %s\n", names[i]); }
        else    printf("[NABI]   resolved [%2d] %s\n", i, names[i]);
    }
    printf("[NABI]   errcount = %d (HostLib_GetInterface contract: MUST be 0)\n", errc);

    /* Resolve the handful we actually drive (through the boundary, by pointer). */
    int      (*p_start)(void)                              = (int(*)(void))                              dlsym(h, "pump_start");
    void     (*p_stop )(void)                              = (void(*)(void))                             dlsym(h, "pump_stop");
    int      (*p_reg  )(int, unsigned, PumpSig *)          = (int(*)(int, unsigned, PumpSig *))          dlsym(h, "pump_register");
    int      (*p_unreg)(int, PumpSig *)                    = (int(*)(int, PumpSig *))                    dlsym(h, "pump_unregister");
    int      (*p_drain)(PumpSig *, PumpReady *, int)       = (int(*)(PumpSig *, PumpReady *, int))       dlsym(h, "pump_drain");
    PumpSig *(*p_mkcb )(void(*)(void *), void *)           = (PumpSig*(*)(void(*)(void *), void *))      dlsym(h, "ps_create_cb");
    void     (*p_psdel)(PumpSig *)                         = (void(*)(PumpSig *))                        dlsym(h, "ps_destroy");

    int seam_ok = 0, drain_ok = 0;
    if (p_start && p_stop && p_reg && p_unreg && p_drain && p_mkcb && p_psdel) {
        int sv[2] = { -1, -1 };
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && p_start() == 0) {
            fcntl(sv[0], F_SETFL, O_NONBLOCK);          /* the side "AROS" waits on */
            PumpSig *sig = p_mkcb(on_wake, NULL);       /* the graft seam: cb, not self-pipe */

            atomic_store(&g_woke, 0);
            p_reg(sv[0], PS_WANT_READ, sig);
            ssize_t w = write(sv[1], "x", 1);           /* peer makes sv[0] readable */
            (void)w;

            /* The pump must fire the callback (≈immediately); bound the wait. */
            for (int i = 0; i < 200 && !atomic_load(&g_woke); i++) usleep(10 * 1000);
            seam_ok = atomic_load(&g_woke);

            PumpReady rd[8];
            int got = p_drain(sig, rd, 8);
            drain_ok = (got == 1 && rd[0].fd == sv[0] && (rd[0].ready & PS_WANT_READ));

            p_unreg(sv[0], sig);
            p_psdel(sig);
            p_stop();
            close(sv[0]); close(sv[1]);
        }
    }

    int ok = (errc == 0 && seam_ok && drain_ok);
    printf("[NABI]   wake-callback via dylib: %s ; pump_drain reports the ready fd: %s\n",
           seam_ok ? "ok" : "FAIL", drain_ok ? "ok" : "FAIL");
    printf("[NABI] %s errcount=%d (%d symbols), readiness wake + drain through the dlopen boundary\n",
           ok ? "PASS" : "FAIL", errc, n);
    dlclose(h);
    return ok ? 0 : 1;
}
