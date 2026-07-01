/* hostbind_sample.c -- worked template for tapping the host from AROS using the
 * <aros/hostbind.h> helper. Copy one of the two shapes for a new host bridge.
 *
 * Compiled against the crosstools + the AROS SDK; header-clean (no macOS SDK). See
 * docs/features/host-bridge/README.md for the why, and build.sh here for the exact
 * compile line.
 */
#include <proto/exec.h>
#include <aros/hostbind.h>

#include <stddef.h>

/* ---- Shape (a): borrow a function the host libc already provides -------------
 * No host-side code to write. This is how arc4random / battclock work. */
long sample_host_getpid(void)
{
    typedef long (*getpid_fn)(void);
    static getpid_fn host_getpid = NULL;
    static int tried = 0;

    if (!tried)
    {
        /* resolves from libSystem.dylib on darwin, libc.so.* on linux-hosted,
         * NULL on a native build */
        host_getpid = (getpid_fn)HostBind_LibcSym("getpid");
        tried = 1;
    }

    if (host_getpid)
        return host_getpid();
    return -1;                          /* no host: caller decides on a fallback */
}

/* ---- Shape (b): open your own host dylib and bind a function table -----------
 * Use this when the host API is complex/async/Objective-C, so you ship a small
 * libXXXhost.dylib in hosted/ (see hosted/bsdsocket, hosted/clipboard). The struct
 * fields must be in the same order as the symbol names. */
struct SampleIFace
{
    int  (*sample_open)(void);
    long (*sample_read)(void *buf, long len);
    void (*sample_close)(void);
};

static const char *sample_libs[] = { "libsamplehost.dylib", (const char *)0 };
static const char *sample_syms[] = { "sample_open", "sample_read", "sample_close",
                                     (const char *)0 };

struct SampleIFace *sample_bind_host(void)
{
    ULONG unresolved = 0;
    /* NULL if the dylib isn't found or any symbol is missing (unresolved gets the
     * count from the last attempt). The interface stays valid for the process. */
    return (struct SampleIFace *)HostBind_Interface(sample_libs, sample_syms,
                                                    &unresolved);
}
