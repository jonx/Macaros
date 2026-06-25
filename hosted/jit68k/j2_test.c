/* j2_test.c — [J2]: one 68k basic block, translated by the ADOPTED Emu68 emitter
 * into our MAP_JIT region, verified against an INDEPENDENT from-scratch 68k
 * interpreter. (OURS, AROS-licensed; touches no Emu68 source directly.)
 *
 * Clean-room / OURS. Standalone spike for docs/features/68k-jit/spec.md [J2].
 *
 * What it proves, unattended, value-asserting (no-crash is necessary, never
 * sufficient — a silent mistranslation must not pass):
 *
 *   The 68k block   moveq #10,d0 ; moveq #7,d1 ; add.l d1,d0 ; rts   (expect d0=17)
 *   is translated to AArch64 by Emu68's own instruction encoders (the verbatim,
 *   MPL-quarantined emu68/A64.h), written into the [J1] MAP_JIT code cache, and
 *   EXECUTED under W^X. Its resulting 68k register file (d0..d7, a0..a7, ccr, pc)
 *   is asserted BIT-IDENTICAL to a tiny independent interpreter that runs the same
 *   big-endian opcode stream from scratch. PASS only if EVERY asserted register
 *   matches AND d0 == 17.
 *
 * The independence matters: the reference (j2_interp.c) does NOT use Emu68, so a
 * match validates the Emu68-emitter path against a separate semantics, not against
 * Emu68's own decode (per the [J2] requirement).
 *
 * Watchdog: a SIGALRM hard-kills the process so the spike can never hang.
 */
#include "j2_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static void watchdog(int sig)
{
    (void)sig;
    const char *m = "[J2] FAIL: watchdog timeout (translated block hung or faulted)\n";
    write(2, m, strlen(m));
    _exit(2);
}

/* Seed an identical NONZERO initial state into both engines, so the test proves
 * the block reads/writes the real state (not that a zeroed struct happens to
 * match a zeroed struct) and that ONLY the touched regs change. */
static void seed_state(struct m68k_state *st)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < 8; i++) st->d[i] = 0xD0000000u + (uint32_t)i;  /* D0..D7 sentinels */
    for (int i = 0; i < 8; i++) st->a[i] = 0xA0000000u + (uint32_t)i;  /* A0..A7 sentinels */
    st->ccr = 0x00000010u;   /* X set, NZVC clear — to prove the block recomputes them */
    st->pc  = 0x00010000u;   /* arbitrary entry PC */
}

static int diff_reg(const char *name, uint32_t jit, uint32_t ref)
{
    int bad = (jit != ref);
    printf("    %-4s jit=0x%08X  ref=0x%08X  %s\n",
           name, jit, ref, bad ? "<-- MISMATCH" : "ok");
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Never hang. */
    signal(SIGALRM, watchdog);
    alarm(10);

    printf("[J2] one 68k block via the ADOPTED Emu68 emitter, vs an independent interpreter\n");
    printf("[J2]   block: moveq #10,d0 ; moveq #7,d1 ; add.l d1,d0 ; rts   (expect d0=17=0x11)\n");

    /* Build the translated block (Emu68 emitter -> MAP_JIT region). */
    char err[128] = {0};
    jit68k_block_fn block = jit68k_build_block(err, sizeof(err));
    if (!block) {
        printf("[J2] FAIL: could not build JIT block: %s\n", err[0] ? err : "(unknown)");
        return 1;
    }
    printf("[J2]   translated to AArch64 and written into MAP_JIT region at %p\n", (void *)block);

    /* Run the reference interpreter on one copy of the seeded state. */
    struct m68k_state ref_st;
    seed_state(&ref_st);
    interp68k_run_block(&ref_st);

    /* Run the JITed block on an identically-seeded copy. */
    struct m68k_state jit_st;
    seed_state(&jit_st);
    block(&jit_st);     /* <-- executes freshly-emitted AArch64 under W^X */

    /* Diff the full architectural register file. */
    int bad = 0;
    printf("[J2]   register diff (JIT executed vs independent interpreter):\n");
    char nm[8];
    for (int i = 0; i < 8; i++) { snprintf(nm, sizeof(nm), "d%d", i); bad |= diff_reg(nm, jit_st.d[i], ref_st.d[i]); }
    for (int i = 0; i < 8; i++) { snprintf(nm, sizeof(nm), "a%d", i); bad |= diff_reg(nm, jit_st.a[i], ref_st.a[i]); }
    bad |= diff_reg("ccr", jit_st.ccr, ref_st.ccr);
    bad |= diff_reg("pc",  jit_st.pc,  ref_st.pc);

    /* The headline value assert: d0 must be 17. */
    int d0_ok = (jit_st.d[0] == 17u) && (ref_st.d[0] == 17u);
    printf("[J2]   value check: d0 == 17 ?  jit d0=%u  ref d0=%u  -> %s\n",
           jit_st.d[0], ref_st.d[0], d0_ok ? "yes" : "NO");

    jit68k_free_block();

    int ok = (!bad) && d0_ok;
    if (ok) {
        printf("[J2] PASS: Emu68-emitter block matches the independent interpreter on every "
               "register (d0..d7,a0..a7,ccr,pc) and d0=17 (0x%08X). "
               "Adopted AArch64 emitter runs hosted under MAP_JIT and computes the correct result.\n",
               jit_st.d[0]);
    } else {
        printf("[J2] FAIL: %s%s — Emu68-emitter output diverged from the independent reference.\n",
               bad ? "register mismatch " : "",
               d0_ok ? "" : "d0 != 17 ");
    }
    return ok ? 0 : 1;
}
