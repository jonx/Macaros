/* j5b_test.c — [J5b] translate a self-contained 68k LOOP (a conditional backward
 * branch + real condition codes), verify the registers, the iteration count, and
 * termination against an INDEPENDENT reference. (OURS, AROS-licensed; touches no Emu68
 * source directly.)
 *
 * Clean-room / OURS. Standalone spike for docs/features/68k-jit/spec.md [J5b].
 *
 * What it proves, unattended, value-asserting (no-crash is necessary, never
 * sufficient — a silent mistranslation, a wrong flag, or a non-terminating loop must
 * not pass):
 *
 *   The 68k loop
 *       moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ; subq.l #1,d1 ; bne.s L ; rts
 *   sums 5+4+3+2+1 = 15 into d0 over 5 iterations, then returns (d1=0). It is loaded
 *   from a REAL big-endian AmigaOS hunk binary into a 32-bit sandbox (the [J4] loader,
 *   reused), then translated to AArch64 by the ADOPTED Emu68 emitter driven by our
 *   HAND-ROLLED loop decode + real-CCR emit (j5b_build.c), as a SINGLE jit_region with
 *   an INTERNAL backward branch (b.ne to the loop top), and RUN under W^X.
 *
 *   VALUE ASSERTS (PASS iff ALL hold):
 *     (a) the JITed register file (d0..d7) == the INDEPENDENT interpreter's
 *         (j5b_interp.c), field by field — in particular d0 == 15, d1 == 0;
 *     (b) the JITed CCR == the interpreter's full N/Z/V/C/X (real condition codes) —
 *         after the final subq.l #1 leaves d1==0, the 68k Z flag must be SET;
 *     (c) the loop ran EXACTLY 5 iterations and TERMINATED (the reference counts the
 *         number of loop-body passes = bne executions: a 5-trip loop reaches the bne
 *         5 times, taken 4 times then falling through; the JIT terminating at all is
 *         proven by the watchdog not firing and d0/d1 matching the 5-iteration result).
 *
 *   NEGATIVE CONTROL (must make the asserts BITE):
 *     break the branch condition — emit the backward branch as ALWAYS-taken (wrong
 *     condition / broken Z test). The loop then never terminates; run it in a forked
 *     CHILD with its OWN short alarm, and assert the child is KILLED by the alarm
 *     (i.e. it hung), proving the termination path genuinely depends on the real Z and
 *     that a broken branch is caught. (The parent's main watchdog is untouched.)
 *
 * Watchdog: a SIGALRM hard-kills the (main) process so the spike can never hang.
 */
#include "j4_hunk.h"        /* reuse the [J4] loader/relocator + sandbox (shared) */
#include "j5b_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static void watchdog(int sig)
{
    (void)sig;
    const char *m = "[J5b] FAIL: watchdog timeout (translate/run hung or faulted)\n";
    write(2, m, strlen(m));
    _exit(2);
}

/* ---- Hand-assemble the REAL hunk binary, big-endian -----------------------------
 * One CODE hunk holding the loop. Byte offsets within the hunk:
 *   0:  moveq #0,d0    = 0x7000
 *   2:  moveq #5,d1    = 0x7205   (0111 ddd=001 0 imm=0x05)
 *   4:  add.l  d1,d0   = 0xD081   (1101 ddd=000 010 000 mmm=001)   <-- L (loop top)
 *   6:  subq.l #1,d1   = 0x5381   (0101 q=001 1 sz=10 mode=000 reg=001)
 *   8:  bne.s  L       = 0x66FA   (0110 0110 disp8) ; disp = 4 - (8+2) = -6 = 0xFA
 *  10:  rts            = 0x4E75
 * 6 words = 12 bytes = 3 longwords.
 */
#define W_MOVEQ_D0   0x7000u
#define W_MOVEQ_D1   0x7205u
#define W_ADD        0xD081u
#define W_SUBQ       0x5381u
#define W_BNE        0x66FAu     /* bne.s L (disp = -6) */
#define W_RTS        0x4E75u

#define EXPECT_D0    15u         /* 5+4+3+2+1 */
#define EXPECT_D1    0u
#define EXPECT_ITERS 5u
/* After subq.l #1,d1 leaves d1 == 0: Z set, N clear, V clear, C clear, X clear. */
#define EXPECT_CCR   (J5B_CCR_Z)

static void put_be32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24); (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >>  8); (*p)[3] = (uint8_t)(v      );
    *p += 4;
}

static size_t build_hunk_binary(uint8_t *out)
{
    uint8_t *p = out;
    /* HUNK_HEADER */
    put_be32(&p, J4_HUNK_HEADER);
    put_be32(&p, 0);            /* empty name list */
    put_be32(&p, 1);            /* numhunks = 1 (0=CODE) */
    put_be32(&p, 0);            /* first */
    put_be32(&p, 0);            /* last  */
    put_be32(&p, 3);            /* hunk0 size = 3 longs (6 opcode words) */
    /* HUNK_CODE (hunk 0): the loop, 3 longwords */
    put_be32(&p, J4_HUNK_CODE);
    put_be32(&p, 3);            /* length = 3 longs */
    put_be32(&p, (W_MOVEQ_D0 << 16) | W_MOVEQ_D1);   /* 0x7000 0x7205 */
    put_be32(&p, (W_ADD      << 16) | W_SUBQ);       /* 0xD081 0x5381 */
    put_be32(&p, (W_BNE      << 16) | W_RTS);        /* 0x66FA 0x4E75 */
    /* HUNK_END */
    put_be32(&p, J4_HUNK_END);
    return (size_t)(p - out);
}

/* A second, tiny block that exercises the FULL real-CCR derivation (not just Z): a
 * BORROWING subtract.  moveq #0,d0 ; subq.l #1,d0 ; rts  ->  d0 = 0 - 1 = 0xFFFFFFFF.
 * 68k subtract flags: N=1 (bit31 set), Z=0, V=0 (no signed overflow of 0-1),
 * C=1 (borrow: src 1 > dst 0, unsigned), X:=C=1.  CCR = N|C|X = 0x08|0x01|0x10 = 0x19.
 * This proves the emitted CCR-from-subs computes N, C, X (and the 68k-C = AArch64
 * carry-CLEAR borrow inversion) correctly, cross-checked JIT-vs-interpreter — the loop
 * above only ever ends with Z set, which would hide a bug in the other bits. */
#define W_SUBQ_D0    0x5380u     /* subq.l #1,d0 (0101 q=001 1 sz=10 mode=000 reg=000) */
#define CCR_BORROW   (J5B_CCR_N | J5B_CCR_C | J5B_CCR_X)   /* 0x19 */

static size_t build_borrow_binary(uint8_t *out)
{
    uint8_t *p = out;
    put_be32(&p, J4_HUNK_HEADER);
    put_be32(&p, 0); put_be32(&p, 1); put_be32(&p, 0); put_be32(&p, 0);
    put_be32(&p, 2);            /* hunk0 size = 2 longs (3 opcode words, padded) */
    put_be32(&p, J4_HUNK_CODE);
    put_be32(&p, 2);            /* length = 2 longs */
    put_be32(&p, (W_MOVEQ_D0 << 16) | W_SUBQ_D0);    /* 0x7000 0x5380 */
    put_be32(&p, (W_RTS      << 16) | 0x0000u);      /* 0x4E75 (pad) */
    put_be32(&p, J4_HUNK_END);
    return (size_t)(p - out);
}

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00010000u     /* 64 KiB */

/* Compare two register files; print the first mismatch. Returns 1 if equal. */
static int regs_equal(const struct j5b_m68k_state *a, const struct j5b_m68k_state *b)
{
    int ok = 1;
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) {
        printf("[J5b]     D%d mismatch: jit=0x%08X ref=0x%08X\n", i, a->d[i], b->d[i]); ok = 0;
    }
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) {
        printf("[J5b]     A%d mismatch: jit=0x%08X ref=0x%08X\n", i, a->a[i], b->a[i]); ok = 0;
    }
    return ok;
}

/* Run the negative control (always-taken branch -> infinite loop) in a forked child
 * with a short SIGALRM. Returns 1 if the child HUNG (was killed by the alarm), 0 if it
 * returned on its own (which would mean the broken branch didn't actually loop). */
static int run_neg_control(const uint8_t *bin, size_t binlen)
{
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* CHILD: emit the loop with the branch condition BROKEN (always taken). With a
         * real bne this terminates in 5 iterations; broken, it never terminates. A 2s
         * alarm with the DEFAULT handler kills the child if it hangs. */
        alarm(2);
        signal(SIGALRM, SIG_DFL);

        uint8_t *mem = malloc(SANDBOX_SIZE);
        j4_sandbox sb; j4_seglist seg; char err[200] = {0};
        if (!mem) _exit(3);
        j4_sandbox_init(&sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
        if (j4_load_hunks(&sb, bin, binlen, 0, &seg, err, sizeof(err)) != 0) _exit(3);

        struct j5b_m68k_state st; memset(&st, 0, sizeof(st));
        const uint8_t *code = j4_sandbox_host(&sb, seg.entry);
        /* neg_break_branch=1 -> always-taken backward branch -> infinite loop. */
        j5b_run_block(seg.entry, code, seg.hunk_size[0], &st, /*neg_break_branch=*/1,
                      err, sizeof(err));
        j5b_run_free();
        _exit(0);                /* if we get HERE, the broken branch did NOT hang */
    }
    /* PARENT: wait and report how the child died. */
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM)
        return 1;                /* killed by its own alarm -> it hung, as expected */
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(10);

    printf("[J5b] translate a self-contained 68k LOOP (conditional backward branch + real CCR), "
           "verify registers/iterations/termination vs an independent reference\n");
    printf("[J5b]   loop: moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ; subq.l #1,d1 ; bne.s L ; rts  "
           "(-> d0=15 over 5 iterations, d1=0, Z set)\n");

    uint8_t bin[256];
    size_t  binlen = build_hunk_binary(bin);
    char    err[200] = {0};

    /* ---------- LOAD the binary into a sandbox ---------- */
    uint8_t *mem = malloc(SANDBOX_SIZE);
    if (!mem) { printf("[J5b] FAIL: malloc sandbox failed\n"); return 1; }
    j4_sandbox sb; j4_seglist seg;
    j4_sandbox_init(&sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    if (j4_load_hunks(&sb, bin, binlen, /*skip_reloc=*/0, &seg, err, sizeof(err)) != 0) {
        printf("[J5b] FAIL: load error: %s\n", err);
        return 1;
    }
    const uint8_t *code = j4_sandbox_host(&sb, seg.entry);
    uint32_t code_len = seg.hunk_size[0];
    printf("[J5b]   loaded: CODE @ 0x%08X (%u B), entry PC 0x%08X\n",
           seg.hunk_base[0], code_len, seg.entry);

    /* ================= MAIN CASE ================= */
    struct j5b_m68k_state st_jit, st_ref;
    memset(&st_jit, 0, sizeof(st_jit));
    memset(&st_ref, 0, sizeof(st_ref));

    /* JIT path (single region, real backward b.ne). */
    if (j5b_run_block(seg.entry, code, code_len, &st_jit,
                      /*neg_break_branch=*/0, err, sizeof(err)) != 0) {
        printf("[J5b] FAIL: run error: %s\n", err);
        return 1;
    }
    j5b_run_free();

    /* INDEPENDENT reference (counts iterations + termination). */
    uint32_t ref_iters = 0; int ref_terminated = 0;
    j5b_interp_run_block(seg.entry, code, code_len, &st_ref, &ref_iters, &ref_terminated);

    int regs_ok = regs_equal(&st_jit, &st_ref);
    int ccr_ok  = (st_jit.ccr == st_ref.ccr);
    int val_ok  = (st_jit.d[0] == EXPECT_D0) && (st_jit.d[1] == EXPECT_D1) &&
                  (st_jit.ccr == EXPECT_CCR);
    int loop_ok = (ref_iters == EXPECT_ITERS) && ref_terminated;

    printf("[J5b]   JIT: d0=%u (0x%08X) d1=%u (0x%08X) ccr=0x%02X\n",
           st_jit.d[0], st_jit.d[0], st_jit.d[1], st_jit.d[1], st_jit.ccr);
    printf("[J5b]   REF: d0=%u (0x%08X) d1=%u (0x%08X) ccr=0x%02X ; iterations=%u terminated=%d\n",
           st_ref.d[0], st_ref.d[0], st_ref.d[1], st_ref.d[1], st_ref.ccr,
           ref_iters, ref_terminated);
    printf("[J5b]   ASSERT registers JIT==REF -> %s ; CCR JIT==REF (real N/Z/V/C/X) -> %s ; "
           "d0==15 && d1==0 && Z-set -> %s ; loop ran 5 iters && terminated -> %s\n",
           regs_ok ? "MATCH" : "MISMATCH", ccr_ok ? "MATCH" : "MISMATCH",
           val_ok ? "MATCH" : "MISMATCH", loop_ok ? "MATCH" : "MISMATCH");

    /* ========== FULL-CCR CROSS-CHECK: a borrowing subtract (N,C,X set) ==========
     * The loop above always ends with Z set; this proves the real-CCR emit also gets
     * N, C (the 68k borrow = AArch64 carry-CLEAR inversion) and X right. */
    int borrow_ok = 0;
    {
        uint8_t binb[256]; size_t binblen = build_borrow_binary(binb);
        uint8_t *memb = malloc(SANDBOX_SIZE);
        j4_sandbox sbb; j4_seglist segb;
        struct j5b_m68k_state bjit, bref;
        if (memb) {
            j4_sandbox_init(&sbb, memb, SANDBOX_ORIGIN, SANDBOX_SIZE);
            if (j4_load_hunks(&sbb, binb, binblen, 0, &segb, err, sizeof(err)) == 0) {
                const uint8_t *bcode = j4_sandbox_host(&sbb, segb.entry);
                memset(&bjit, 0, sizeof(bjit));
                memset(&bref, 0, sizeof(bref));
                if (j5b_run_block(segb.entry, bcode, segb.hunk_size[0], &bjit, 0,
                                  err, sizeof(err)) == 0) {
                    j5b_run_free();
                    j5b_interp_run_block(segb.entry, bcode, segb.hunk_size[0], &bref, NULL, NULL);
                    borrow_ok = (bjit.d[0] == 0xFFFFFFFFu) && (bjit.ccr == CCR_BORROW) &&
                                (bjit.ccr == bref.ccr) && (bjit.d[0] == bref.d[0]);
                    printf("[J5b]   FULL-CCR (0-1 borrow): JIT d0=0x%08X ccr=0x%02X ; REF d0=0x%08X "
                           "ccr=0x%02X ; expect ccr=0x%02X (N|C|X) -> %s\n",
                           bjit.d[0], bjit.ccr, bref.d[0], bref.ccr, CCR_BORROW,
                           borrow_ok ? "MATCH" : "MISMATCH");
                }
            }
        }
        free(memb);
    }

    /* ================= NEGATIVE CONTROL: break the branch condition ================= */
    int ctl_ok = run_neg_control(bin, binlen);
    printf("[J5b]   NEG CONTROL (branch always-taken -> no termination): child %s -> %s\n",
           ctl_ok ? "HUNG (killed by its own 2s alarm)" : "returned on its own",
           ctl_ok ? "correctly NON-TERMINATING (termination assert bites)" : "unexpected");

    free(mem);

    int ok = regs_ok && ccr_ok && val_ok && loop_ok && borrow_ok && ctl_ok;
    if (ok) {
        printf("[J5b] PASS: a self-contained 68k loop with a conditional backward branch + real "
               "condition codes translated by the adopted Emu68 emitter (single jit_region, internal "
               "b.ne reading the live NZCV from subs) is byte-exact vs an independent reference — "
               "d0=%u over %u iterations, d1=%u, CCR=0x%02X (Z set); the broken-branch negative "
               "control fails to terminate (caught), proving the asserts bite.\n",
               st_jit.d[0], ref_iters, st_jit.d[1], st_jit.ccr);
    } else {
        printf("[J5b] FAIL: %s%s%s%s%s%s\n",
               regs_ok   ? "" : "register mismatch ",
               ccr_ok    ? "" : "CCR mismatch ",
               val_ok    ? "" : "value/Z assert ",
               loop_ok   ? "" : "iteration/termination assert ",
               borrow_ok ? "" : "full-CCR borrow cross-check ",
               ctl_ok    ? "" : "negative control did not bite ");
    }
    return ok ? 0 : 1;
}
