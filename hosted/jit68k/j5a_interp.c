/* j5a_interp.c — [J5a] INDEPENDENT reference 68k interpreter, extended for memory
 * load/store + addq (OURS, AROS-licensed).
 *
 * Clean-room / OURS. This is the verification ORACLE for [J5a]. Written FROM SCRATCH
 * from the Motorola 68000 ISA only; it touches NO Emu68 source, no Emu68 type/macro,
 * and does NOT decode through Emu68 — so when j5a_test.c asserts the JITed register
 * file AND the sandbox memory (produced by the adopted Emu68 EMITTER, driven by our
 * hand-rolled EA path) equal this interpreter's, the comparison is against a TRULY
 * INDEPENDENT reference.
 *
 * Opcode subset (the [J5a] memory block + the carried-forward register-only ops):
 *   moveq  #imm8,Dn        (0111 ddd 0 iiiiiiii)           sign-extend imm8
 *   add.l  Dm,Dn           (1101 ddd 010 000 mmm)          register-direct .L add
 *   addq.l #imm,Dn         (0101 qqq 0 10 000 ddd)         imm in {1..8}, q=0 => 8
 *   move.l (An),Dn         (0010 ddd 000 010 nnn)          BE load  from sandbox
 *   move.l Dn,(An)         (0010 nnn 010 000 ddd)          BE store to  sandbox
 *   rts                    (0x4E75)                         ends the block
 *
 * Memory accesses are BIG-ENDIAN against the sandbox model (sb->host_mem indexed by
 * (addr - origin)) — the same bytes the JITed block reads/writes — and bounds-checked
 * (an out-of-range access sets st->fault and stops, mirroring the emitted block's
 * clean fault). The interpreter never touches host memory outside the sandbox.
 */
#include "j5a_jit68k.h"

#include <stdint.h>

/* ----- Big-endian helpers over the sandbox model -------------------------------- */
static uint16_t fetch_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* In-range check: [addr, addr+size) must lie within [origin, origin+size_total). */
static int sb_in_range(const j5a_sandbox *sb, uint32_t addr, uint32_t size)
{
    if (addr < sb->origin) return 0;
    uint64_t end = (uint64_t)addr + size;
    if (end > (uint64_t)sb->origin + sb->size) return 0;
    return 1;
}

static uint32_t sb_load_be32(const j5a_sandbox *sb, uint32_t addr)
{
    const uint8_t *p = j5a_sandbox_host(sb, addr);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void sb_store_be32(const j5a_sandbox *sb, uint32_t addr, uint32_t v)
{
    uint8_t *p = j5a_sandbox_host(sb, addr);
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v      );
}

/* moveq flag rule: N,Z from result; V,C cleared; X untouched. */
static void set_nz_clr_vc(struct j5a_m68k_state *st, uint32_t res)
{
    uint32_t ccr = st->ccr & ~(J5A_CCR_N | J5A_CCR_Z | J5A_CCR_V | J5A_CCR_C);
    if (res & 0x80000000u) ccr |= J5A_CCR_N;
    if (res == 0)          ccr |= J5A_CCR_Z;
    st->ccr = ccr;
}

/* ADD/ADDQ.L flag rule (PRM): N,Z from result; C=carry-out; V=signed overflow; X:=C. */
static void set_add_flags(struct j5a_m68k_state *st, uint32_t dst, uint32_t src,
                          uint64_t sum, uint32_t res)
{
    uint32_t ccr = st->ccr & ~(J5A_CCR_X | J5A_CCR_N | J5A_CCR_Z | J5A_CCR_V | J5A_CCR_C);
    if (res & 0x80000000u)                        ccr |= J5A_CCR_N;
    if (res == 0)                                 ccr |= J5A_CCR_Z;
    if (sum >> 32)                                ccr |= (J5A_CCR_C | J5A_CCR_X);
    if (((~(dst ^ src)) & (dst ^ res)) & 0x80000000u) ccr |= J5A_CCR_V;
    st->ccr = ccr;
}

/* move.l flag rule (PRM, move to a Dn): N,Z from the moved long; V,C cleared; X kept. */
static void set_move_flags(struct j5a_m68k_state *st, uint32_t res)
{
    set_nz_clr_vc(st, res);
}

void j5a_interp_run_block(const j5a_sandbox *sb, uint32_t entry_pc,
                          const uint8_t *code, uint32_t code_len,
                          struct j5a_m68k_state *st)
{
    uint32_t ip = 0;
    st->pc = entry_pc;
    st->fault = 0;

    while (ip + 2 <= code_len) {
        uint16_t op = fetch_be16(code + ip);
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

        /* addq.l #imm,Dn : 0101 qqq 0 10 000 ddd. q==0 means immediate 8. */
        if ((op & 0xF1F8u) == 0x5080u) {
            unsigned q   = (op >> 9) & 7u;
            unsigned imm = (q == 0) ? 8u : q;
            unsigned dn  = op & 7u;
            uint32_t dst = st->d[dn];
            uint64_t sum = (uint64_t)dst + (uint64_t)imm;
            uint32_t res = (uint32_t)sum;
            st->d[dn] = res;
            set_add_flags(st, dst, imm, sum, res);
            st->pc += 2;
            continue;
        }

        /* move.l (An),Dn : 0010 ddd 000 010 nnn  (dst Dn direct, src (An) indirect). */
        if ((op & 0xF1F8u) == 0x2010u) {
            unsigned dn = (op >> 9) & 7u;
            unsigned an = op & 7u;
            uint32_t addr = st->a[an];
            if (!sb_in_range(sb, addr, 4)) { st->fault = J5A_FAULT_OOB; return; }
            uint32_t val = sb_load_be32(sb, addr);
            st->d[dn] = val;
            set_move_flags(st, val);
            st->pc += 2;
            continue;
        }

        /* move.l Dn,(An) : 0010 nnn 010 000 ddd  (dst (An) indirect, src Dn direct). */
        if ((op & 0xF1F8u) == 0x2080u) {
            unsigned an = (op >> 9) & 7u;
            unsigned dn = op & 7u;
            uint32_t addr = st->a[an];
            uint32_t val  = st->d[dn];
            if (!sb_in_range(sb, addr, 4)) { st->fault = J5A_FAULT_OOB; return; }
            sb_store_be32(sb, addr, val);
            set_move_flags(st, val);
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

        if (op == 0x4E75u)      /* rts — end of block */
            return;

        /* Out-of-subset opcode: stop (the [J5a] block stays in-subset by construction). */
        return;
    }
}
