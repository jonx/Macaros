/* j5b_interp.c — [J5b] INDEPENDENT reference 68k interpreter, extended for subq.l +
 * a real bne.s backward branch (OURS, AROS-licensed).
 *
 * Clean-room / OURS. This is the verification ORACLE for [J5b]. Written FROM SCRATCH
 * from the Motorola 68000 ISA only; it touches NO Emu68 source, no Emu68 type/macro,
 * and does NOT decode through Emu68 — so when j5b_test.c asserts the JITed register
 * file AND the loop iteration count (produced by the adopted Emu68 EMITTER, driven by
 * our hand-rolled loop decode) equal this interpreter's, the comparison is against a
 * TRULY INDEPENDENT reference.
 *
 * Opcode subset (the [J5b] loop):
 *   moveq  #imm8,Dn        (0111 ddd 0 iiiiiiii)              sign-extend imm8
 *   add.l  Dm,Dn           (1101 ddd 010 000 mmm)             register-direct .L add
 *   subq.l #imm,Dn         (0101 qqq 1 10 000 ddd)            imm in {1..8}, q=0 => 8
 *   bne.s  disp8           (0110 0110 dddddddd)               Z==0 -> PC+2+disp8
 *   rts                    (0x4E75)                           ends the block
 *
 * Control flow is REAL: bne.s follows the signed 8-bit displacement (the loop's
 * backward branch). The interpreter counts LOOP ITERATIONS as the number of times the
 * bne.s instruction is EXECUTED (= the number of passes through the loop body): for an
 * N-trip loop the branch is reached N times — taken N-1 times, then falling through on
 * the final pass. (Counting "branch taken" would undercount by one, mistaking the
 * 5-trip loop for 4 iterations.) `*terminated` reports whether the block hit rts within
 * a step cap.
 */
#include "j5b_jit68k.h"

#include <stdint.h>

static uint16_t fetch_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* moveq flag rule (PRM): N,Z from result; V,C cleared; X untouched. */
static void set_nz_clr_vc(struct j5b_m68k_state *st, uint32_t res)
{
    uint32_t ccr = st->ccr & ~(J5B_CCR_N | J5B_CCR_Z | J5B_CCR_V | J5B_CCR_C);
    if (res & 0x80000000u) ccr |= J5B_CCR_N;
    if (res == 0)          ccr |= J5B_CCR_Z;
    st->ccr = ccr;
}

/* ADD.L flag rule (PRM): N,Z from result; C=carry-out; V=signed overflow; X:=C. */
static void set_add_flags(struct j5b_m68k_state *st, uint32_t dst, uint32_t src,
                          uint64_t sum, uint32_t res)
{
    uint32_t ccr = st->ccr & ~(J5B_CCR_X | J5B_CCR_N | J5B_CCR_Z | J5B_CCR_V | J5B_CCR_C);
    if (res & 0x80000000u)                            ccr |= J5B_CCR_N;
    if (res == 0)                                     ccr |= J5B_CCR_Z;
    if (sum >> 32)                                    ccr |= (J5B_CCR_C | J5B_CCR_X);
    if (((~(dst ^ src)) & (dst ^ res)) & 0x80000000u) ccr |= J5B_CCR_V;
    st->ccr = ccr;
}

/* SUB/SUBQ.L flag rule (PRM, result = dst - src):
 *   N = result bit31 ; Z = (result == 0) ;
 *   C = borrow = (unsigned) src > dst ; X := C ;
 *   V = signed overflow of dst - src. */
static void set_sub_flags(struct j5b_m68k_state *st, uint32_t dst, uint32_t src,
                          uint32_t res)
{
    uint32_t ccr = st->ccr & ~(J5B_CCR_X | J5B_CCR_N | J5B_CCR_Z | J5B_CCR_V | J5B_CCR_C);
    if (res & 0x80000000u)                          ccr |= J5B_CCR_N;
    if (res == 0)                                    ccr |= J5B_CCR_Z;
    if (src > dst)                                   ccr |= (J5B_CCR_C | J5B_CCR_X);
    if (((dst ^ src) & (dst ^ res)) & 0x80000000u)  ccr |= J5B_CCR_V;
    st->ccr = ccr;
}

/* A generous instruction-step cap so a (hypothetical) mis-decode in the reference
 * itself cannot hang the test; the real loop runs 5 iterations (~25 steps). */
#define STEP_CAP 100000u

void j5b_interp_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                          struct j5b_m68k_state *st, uint32_t *iters, int *terminated)
{
    uint32_t ip = 0;                 /* byte offset into the code stream */
    uint32_t passes = 0;             /* count of bne executions = loop iterations */
    uint32_t steps = 0;
    int done = 0;
    st->pc = entry_pc;

    while (ip + 2 <= code_len) {
        if (++steps > STEP_CAP) break;          /* reference watchdog (never reached) */

        uint16_t op = fetch_be16(code + ip);
        uint32_t op_ip = ip;                    /* ip of this opcode word */
        ip += 2;

        /* moveq #imm8,Dn — sign-extend the 8-bit immediate. */
        if ((op & 0xF100u) == 0x7000u) {
            unsigned dn  = (op >> 9) & 7u;
            int32_t  imm = (int8_t)(op & 0xFFu);
            st->d[dn] = (uint32_t)imm;
            set_nz_clr_vc(st, (uint32_t)imm);
            st->pc += 2;
            continue;
        }

        /* subq.l #imm,Dn : 0101 qqq 1 10 000 ddd. q==0 means immediate 8. */
        if ((op & 0xF1F8u) == 0x5180u) {
            unsigned q   = (op >> 9) & 7u;
            unsigned imm = (q == 0) ? 8u : q;
            unsigned dn  = op & 7u;
            uint32_t dst = st->d[dn];
            uint32_t res = dst - imm;
            st->d[dn] = res;
            set_sub_flags(st, dst, imm, res);
            st->pc += 2;
            continue;
        }

        /* add.l Dm,Dn : 1101 ddd 010 000 mmm (register-direct .L). */
        if ((op & 0xF1F8u) == 0xD080u) {
            unsigned dn   = (op >> 9) & 7u;
            unsigned dm   = op & 7u;
            uint32_t src  = st->d[dm];
            uint32_t dst  = st->d[dn];
            uint64_t sum  = (uint64_t)dst + (uint64_t)src;
            uint32_t res  = (uint32_t)sum;
            st->d[dn] = res;
            set_add_flags(st, dst, src, sum, res);
            st->pc += 2;
            continue;
        }

        /* bne.s disp8 : 0110 0110 dddddddd. Branch if Z==0; target = (op_ip+2)+disp8.
         * (disp8 != 0 here — the .W/.L extended forms are not in the [J5b] subset.)
         * Every execution of this instruction is one pass through the loop body. */
        if ((op & 0xFF00u) == 0x6600u) {
            int32_t disp = (int8_t)(op & 0xFFu);
            passes++;                           /* one loop-body pass completed */
            if (!(st->ccr & J5B_CCR_Z)) {       /* Z==0 -> taken */
                int64_t tgt = (int64_t)op_ip + 2 + disp;
                if (tgt < 0 || (uint64_t)tgt >= code_len) break;  /* out of block */
                ip = (uint32_t)tgt;
                st->pc = entry_pc + ip;
            } else {
                st->pc += 2;                    /* fall through */
            }
            continue;
        }

        if (op == 0x4E75u) { done = 1; break; }  /* rts — end of block */

        break;                                   /* out-of-subset opcode: stop */
    }

    if (iters)      *iters = passes;
    if (terminated) *terminated = done;
}
