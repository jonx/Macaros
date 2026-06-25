/* j2_interp.c — [J2] INDEPENDENT reference 68k interpreter (OURS, AROS-licensed).
 *
 * Clean-room / OURS. This is the verification ORACLE for [J2]. It is written FROM
 * SCRATCH from the Motorola 68000 ISA only; it touches NO Emu68 source, no Emu68
 * type/macro, and crucially does NOT decode through Emu68 — so when j2_test.c
 * asserts that the JITed register file (produced by the adopted Emu68 emitter)
 * equals this interpreter's, the comparison is against a TRULY INDEPENDENT
 * reference, per the [J2] requirement "do NOT verify Emu68's output against
 * Emu68's own decode".
 *
 * It is a tiny stepping interpreter for exactly the opcodes the [J2] block uses:
 *   moveq #imm,Dn     (0111 ddd 0 iiiiiiii)
 *   add.l  Dm,Dn      (1101 ddd 1 10 000 mmm)   ; ADD <ea=Dm>,Dn  (.L, ea->Dn)
 *   rts               (0x4E75)                  ; ends the block
 *
 * It reads the SAME big-endian 68k opcode stream the JIT builder hand-decoded, so
 * the two are driven by the identical bytes — the diff is then a real cross-check
 * of semantics, not a tautology. Anything outside this opcode subset stops the
 * interpreter (the spike's block stays in-subset by construction).
 */
#include "j2_jit68k.h"

#include <stdint.h>

/* The fixed 68k basic block, as a big-endian opcode-word stream. These are the
 * canonical 68k encodings (Motorola M68000 PRM), independent of any emulator:
 *   moveq #10,d0 = 0x700A
 *   moveq #7,d1  = 0x7207
 *   add.l  d1,d0 = 0xD081   (1101 000 010 000 001 : Dn=000(d0), opmode=010(.L,
 *                            <ea>+Dn->Dn), ea-mode=000 reg=001 -> D1)
 *   rts          = 0x4E75
 * Kept as a 16-bit big-endian stream so the interpreter does real fetch+decode. */
static const uint8_t k_block_be[] = {
    0x70, 0x0A,   /* moveq #10,d0 */
    0x72, 0x07,   /* moveq #7,d1  */
    0xD0, 0x81,   /* add.l d1,d0  */
    0x4E, 0x75,   /* rts          */
};

/* Big-endian 16-bit fetch from the 68k stream. */
static uint16_t fetch_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Set N,Z for a 32-bit result; clear V,C (the moveq flag rule). X untouched. */
static void set_nz_clr_vc(struct m68k_state *st, uint32_t res)
{
    uint32_t ccr = st->ccr & ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (res & 0x80000000u) ccr |= CCR_N;
    if (res == 0)          ccr |= CCR_Z;
    st->ccr = ccr;
}

void interp68k_run_block(struct m68k_state *st)
{
    unsigned ip = 0;                 /* index into k_block_be */
    const unsigned len = (unsigned)sizeof(k_block_be);

    while (ip + 2 <= len) {
        uint16_t op = fetch_be16(&k_block_be[ip]);
        ip += 2;

        if ((op & 0xF100u) == 0x7000u) {
            /* moveq #imm8,Dn : sign-extend the 8-bit immediate to 32 bits. */
            unsigned dn  = (op >> 9) & 7u;
            int32_t  imm = (int8_t)(op & 0xFFu);     /* sign-extended */
            st->d[dn] = (uint32_t)imm;
            set_nz_clr_vc(st, (uint32_t)imm);        /* moveq: N,Z set; V,C cleared */
            st->pc += 2;
            continue;
        }

        if ((op & 0xF000u) == 0xD000u) {
            /* ADD: 1101 nnn ooo eeeeee. opmode 010b = .L with <ea>+Dn->Dn;
             * ea-mode 000b = data register direct (Dm). 0xD081 -> Dn=0, op=2, D1. */
            unsigned dn     = (op >> 9) & 7u;        /* destination Dn */
            unsigned opmode = (op >> 6) & 7u;
            unsigned eamode = (op >> 3) & 7u;
            unsigned eareg  = op & 7u;
            if (opmode == 2 /*010b: .L, <ea>+Dn->Dn*/ && eamode == 0 /*Dm direct*/) {
                uint32_t src = st->d[eareg];
                uint32_t dst = st->d[dn];
                uint64_t sum = (uint64_t)dst + (uint64_t)src;
                uint32_t res = (uint32_t)sum;
                st->d[dn] = res;

                /* 68k ADD.L flag rules (PRM): N,Z from result; C=carry-out (bit32);
                 * V=signed overflow; X:=C. */
                uint32_t ccr = st->ccr & ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
                if (res & 0x80000000u)            ccr |= CCR_N;
                if (res == 0)                     ccr |= CCR_Z;
                if (sum >> 32)                    ccr |= (CCR_C | CCR_X);
                /* signed overflow: src,dst same sign, result differs */
                if (((~(dst ^ src)) & (dst ^ res)) & 0x80000000u)
                    ccr |= CCR_V;
                st->ccr = ccr;
                st->pc += 2;
                continue;
            }
            /* out-of-subset add form — stop (shouldn't happen for the [J2] block) */
            return;
        }

        if (op == 0x4E75u) {
            /* rts: ends the basic block (the dispatcher funnel). */
            return;
        }

        /* Out-of-subset opcode: stop (the [J2] block never reaches here). */
        return;
    }
}
