/* aros_rt_glue.c — the flat C boundary between aros-rt (Rust, no_std) and AROS.
 *
 * Compiled by the AROS crosstools cc (where <proto/exec.h>/<proto/dos.h> are real
 * and SysBase/DOSBase come from the C startup). aros-rt imports exactly these four
 * symbols and nothing else AROS-specific — Rust never sees the library-base
 * register convention or any AROS header. This mirrors the "flat C ABI surface"
 * the host shims expose (hostcpu_shim.h, the bsdsock verbs); the difference is the
 * side: those forward to macOS frameworks, this forwards to AROS exec/dos.
 *
 * Header-clean on purpose: it pulls only AROS proto headers + the compiler's own
 * <stddef.h>, never host libc (<stdlib.h>/<stdio.h>). On the darwin-aarch64 backend
 * those drag in raw macOS SDK headers with a broken type chain (UPSTREAM-NOTES
 * #9/#16), so abort() is declared directly rather than via <stdlib.h>.
 *
 * Independent work: written from the AROS exec.library AllocVec/FreeVec and
 * dos.library PutStr autodocs [PUB] and this project's flat-C shim shape [OURS];
 * no third-party implementation source was read or consulted.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <stddef.h>        /* size_t — clang builtin header, no host SDK chain */

extern void abort(void);   /* posixc; declared here to avoid <stdlib.h> (see above) */

/* Rust's #[global_allocator] calls these. AllocVec records the size, so FreeVec
   needs no length. MEMF_ANY lets exec pick any memory; the allocator does its own
   alignment over the top (see lib.rs), so MEM_BLOCKSIZE alignment here suffices. */
void *aros_exec_allocvec(size_t size)
{
    return AllocVec(size, MEMF_ANY);
}

void aros_exec_freevec(void *p)
{
    FreeVec(p);
}

/* Rust selftest output + #[panic_handler] route through these. */
void aros_rt_puts(const char *s)
{
    PutStr(s);          /* dos.library: write a NUL-terminated string to Output() */
}

void aros_rt_abort(void)
{
    abort();            /* never returns. panic = "abort", no unwinding. */
}
