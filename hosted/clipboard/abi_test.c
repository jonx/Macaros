/* abi_test.c — dlopen-based ABI conformance test for libpasteboard.dylib ([PBABI]).
 *
 * Models HostLib_Open(dlopen) + HostLib_GetPointer(dlsym) the AROS clipboard-sync
 * task uses: dlopen the dylib, resolve every pasteboard.h symbol (errcount must be
 * 0), then drive a text round-trip + a Latin-1->UTF-8 transcode through the resolved
 * pointers. Uses a uniquely-named NSPasteboard so it never touches the real one.
 * Plain C, links none of the .m — the REAL load boundary. Clean-room: pasteboard.h
 * + Apple docs only; no GPL source read.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pasteboard.h"

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1]
                     : getenv("PASTEBOARD_DYLIB") ? getenv("PASTEBOARD_DYLIB")
                     : "build/libpasteboard.dylib";
    printf("[PBABI] dlopen-based conformance test against %s\n", path);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[PBABI] FAIL dlopen: %s\n", dlerror()); return 1; }

    static const char *names[] = {
        "host_pb_get_text", "host_pb_set_text", "host_pb_change_count", "host_pb_free",
        "host_pb_poller_start", "host_pb_poller_stop", "host_pb_set_signal_cb",
        "host_latin1_to_utf8", "host_utf8_to_latin1", "host_pb_use_named",
        "host_pb_get_png", "host_pb_set_png",
    };
    int n = (int)(sizeof(names) / sizeof(names[0])), errc = 0;
    for (int i = 0; i < n; i++) {
        void *s = dlsym(h, names[i]);
        if (!s) { errc++; printf("[PBABI]   MISSING  %s\n", names[i]); }
        else    printf("[PBABI]   resolved [%2d] %s\n", i, names[i]);
    }
    printf("[PBABI]   errcount = %d (HostLib_GetInterface contract: MUST be 0)\n", errc);

    void  (*use_named)(const char *)          = (void(*)(const char *))         dlsym(h, "host_pb_use_named");
    long  (*set_text )(const char *, size_t)  = (long(*)(const char *, size_t)) dlsym(h, "host_pb_set_text");
    int   (*get_text )(char **, size_t *)     = (int(*)(char **, size_t *))     dlsym(h, "host_pb_get_text");
    void  (*pb_free  )(void *)                = (void(*)(void *))               dlsym(h, "host_pb_free");
    int   (*l2u      )(const unsigned char *, size_t, char **, size_t *)
                                              = (int(*)(const unsigned char *, size_t, char **, size_t *))
                                                dlsym(h, "host_latin1_to_utf8");

    int rt_ok = 0, tr_ok = 0; long cc = -1;
    if (use_named && set_text && get_text && pb_free) {
        use_named("me.jkn.aros.pbabi.test");          /* never the user's clipboard */
        cc = set_text("hi", 2);
        char *out = NULL; size_t len = 0;
        int rc = get_text(&out, &len);
        rt_ok = (rc && out && len == 2 && memcmp(out, "hi", 2) == 0);
        if (out) pb_free(out);
    }
    if (l2u && pb_free) {
        unsigned char lat[1] = { 0xE9 };              /* 'é' in Latin-1 */
        char *u = NULL; size_t ul = 0;
        int tr = l2u(lat, 1, &u, &ul);
        tr_ok = (tr == 0 && u && ul == 2 &&
                 (unsigned char)u[0] == 0xC3 && (unsigned char)u[1] == 0xA9);  /* U+00E9 */
        if (u) pb_free(u);
    }

    int ok = (errc == 0 && cc >= 0 && rt_ok && tr_ok);
    printf("[PBABI]   text round-trip via dylib: %s (changeCount=%ld) ; latin1->utf8('é'): %s\n",
           rt_ok ? "ok" : "FAIL", cc, tr_ok ? "ok" : "FAIL");
    printf("[PBABI] %s errcount=%d (%d symbols), text+transcode through the dlopen boundary\n",
           ok ? "PASS" : "FAIL", errc, n);
    return ok ? 0 : 1;
}
