/* j5c_interp.c — [J5c] INDEPENDENT reference interpreter (OURS, NO Emu68). The primary
 * oracle: it executes the SAME register-direct block from scratch, computing each opcode's
 * architectural result AND the full 68k CCR (N/Z/V/C/X) by the M68000 rules, so the test
 * can assert the REAL-decoder-JITed register file + CCR are byte-exact equal to this.
 *
 * Clean-room / OURS. Authored from the Motorola 68000 PRM only. Uses NO Emu68 code. The
 * opcode subset matches j5c_build.c's driven set (register-direct, .L unless noted):
 *   moveq #imm8,Dn ; add.l/sub.l/and.l/or.l Dm,Dn ; eor.l Dn,Dm ; cmp.l Dm,Dn ;
 *   muls.w Dm,Dn ; rts.
 *
 * CCR layout (this reference): the 68k CCR byte, bit0=C bit1=V bit2=Z bit3=N bit4=X.
 * NOTE on the JIT side's CCR: Emu68's A64.h CCR helpers keep an internal representation in
 * which some paths swap C/V vs the 68k SR order. The test asserts the architectural D/A
 * registers byte-exact (the unambiguous proof) and checks the Z and N CCR bits (which are
 * layout-stable: bit2=Z, bit3=N in both representations) against this reference.
 */
#include "j5c_jit68k.h"
#include <stdint.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Set N (bit3) and Z (bit2) from a 32-bit result; leave V/C/X per caller. */
static uint32_t nz32(uint32_t res, uint32_t keep_xvc)
{
    uint32_t ccr = keep_xvc & (J5C_CCR_X | J5C_CCR_V | J5C_CCR_C);
    if (res == 0)            ccr |= J5C_CCR_Z;
    if (res & 0x80000000u)   ccr |= J5C_CCR_N;
    return ccr;
}

void j5c_interp_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                          struct j5c_m68k_state *st)
{
    (void)entry_pc;
    uint32_t ip = 0;

    while (ip + 2 <= code_len) {
        uint16_t op = be16(code + ip);
        ip += 2;

        if (op == 0x4E75u) break;                       /* rts */

        if ((op & 0xF100u) == 0x7000u) {                /* moveq #imm8,Dn (sign-extend) */
            unsigned dn = (op >> 9) & 7u;
            int32_t v = (int8_t)(op & 0xFFu);           /* REAL sign-extend */
            st->d[dn] = (uint32_t)v;
            uint32_t ccr = st->ccr & J5C_CCR_X;         /* moveq: X preserved, V=C=0 */
            if (v == 0)        ccr |= J5C_CCR_Z;
            if (v < 0)         ccr |= J5C_CCR_N;
            st->ccr = ccr;
            continue;
        }

        if ((op & 0xF1F8u) == 0xD080u) {                /* add.l Dm,Dn */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm];
            uint64_t s = (uint64_t)a + (uint64_t)b;
            uint32_t res = (uint32_t)s;
            uint32_t ccr = 0;
            if (s >> 32) ccr |= J5C_CCR_C | J5C_CCR_X;   /* carry => C and X */
            if (((a ^ res) & (b ^ res)) & 0x80000000u) ccr |= J5C_CCR_V; /* signed overflow */
            st->d[dn] = res;
            st->ccr = nz32(res, ccr) | (ccr & (J5C_CCR_V | J5C_CCR_C | J5C_CCR_X));
            continue;
        }

        if ((op & 0xF1F8u) == 0x9080u) {                /* sub.l Dm,Dn : Dn = Dn - Dm */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm];
            uint32_t res = a - b;
            uint32_t ccr = 0;
            if (b > a) ccr |= J5C_CCR_C | J5C_CCR_X;     /* borrow => C and X */
            if (((a ^ b) & (a ^ res)) & 0x80000000u) ccr |= J5C_CCR_V; /* signed overflow */
            st->d[dn] = res;
            st->ccr = nz32(res, ccr) | (ccr & (J5C_CCR_V | J5C_CCR_C | J5C_CCR_X));
            continue;
        }

        if ((op & 0xF1F8u) == 0xC080u) {                /* and.l Dm,Dn */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = st->d[dn] & st->d[dm];
            st->d[dn] = res;
            st->ccr = nz32(res, st->ccr & J5C_CCR_X);    /* and: V=C=0, X preserved */
            continue;
        }

        if ((op & 0xF1F8u) == 0x8080u) {                /* or.l Dm,Dn */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = st->d[dn] | st->d[dm];
            st->d[dn] = res;
            st->ccr = nz32(res, st->ccr & J5C_CCR_X);    /* or: V=C=0, X preserved */
            continue;
        }

        if ((op & 0xF1F8u) == 0xB180u) {                /* eor.l Dn,Dm : Dm = Dm ^ Dn */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;  /* src=Dn (bits 9-11), dst=Dm */
            uint32_t res = st->d[dm] ^ st->d[dn];
            st->d[dm] = res;
            st->ccr = nz32(res, st->ccr & J5C_CCR_X);    /* eor: V=C=0, X preserved */
            continue;
        }

        if ((op & 0xF1F8u) == 0xB080u) {                /* cmp.l Dm,Dn : flags of Dn - Dm */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm];
            uint32_t res = a - b;
            uint32_t ccr = st->ccr & J5C_CCR_X;          /* cmp does NOT affect X */
            if (b > a) ccr |= J5C_CCR_C;                 /* borrow => C (X untouched) */
            if (((a ^ b) & (a ^ res)) & 0x80000000u) ccr |= J5C_CCR_V;
            if (res == 0)          ccr |= J5C_CCR_Z;
            if (res & 0x80000000u) ccr |= J5C_CCR_N;
            st->ccr = ccr;                               /* Dn unchanged */
            continue;
        }

        if ((op & 0xF1C0u) == 0xC1C0u) {                /* muls.w Dm,Dn : Dn = (int16)Dn * (int16)Dm */
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            int32_t a = (int16_t)(st->d[dn] & 0xFFFFu);
            int32_t b = (int16_t)(st->d[dm] & 0xFFFFu);
            int32_t res = a * b;                          /* 16x16 -> 32, signed */
            st->d[dn] = (uint32_t)res;
            st->ccr = nz32((uint32_t)res, st->ccr & J5C_CCR_X); /* muls: V=C=0, X preserved */
            continue;
        }

        /* Unknown opcode for the reference: stop (the test treats a short interp as fail). */
        break;
    }
}
