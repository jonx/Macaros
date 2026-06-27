/* j1_test.c — [J1]: prove a JIT code cache works under Apple-Silicon W^X.
 *
 * Independent work [OURS] — no third-party implementation source was read or
 * consulted; any resemblance is coincidental. Standalone spike for
 * docs/features/68k-jit/spec.md [J1]. Pure Apple/POSIX. Builds on the reusable
 * executable-memory API in jit_region.{h,c} that [J2] (the adapted Emu68
 * emitter) and the native LoadSeg path will share.
 *
 * What it proves, unattended, value-asserting (no-crash is necessary, never
 * sufficient — a stale I-cache or a no-op toggle would crash nothing yet return
 * the wrong word):
 *
 *   [J1a] mmap a MAP_JIT region, open a pthread_jit_write_protect_np(0) window,
 *         write a hand-assembled `movz w0,#0x6804; ret`, close the window,
 *         sys_icache_invalidate, call it -> MUST return 0x6804.
 *   [J1b] CONTROL — a SECOND stub returning a DIFFERENT constant (0x1ED5) written
 *         into the SAME region and executed proves we run freshly-written code,
 *         not a cached/constant-folded result.
 *   [J1c] NEGATIVE CONTROL — a write to the MAP_JIT region WITHOUT opening the
 *         write window must be rejected (W^X). Run in a forked child so a fault
 *         there leaves the parent's verdict clean; the child's exit status tells
 *         us whether W^X actually bites on this machine.
 *
 * PASS: [J1a] and [J1b] both return their exact constants. (The negative control
 * is reported as evidence; it does not gate the marker, since "the OS happens to
 * allow the unguarded write" would be a weaker machine, not a failure of OUR
 * write path — our path always opens the window.)
 */
#include "jit_region.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

typedef uint32_t (*stub_fn)(void);

/* Emit `movz w0,#imm16; ret` at `dst` (two 32-bit AArch64 words). Caller owns the
 * write window + the i-cache invalidate. Returns bytes written. */
static size_t emit_const_stub(uint32_t *dst, uint16_t imm16)
{
    dst[0] = JIT_MOVZ_W0(imm16);   /* movz w0, #imm16  (computed from the encoding) */
    dst[1] = JIT_RET;              /* ret */
    return 2 * sizeof(uint32_t);
}

/* Write `imm16`'s stub at `slot`, finalize the cache, and call it. */
static uint32_t build_and_call(jit_region *r, uint32_t *slot, uint16_t imm16)
{
    jit_write_begin(r);                       /* R-JIT-WRITE: open window */
    size_t n = emit_const_stub(slot, imm16);
    jit_write_end(r);                          /* R-JIT-WRITE: close window */
    jit_finalize(r, slot, n);                  /* R-JIT-ICACHE */

    stub_fn fn = (stub_fn)(void *)slot;
    return fn();
}

/* Negative control, in a child: attempt to write the region WITHOUT the toggle.
 * Returns the child's outcome via exit code so the parent stays clean:
 *   0   = the unguarded write FAULTED (W^X enforced — the expected, strong case)
 *   42  = the unguarded write SUCCEEDED (W^X did not bite this attempt)
 * A signal-terminated child is reported by the parent as "faulted (signal N)". */
static void child_unguarded_write(jit_region *r)
{
    /* No pthread_jit_write_protect_np(0) here on purpose. Under W^X the MAP_JIT
     * pages are execute-only to this thread, so the store should SIGBUS. */
    volatile uint32_t *p = (volatile uint32_t *)r->base;
    p[0] = JIT_RET;          /* if W^X bites, we never return from this store */
    _exit(42);               /* reached only if the store was allowed */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[J1a] MAP_JIT exec-memory: mmap(MAP_JIT) + pthread_jit_write_protect_np + sys_icache_invalidate\n");

    jit_region r;
    if (jit_region_alloc(&r, 4096) != 0) {
        printf("[J1] FAIL: jit_region_alloc(MAP_JIT) failed (errno=%d) — entitlement/codesign?\n", errno);
        return 1;
    }
    printf("[J1a]   mmap MAP_JIT region: base=%p size=%zu\n", r.base, r.size);

    uint32_t *slotA = (uint32_t *)r.base;
    uint32_t *slotB = slotA + 8;   /* a separate slot in the same region */

    const uint16_t WANT_A = 0x6804;
    const uint16_t WANT_B = 0x1ED5;

    uint32_t gotA = build_and_call(&r, slotA, WANT_A);
    printf("[J1a]   stub returns 0x%04X (want 0x%04X)\n", gotA, WANT_A);

    /* [J1b] control: a different constant from freshly-written code in the same region. */
    uint32_t gotB = build_and_call(&r, slotB, WANT_B);
    printf("[J1b]   second stub (control) returns 0x%04X (want 0x%04X) — proves live codegen, not a cached result\n",
           gotB, WANT_B);

    /* [J1c] negative control: unguarded write should be rejected by W^X. */
    pid_t pid = fork();
    if (pid == 0) {
        child_unguarded_write(&r);
        _exit(43);   /* unreachable */
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st))
            printf("[J1c]   negative control: unguarded write FAULTED (signal %d) — W^X enforced (expected)\n",
                   WTERMSIG(st));
        else if (WIFEXITED(st) && WEXITSTATUS(st) == 42)
            printf("[J1c]   negative control: unguarded write was ALLOWED on this machine (W^X did not bite this path)\n");
        else
            printf("[J1c]   negative control: child exit status %d (inconclusive)\n", st);
    } else {
        printf("[J1c]   negative control skipped (fork failed)\n");
    }

    int ok = (gotA == WANT_A) && (gotB == WANT_B);
    jit_region_free(&r);

    if (ok)
        printf("[J1] PASS: MAP_JIT stub returned 0x%04X and control 0x%04X — W^X JIT code cache works (asserted values)\n",
               gotA, gotB);
    else
        printf("[J1] FAIL: got 0x%04X / 0x%04X, wanted 0x%04X / 0x%04X (stale I-cache or no-op toggle)\n",
               gotA, gotB, WANT_A, WANT_B);
    return ok ? 0 : 1;
}
