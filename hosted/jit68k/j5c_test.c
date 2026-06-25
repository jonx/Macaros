/* j5c_test.c — [J5c] value-asserting driver: translate a richer register/control block
 * THROUGH THE REAL EMU68 DECODERS, run it under W^X, and assert the result is byte-exact
 * equal to an INDEPENDENT from-scratch interpreter (OURS). Plus negative controls + a
 * watchdog. Prints `[J5c] PASS` only if every assert holds.
 *
 * OURS, AROS-licensed. The re-hosting VERDICT is summarised at the end of the output.
 */
#include "j5c_jit68k.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* ---- assemble the richer 68k block (big-endian) into a buffer ---------------------- */
static unsigned put_be16(uint8_t *p, unsigned i, uint16_t v) { p[i] = v >> 8; p[i+1] = v & 0xff; return i + 2; }

/* opcode encoders (register-direct), m68k bit layout */
static uint16_t OP_MOVEQ(unsigned dn, int imm)   { return (uint16_t)(0x7000u | (dn << 9) | (imm & 0xFF)); }
static uint16_t OP_ADD_L(unsigned dm, unsigned dn){ return (uint16_t)(0xD080u | (dn << 9) | dm); } /* Dn += Dm */
static uint16_t OP_SUB_L(unsigned dm, unsigned dn){ return (uint16_t)(0x9080u | (dn << 9) | dm); } /* Dn -= Dm */
static uint16_t OP_AND_L(unsigned dm, unsigned dn){ return (uint16_t)(0xC080u | (dn << 9) | dm); } /* Dn &= Dm */
static uint16_t OP_OR_L (unsigned dm, unsigned dn){ return (uint16_t)(0x8080u | (dn << 9) | dm); } /* Dn |= Dm */
static uint16_t OP_EOR_L(unsigned dn, unsigned dm){ return (uint16_t)(0xB180u | (dn << 9) | dm); } /* Dm ^= Dn */
static uint16_t OP_CMP_L(unsigned dm, unsigned dn){ return (uint16_t)(0xB080u | (dn << 9) | dm); } /* flags Dn-Dm */
static uint16_t OP_MULS_W(unsigned dm, unsigned dn){return (uint16_t)(0xC1C0u | (dn << 9) | dm); } /* Dn=(i16)Dn*(i16)Dm */
#define OP_RTS 0x4E75u

static unsigned build_block(uint8_t *b)
{
    unsigned n = 0;
    n = put_be16(b, n, OP_MOVEQ(2, -5));     /* moveq #-5,d2     d2 = 0xFFFFFFFB (sign-ext!) */
    n = put_be16(b, n, OP_ADD_L(2, 0));      /* add.l  d2,d0     d0 += d2                     */
    n = put_be16(b, n, OP_SUB_L(3, 0));      /* sub.l  d3,d0     d0 -= d3                     */
    n = put_be16(b, n, OP_AND_L(4, 0));      /* and.l  d4,d0     d0 &= d4                     */
    n = put_be16(b, n, OP_OR_L (5, 0));      /* or.l   d5,d0     d0 |= d5                     */
    n = put_be16(b, n, OP_EOR_L(6, 1));      /* eor.l  d6,d1     d1 ^= d6                     */
    n = put_be16(b, n, OP_MULS_W(7, 1));     /* muls.w d7,d1     d1 = (i16)d1 * (i16)d7       */
    n = put_be16(b, n, OP_CMP_L(1, 0));      /* cmp.l  d1,d0     flags of d0 - d1 (coverage)  */
    n = put_be16(b, n, OP_AND_L(2, 2));      /* and.l  d2,d2     d2&=d2 (==d2) -> N=1 (bit31), */
                                             /*                  Z=0: a NON-TRIVIAL final CCR  */
    n = put_be16(b, n, OP_RTS);
    return n;
}

static void seed_state(struct j5c_m68k_state *s)
{
    memset(s, 0, sizeof(*s));
    /* Nonzero, distinct seeds so a zeroed-vs-zeroed accident cannot pass. */
    s->d[0] = 0x00001000u;
    s->d[1] = 0x00000007u;   /* low 16 = 7 (muls operand)                 */
    s->d[2] = 0xAAAAAAAAu;   /* overwritten by moveq                       */
    s->d[3] = 0x00000010u;
    s->d[4] = 0x0000FFFFu;
    s->d[5] = 0x12000000u;
    s->d[6] = 0x0000000Au;
    s->d[7] = 0x00000003u;   /* low 16 = 3 (muls operand)                 */
    for (int i = 0; i < 8; i++) s->a[i] = 0x00210000u + (uint32_t)(i * 4);
    s->ccr = 0;
}

static int eq_state_regs(const struct j5c_m68k_state *a, const struct j5c_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

static volatile sig_atomic_t g_alarmed = 0;
static void on_alarm(int sig) { (void)sig; g_alarmed = 1; const char *m = "[J5c] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(10);   /* watchdog: any hang/fault -> FAIL */

    printf("[J5c] re-hosting Emu68's REAL decoder + register allocator (hosted)\n");

    uint8_t code[64];
    unsigned code_len = build_block(code);

    /* The sandbox just needs to host the 68k stream so cache_read_16 (HOOK 2) reads it. */
    j5c_sandbox sb = { .host_mem = code, .origin = 0x00210000u, .size = sizeof(code) };
    uint32_t entry_pc = 0x00210000u;

    /* ---- JIT path: drive the REAL Emu68 decoders ---- */
    struct j5c_m68k_state jit_st;  seed_state(&jit_st);
    char err[160] = {0};
    int rc = j5c_run_block(&sb, entry_pc, code, code_len, &jit_st, 0, err, sizeof(err));
    if (rc != 0) { printf("[J5c] FAIL (run_block error: %s)\n", err); return 1; }

    /* ---- reference path: the independent interpreter ---- */
    struct j5c_m68k_state ref_st;  seed_state(&ref_st);
    j5c_interp_run_block(entry_pc, code, code_len, &ref_st);

    printf("  decoders driven : EMIT_moveq, EMIT_lineD(add), EMIT_line9(sub), "
           "EMIT_lineC(and,muls), EMIT_line8(or), EMIT_lineB(eor,cmp)\n");
    printf("  JIT  d0..d7 : %08x %08x %08x %08x %08x %08x %08x %08x\n",
           jit_st.d[0], jit_st.d[1], jit_st.d[2], jit_st.d[3],
           jit_st.d[4], jit_st.d[5], jit_st.d[6], jit_st.d[7]);
    printf("  REF  d0..d7 : %08x %08x %08x %08x %08x %08x %08x %08x\n",
           ref_st.d[0], ref_st.d[1], ref_st.d[2], ref_st.d[3],
           ref_st.d[4], ref_st.d[5], ref_st.d[6], ref_st.d[7]);
    printf("  JIT ccr=%02x  REF ccr=%02x  (Z/N bits asserted; full byte shown)\n",
           jit_st.ccr & 0xFF, ref_st.ccr & 0xFF);

    int ok = 1;

    /* PRIMARY assert: architectural registers byte-exact vs the independent interpreter. */
    if (!eq_state_regs(&jit_st, &ref_st)) { printf("  MISMATCH: D/A registers diverge\n"); ok = 0; }

    /* Spot-check the well-known computed value: d2 = moveq #-5 sign-extended. */
    if (jit_st.d[2] != 0xFFFFFFFBu) { printf("  MISMATCH: moveq #-5 sign-extend (d2=%08x, want FFFFFFFB)\n", jit_st.d[2]); ok = 0; }

    /* CCR: assert the layout-stable Z (bit2) and N (bit3) match the reference. The block
     * ends on `and.l d2,d2` with d2=0xFFFFFFFB, so the EXPECTED final CCR has N=1, Z=0 —
     * a NON-TRIVIAL value (a CCR stuck at 0 would fail this, so the flag path is real). */
    uint32_t jzn = jit_st.ccr & (J5C_CCR_Z | J5C_CCR_N);
    uint32_t rzn = ref_st.ccr & (J5C_CCR_Z | J5C_CCR_N);
    if (jzn != rzn) { printf("  MISMATCH: CCR Z/N (jit=%02x ref=%02x)\n", jzn, rzn); ok = 0; }
    if (rzn != J5C_CCR_N) { printf("  REF CCR Z/N unexpected (want N set, Z clear; got %02x)\n", rzn); ok = 0; }
    if (jzn != J5C_CCR_N) { printf("  MISMATCH: JIT final CCR N not set (got %02x) — flag path wrong\n", jzn); ok = 0; }

    /* ---- NEGATIVE CONTROL: corrupt the decoded opcode stream; JIT must diverge ---- */
    struct j5c_m68k_state neg_st; seed_state(&neg_st);
    char nerr[160] = {0};
    int nrc = j5c_run_block(&sb, entry_pc, code, code_len, &neg_st, 1 /*corrupt*/, nerr, sizeof(nerr));
    /* The control must (a) run successfully — proving the divergence is a real wrong
     * VALUE, not a crash/error — and (b) produce registers that differ from the
     * reference. (Corrupting moveq #-5 -> #-7 cascades a different d2 through d0.) */
    if (nrc != 0) {
        printf("  NEG-CONTROL inconclusive: corrupt run errored (%s) rather than mis-valuing\n", nerr);
        ok = 0;
    } else if (eq_state_regs(&neg_st, &ref_st)) {
        printf("  NEG-CONTROL FAILED TO BITE: corrupt decode still matched reference\n");
        ok = 0;
    } else {
        printf("  neg-control (corrupt decode) bit: d0 jit=%08x ref=%08x diverged (good)\n",
               neg_st.d[0], ref_st.d[0]);
    }

    j5c_run_free();

    if (!ok) { printf("[J5c] FAIL\n"); return 1; }

    printf("  VERDICT: re-hosting Emu68's REAL decoders + RA WORKS for the register/ALU/\n");
    printf("           control opcodes (no 68k-memory EA). Broad coverage of THIS class =\n");
    printf("           vendor more M68k_LINE*.c + extend the interpreter oracle. The\n");
    printf("           memory-EA modes ((An)/(An)+/-(An)/d16(An)/abs) remain blocked by the\n");
    printf("           EA emit's no-sandbox-base + big-endian-CPU assumption (edit M68k_EA.c).\n");
    printf("[J5c] PASS\n");
    return 0;
}
