/* j5d_interp.c — [J5d] INDEPENDENT reference interpreter (OURS, NO Emu68). The oracle
 * the test asserts the REAL-decoder JIT against, byte-exact, on the WHOLE corpus.
 *
 * Clean-room / OURS. Authored from the Motorola 68000 PRM only; uses NO Emu68 code. It
 * decodes the exact opcode/addressing-mode set the four apps68k programs use, executes
 * over the SAME [J5d] sandbox (so memory effects + the loader's relocation are real),
 * and routes `jsr d16(A6)` through the SAME library bridge the JIT uses — so the test
 * can compare the JITed and reference register files, sandbox memory, and observed
 * library-call args/returns and demand they agree exactly.
 *
 * Opcodes (matching j5d_engine.c's driven set + the dispatcher-handled terminators):
 *   moveq #imm8,Dn ; move.l #imm32,Dn ; movea.l #imm32,An ; move.l Dn,Dm ;
 *   movea.l Dn,An ; movea.l An,Am ; lea abs.l,An ; add.l Dm,Dn ; add.l (An)+,Dn ;
 *   sub.l Dm,Dn ; and.l/or.l/eor.l Dm,Dn ; cmp.l Dm,Dn ; muls.w Dm,Dn ;
 *   addq.l #imm,Dn ; subq.l #imm,Dn ; bra.s/bne.s/beq.s ; jsr d16(A6) ; rts.
 *
 * CCR layout: the 68k CCR byte, bit0=C bit1=V bit2=Z bit3=N bit4=X (J5D_CCR_*). */
#include "j5d_jit68k.h"
#include <string.h>
#include <stdio.h>

#define STEP_CAP 2000000u

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  be16s(const uint8_t *p){ return (int16_t)be16(p); }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* N/Z from a 32-bit result, keeping the caller's V/C/X. */
static uint32_t nz32(uint32_t res, uint32_t keep)
{
    uint32_t ccr = keep & (J5D_CCR_X | J5D_CCR_V | J5D_CCR_C);
    if (res == 0)          ccr |= J5D_CCR_Z;
    if (res & 0x80000000u) ccr |= J5D_CCR_N;
    return ccr;
}

/* [J5f] condition-code table: does a 68k Bcc with 4-bit condition `cc4` branch given the
 * 68k CCR? Mirrors j5d_engine.c's bcc_taken (M68000 PRM). cc4 0/1 = BRA/BSR (caller). */
static int bcc_taken(unsigned cc4, uint32_t ccr)
{
    int N = (ccr & J5D_CCR_N) != 0, Z = (ccr & J5D_CCR_Z) != 0;
    int V = (ccr & J5D_CCR_V) != 0, C = (ccr & J5D_CCR_C) != 0;
    switch (cc4) {
        case 0x2: return !C && !Z;   case 0x3: return  C ||  Z;   /* HI / LS */
        case 0x4: return !C;         case 0x5: return  C;         /* CC / CS */
        case 0x6: return !Z;         case 0x7: return  Z;         /* NE / EQ */
        case 0x8: return !V;         case 0x9: return  V;         /* VC / VS */
        case 0xA: return !N;         case 0xB: return  N;         /* PL / MI */
        case 0xC: return  N == V;    case 0xD: return  N != V;    /* GE / LT */
        case 0xE: return !Z && (N == V); case 0xF: return Z || (N != V); /* GT / LE */
        default:  return 0;
    }
}

/* ============================ [J5g] sandbox memory access (oracle) =================
 * The reference's own model of the SAME [J5d] sandbox: big-endian longword load/store
 * with the same bounds rule the engine validates. Returns 0 on success, sets *oob on a
 * sandbox-range violation (the caller turns that into an IFAIL). */
static uint32_t mem_rd32(j5d_sandbox *sb, uint32_t addr, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 4 > (uint64_t)sb->origin + sb->size) { *oob = 1; return 0; }
    return be32(sb->host_mem + (addr - sb->origin));
}
static void mem_wr32(j5d_sandbox *sb, uint32_t addr, uint32_t v, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 4 > (uint64_t)sb->origin + sb->size) { *oob = 1; return; }
    uint8_t *p = sb->host_mem + (addr - sb->origin);
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=(uint8_t)v;
}

/* ============================ [J5g] CCR helpers (oracle) ===========================
 * Logical ops (and/or/eor/not/tst/move/swap/ext/clr): N,Z from result; V=0; C=0; X kept. */
static uint32_t logic_ccr(uint32_t res, uint32_t keepX)
{
    uint32_t ccr = keepX & J5D_CCR_X;
    if (res == 0)          ccr |= J5D_CCR_Z;
    if (res & 0x80000000u) ccr |= J5D_CCR_N;
    return ccr;   /* V=0, C=0 */
}

int j5d_interp_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                   struct j5d_m68k_state *st, uint32_t *exit_d0,
                   j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen)
{
#define IFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s @pc=%08x", (msg), pc); return 1; } while (0)
    /* d/a left as seeded by the caller; A6 = library base, PC = entry. [J5f]: seed a7
     * to the top of the sandbox (the SAME initial SP the engine uses) and model the real
     * return stack — a top-level rts (back at the initial SP) is the program exit. */
    st->a[6] = a6_libbase;
    uint32_t initial_sp = st->a[7];
    if (initial_sp == 0)
        initial_sp = (sb->origin + sb->size) & ~0xFu;
    st->a[7] = initial_sp;
    uint32_t pc = entry_pc;
    uint32_t steps = 0;

    for (;;) {
        if (++steps > STEP_CAP) IFAIL("step cap exceeded (runaway/mis-decode)");
        if (pc < sb->origin || (uint64_t)pc + 2 > (uint64_t)sb->origin + sb->size)
            IFAIL("pc out of sandbox");
        const uint8_t *ip = sb->host_mem + (pc - sb->origin);
        uint16_t op = be16(ip);
        st->pc = pc;

        if (op == 0x4E75u) {                                     /* rts            */
            if (st->a[7] >= initial_sp) { *exit_d0 = st->d[0]; return 0; }  /* top-level */
            uint32_t sp = st->a[7];
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("rts: a7 out of sandbox");
            pc = be32(sb->host_mem + (sp - sb->origin));         /* pop big-endian */
            st->a[7] = sp + 4u;
            continue;
        }
        if (op == 0x4E71u) { pc += 2; continue; }                /* nop            */

        /* jsr d16(A6) -> library bridge (same as the JIT dispatcher; no stack push). */
        if (op == 0x4EAEu) {
            int16_t d16 = be16s(ip + 2);
            uint32_t target = st->a[6] + (uint32_t)(int32_t)d16;
            if (target > st->a[6]) IFAIL("jsr(A6) target above libbase");
            uint32_t delta = st->a[6] - target;
            if (delta % 6u) IFAIL("jsr(A6) not on a 6-byte vector");
            int n = (int)(delta / 6u);
            if (!lvo) IFAIL("jsr(A6) but no bridge");
            if (lvo(n, st, user, errbuf, errlen)) return 1;
            pc += 4; continue;
        }

        /* jsr abs.l (4EB9) -> push return + jump. */
        if (op == 0x4EB9u) {
            uint32_t target = be32(ip + 2);
            uint32_t sp = st->a[7] - 4u;
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("jsr abs.l: a7 out of sandbox");
            uint32_t ret = pc + 6;
            uint8_t *p = sb->host_mem + (sp - sb->origin);
            p[0]=ret>>24; p[1]=ret>>16; p[2]=ret>>8; p[3]=(uint8_t)ret;
            st->a[7] = sp; pc = target; continue;
        }
        /* jsr (An) (4E90|An) -> computed push + jump. */
        if ((op & 0xFFF8u) == 0x4E90u) {
            unsigned an = op & 7u; uint32_t target = st->a[an];
            uint32_t sp = st->a[7] - 4u;
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("jsr (An): a7 out of sandbox");
            uint32_t ret = pc + 2;
            uint8_t *p = sb->host_mem + (sp - sb->origin);
            p[0]=ret>>24; p[1]=ret>>16; p[2]=ret>>8; p[3]=(uint8_t)ret;
            st->a[7] = sp; pc = target; continue;
        }
        /* jmp abs.l (4EF9) -> jump (no push). */
        if (op == 0x4EF9u) { pc = be32(ip + 2); continue; }
        /* jmp (An) (4ED0|An) -> computed jump (no push). */
        if ((op & 0xFFF8u) == 0x4ED0u) { pc = st->a[op & 7u]; continue; }

        /* moveq #imm8,Dn (sign-extend) */
        if ((op & 0xF100u) == 0x7000u) {
            unsigned dn = (op >> 9) & 7u;
            int32_t v = (int8_t)(op & 0xFFu);
            st->d[dn] = (uint32_t)v;
            uint32_t ccr = st->ccr & J5D_CCR_X;
            if (v == 0) ccr |= J5D_CCR_Z;
            if (v < 0)  ccr |= J5D_CCR_N;
            st->ccr = ccr;
            pc += 2; continue;
        }

        /* move.l #imm32,Dn : 203C + imm32 */
        if ((op & 0xF1FFu) == 0x203Cu) {
            unsigned dn = (op >> 9) & 7u;
            uint32_t v = be32(ip + 2);
            st->d[dn] = v;
            st->ccr = nz32(v, st->ccr & J5D_CCR_X);   /* move sets NZ, V=C=0 */
            pc += 6; continue;
        }
        /* movea.l #imm32,An : 207C + imm32  (movea does NOT affect flags) */
        if ((op & 0xF1FFu) == 0x207Cu) {
            unsigned an = (op >> 9) & 7u;
            st->a[an] = be32(ip + 2);
            pc += 6; continue;
        }
        /* move.l Dm,Dn (reg->reg) : 2000 | dn<<9 | dm */
        if ((op & 0xF1F8u) == 0x2000u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            st->d[dn] = st->d[dm];
            st->ccr = nz32(st->d[dn], st->ccr & J5D_CCR_X);
            pc += 2; continue;
        }
        /* movea.l Dn,An : 2040 (no flags) */
        if ((op & 0xF1F8u) == 0x2040u) {
            unsigned an = (op >> 9) & 7u, dn = op & 7u;
            st->a[an] = st->d[dn]; pc += 2; continue;
        }
        /* movea.l An,Am : 2048 (no flags) */
        if ((op & 0xF1F8u) == 0x2048u) {
            unsigned am = (op >> 9) & 7u, an = op & 7u;
            st->a[am] = st->a[an]; pc += 2; continue;
        }

        /* lea abs.l,An : 41F9 + abs32 (relocated by the loader; no flags) */
        if ((op & 0xF1FFu) == 0x41F9u) {
            unsigned an = (op >> 9) & 7u;
            st->a[an] = be32(ip + 2); pc += 6; continue;
        }
        /* lea (d16,pc),An : 41FA + d16 (PC-relative; PC = address of the ext word) */
        if ((op & 0xF1FFu) == 0x41FAu) {
            unsigned an = (op >> 9) & 7u;
            int16_t d16 = be16s(ip + 2);
            st->a[an] = (uint32_t)((int64_t)(pc + 2) + d16);
            pc += 4; continue;
        }
        /* lea (d16,An),Am : 41E8 | am<<9 | an  (displacement; address compute, no flags) */
        if ((op & 0xF1F8u) == 0x41E8u) {
            unsigned am = (op >> 9) & 7u, an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            st->a[am] = st->a[an] + (uint32_t)(int32_t)d16;
            pc += 4; continue;
        }
        /* lea (d8,An,Xn),Am : 41F0 | am<<9 | an  (indexed; address compute, no flags) */
        if ((op & 0xF1F8u) == 0x41F0u) {
            unsigned am = (op >> 9) & 7u, an = op & 7u;
            uint16_t brief = be16(ip + 2);
            int8_t d8 = (int8_t)(brief & 0xFFu);
            unsigned ix = (brief >> 12) & 7u;
            uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
            if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
            st->a[am] = st->a[an] + (uint32_t)(int32_t)d8 + index;
            pc += 4; continue;
        }

        /* add.l Dm,Dn : D080 */
        if ((op & 0xF1F8u) == 0xD080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm];
            uint64_t s = (uint64_t)a + b; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }
        /* add.l (An)+,Dn : D098 (post-increment longword load) */
        if ((op & 0xF1F8u) == 0xD098u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            uint32_t addr = st->a[an];
            if (addr < sb->origin || (uint64_t)addr + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("add.l (An)+ address out of sandbox");
            uint32_t b = be32(sb->host_mem + (addr - sb->origin));
            uint32_t a = st->d[dn]; uint64_t s = (uint64_t)a + b; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->a[an] = addr + 4;
            st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }

        /* sub.l Dm,Dn : 9080  (Dn = Dn - Dm) */
        if ((op & 0xF1F8u) == 0x9080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm]; uint32_t res = a - b;
            uint32_t cc = 0;
            if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }

        /* and.l Dm,Dn : C080 */
        if ((op & 0xF1F8u) == 0xC080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = st->d[dn] & st->d[dm];
            st->d[dn] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 2; continue;
        }
        /* or.l Dm,Dn : 8080 */
        if ((op & 0xF1F8u) == 0x8080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = st->d[dn] | st->d[dm];
            st->d[dn] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 2; continue;
        }
        /* eor.l Dn,Dm : B180 (dst=Dm, src=Dn) */
        if ((op & 0xF1F8u) == 0xB180u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = st->d[dm] ^ st->d[dn];
            st->d[dm] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 2; continue;
        }
        /* cmp.l Dm,Dn : B080 (flags of Dn - Dm; X untouched) */
        if ((op & 0xF1F8u) == 0xB080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t a = st->d[dn], b = st->d[dm]; uint32_t res = a - b;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a) cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            if (res == 0)          cc |= J5D_CCR_Z;
            if (res & 0x80000000u) cc |= J5D_CCR_N;
            st->ccr = cc; pc += 2; continue;
        }
        /* muls.w Dm,Dn : C1C0 (Dn = (i16)Dn * (i16)Dm) */
        if ((op & 0xF1C0u) == 0xC1C0u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            int32_t a = (int16_t)(st->d[dn] & 0xFFFFu);
            int32_t b = (int16_t)(st->d[dm] & 0xFFFFu);
            uint32_t res = (uint32_t)(a * b);
            st->d[dn] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 2; continue;
        }

        /* addq.l #imm,Dn : 5080 | q<<9 | dn  (q==0 => 8) */
        if ((op & 0xF1F8u) == 0x5080u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            uint32_t a = st->d[dn]; uint64_t s = (uint64_t)a + imm; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (imm ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }
        /* subq.l #imm,Dn : 5180 | q<<9 | dn  (q==0 => 8) */
        if ((op & 0xF1F8u) == 0x5180u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            uint32_t a = st->d[dn]; uint32_t res = a - imm;
            uint32_t cc = 0;
            if (imm > a) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ imm) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }

        /* Bcc / BRA / BSR : 6xxx, all .B/.W/.L displacement widths (cc4 0=BRA 1=BSR). */
        if ((op & 0xF000u) == 0x6000u) {
            unsigned cc4 = (op >> 8) & 0xFu;
            int8_t disp8 = (int8_t)(op & 0xFFu);
            uint32_t base = pc + 2;                  /* disp relative to (PC+2)     */
            uint32_t target, after;
            if (disp8 == 0x00) {                     /* .W                          */
                int16_t d16 = be16s(ip + 2);
                target = (uint32_t)((int64_t)base + d16); after = pc + 4;
            } else if ((uint8_t)disp8 == 0xFFu) {    /* .L                          */
                int32_t d32 = (int32_t)be32(ip + 2);
                target = (uint32_t)((int64_t)base + d32); after = pc + 6;
            } else {                                 /* .B                          */
                target = (uint32_t)((int64_t)base + disp8); after = pc + 2;
            }
            if (cc4 == 0x1) {                        /* BSR -> push + jump          */
                uint32_t sp = st->a[7] - 4u;
                if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                    IFAIL("bsr: a7 out of sandbox");
                uint8_t *p = sb->host_mem + (sp - sb->origin);
                p[0]=after>>24; p[1]=after>>16; p[2]=after>>8; p[3]=(uint8_t)after;
                st->a[7] = sp; pc = target; continue;
            }
            int take = (cc4 == 0x0) ? 1 : bcc_taken(cc4, st->ccr);
            pc = take ? target : after;
            continue;
        }

        /* ============================ [J5g] move.l with an EA operand ===============
         * move.l <ea>,Dn  and  move.l Dn,<ea>  where <ea> is mode 5 (d16,An) or mode 6
         * (d8,An,Xn.L) — the bubble-sort array element access. opcode bits:
         *   15-12 = 0b0010 (.L) ; 11-9 dst reg ; 8-6 dst mode ; 5-3 src mode ; 2-0 src reg.
         * The indexed brief extension word (mode 6): bit15 D/A, 14-12 index reg, bit11 W/L,
         * bits 10-9 scale (M68000: must be 0), bit8=0 (brief), 7-0 signed d8. */
        if ((op & 0xF000u) == 0x2000u) {     /* a move.l (size field == 01) */
            unsigned dst_reg  = (op >> 9) & 7u;
            unsigned dst_mode = (op >> 6) & 7u;
            unsigned src_mode = (op >> 3) & 7u;
            unsigned src_reg  =  op       & 7u;
            const uint8_t *ext = ip + 2;
            int oob = 0;
            uint32_t srcval; int handled = 1;

            /* ---- read the source operand ---- */
            if (src_mode == 0) { srcval = st->d[src_reg]; }
            else if (src_mode == 1) { srcval = st->a[src_reg]; }   /* movea source An */
            else if (src_mode == 2) { srcval = mem_rd32(sb, st->a[src_reg], &oob); }  /* (An) */
            else if (src_mode == 3) {                 /* (An)+ */
                srcval = mem_rd32(sb, st->a[src_reg], &oob);
                if (!oob) st->a[src_reg] += 4u;
            }
            else if (src_mode == 4) {                 /* -(An) */
                st->a[src_reg] -= 4u;
                srcval = mem_rd32(sb, st->a[src_reg], &oob);
            }
            else if (src_mode == 6) {                 /* (d8,An,Xn.L) */
                uint16_t brief = be16(ext); ext += 2;
                int8_t d8 = (int8_t)(brief & 0xFFu);
                unsigned ix = (brief >> 12) & 7u;
                uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index; /* W index */
                uint32_t addr = st->a[src_reg] + (uint32_t)(int32_t)d8 + index;
                srcval = mem_rd32(sb, addr, &oob);
            }
            else if (src_mode == 5) {                 /* (d16,An) */
                int16_t d16 = be16s(ext); ext += 2;
                uint32_t addr = st->a[src_reg] + (uint32_t)(int32_t)d16;
                srcval = mem_rd32(sb, addr, &oob);
            }
            else if (src_mode == 7 && src_reg == 0) {  /* abs.w (sign-extended 16-bit addr) */
                uint32_t addr = (uint32_t)(int32_t)be16s(ext); ext += 2;
                srcval = mem_rd32(sb, addr, &oob);
            }
            else if (src_mode == 7 && src_reg == 1) {  /* abs.l (32-bit address) */
                uint32_t addr = be32(ext); ext += 4;
                srcval = mem_rd32(sb, addr, &oob);
            }
            else if (src_mode == 7 && src_reg == 2) {  /* (d16,PC) — PC = addr of ext word */
                uint32_t pcbase = pc + (uint32_t)(ext - ip);
                int16_t d16 = be16s(ext); ext += 2;
                srcval = mem_rd32(sb, pcbase + (uint32_t)(int32_t)d16, &oob);
            }
            else { handled = 0; srcval = 0; }

            if (handled && !oob) {
                /* ---- write the destination ---- */
                if (dst_mode == 0) { st->d[dst_reg] = srcval; st->ccr = logic_ccr(srcval, st->ccr); }
                else if (dst_mode == 1) { st->a[dst_reg] = srcval; /* movea: no flags */ }
                else if (dst_mode == 2) { mem_wr32(sb, st->a[dst_reg], srcval, &oob);
                                          st->ccr = logic_ccr(srcval, st->ccr); }
                else if (dst_mode == 3) { mem_wr32(sb, st->a[dst_reg], srcval, &oob);
                                          if (!oob) st->a[dst_reg] += 4u;
                                          st->ccr = logic_ccr(srcval, st->ccr); }
                else if (dst_mode == 4) { st->a[dst_reg] -= 4u;
                                          mem_wr32(sb, st->a[dst_reg], srcval, &oob);
                                          st->ccr = logic_ccr(srcval, st->ccr); }
                else if (dst_mode == 6) {             /* (d8,An,Xn.L) store */
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    uint32_t addr = st->a[dst_reg] + (uint32_t)(int32_t)d8 + index;
                    mem_wr32(sb, addr, srcval, &oob);
                    st->ccr = logic_ccr(srcval, st->ccr);
                }
                else if (dst_mode == 5) {             /* (d16,An) store */
                    int16_t d16 = be16s(ext); ext += 2;
                    uint32_t addr = st->a[dst_reg] + (uint32_t)(int32_t)d16;
                    mem_wr32(sb, addr, srcval, &oob);
                    st->ccr = logic_ccr(srcval, st->ccr);
                }
                else if (dst_mode == 7 && dst_reg == 0) {  /* abs.w store */
                    uint32_t addr = (uint32_t)(int32_t)be16s(ext); ext += 2;
                    mem_wr32(sb, addr, srcval, &oob);
                    st->ccr = logic_ccr(srcval, st->ccr);
                }
                else if (dst_mode == 7 && dst_reg == 1) {  /* abs.l store */
                    uint32_t addr = be32(ext); ext += 4;
                    mem_wr32(sb, addr, srcval, &oob);
                    st->ccr = logic_ccr(srcval, st->ccr);
                }
                else handled = 0;
            }
            if (handled) {
                if (oob) IFAIL("move.l EA access out of sandbox");
                pc += (uint32_t)(ext - ip); continue;
            }
        }

        /* move.w Dm,Dn : 3000 | dn<<9 | dm  (writes only the low word; upper word kept) */
        if ((op & 0xF1F8u) == 0x3000u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint16_t w = (uint16_t)(st->d[dm] & 0xFFFFu);
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | w;
            /* move.w sets N/Z from the 16-bit value; V=C=0; X kept. */
            uint32_t ccr = st->ccr & J5D_CCR_X;
            if (w == 0)        ccr |= J5D_CCR_Z;
            if (w & 0x8000u)   ccr |= J5D_CCR_N;
            st->ccr = ccr;
            pc += 2; continue;
        }

        /* ============================ [J5g] LINE0 immediates ========================
         * addi/subi/andi/ori/eori/cmpi #imm32,Dn (long) + btst #bit,Dn (static).
         * op high byte selects the operation; bits 7-6 = size (10 = .L); EA = Dn (mode 0).
         * Encodings (the demanding program is all .L, Dn): 0680=addi 0480=subi 0280=andi
         * 0080=ori 0a80=eori 0c80=cmpi (+ dn in bits 2-0); 0800=btst static. */
        {
            uint16_t hi = op & 0xFF00u;           /* operation+size selector */
            unsigned ea_mode = (op >> 3) & 7u;
            unsigned dn = op & 7u;
            if (ea_mode == 0 && (op & 0x00C0u) == 0x0080u &&     /* .L, Dn */
                (hi==0x0600u||hi==0x0400u||hi==0x0200u||hi==0x0000u||hi==0x0A00u||hi==0x0C00u)) {
                uint32_t imm = be32(ip + 2);
                uint32_t a = st->d[dn], res; uint32_t cc;
                if (hi == 0x0600u) {                 /* addi.l */
                    uint64_t s=(uint64_t)a+imm; res=(uint32_t)s; cc=0;
                    if (s>>32) cc|=J5D_CCR_C|J5D_CCR_X;
                    if (((a^res)&(imm^res))&0x80000000u) cc|=J5D_CCR_V;
                    st->d[dn]=res; st->ccr=nz32(res,cc)|(cc&(J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
                } else if (hi == 0x0400u) {          /* subi.l */
                    res=a-imm; cc=0;
                    if (imm>a) cc|=J5D_CCR_C|J5D_CCR_X;
                    if (((a^imm)&(a^res))&0x80000000u) cc|=J5D_CCR_V;
                    st->d[dn]=res; st->ccr=nz32(res,cc)|(cc&(J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
                } else if (hi == 0x0C00u) {          /* cmpi.l (flags only; X kept) */
                    res=a-imm; cc=st->ccr & J5D_CCR_X;
                    if (imm>a) cc|=J5D_CCR_C;
                    if (((a^imm)&(a^res))&0x80000000u) cc|=J5D_CCR_V;
                    if (res==0) cc|=J5D_CCR_Z;
                    if (res&0x80000000u) cc|=J5D_CCR_N;
                    st->ccr=cc;
                } else if (hi == 0x0200u) {          /* andi.l */
                    res=a&imm; st->d[dn]=res; st->ccr=logic_ccr(res, st->ccr);
                } else if (hi == 0x0000u) {          /* ori.l  */
                    res=a|imm; st->d[dn]=res; st->ccr=logic_ccr(res, st->ccr);
                } else {                             /* eori.l (0x0A00) */
                    res=a^imm; st->d[dn]=res; st->ccr=logic_ccr(res, st->ccr);
                }
                pc += 6; continue;
            }
            /* btst #bit,Dn (static, immediate bit number in the next word; Dn => bit mod 32).
             * Sets Z = !(bit); N/V/C/X unchanged. op = 0800 | (mode 0, dn). */
            if (hi == 0x0800u && ea_mode == 0) {
                unsigned bit = (be16(ip + 2) & 31u);   /* Dn: bit number modulo 32 */
                uint32_t tested = (st->d[dn] >> bit) & 1u;
                uint32_t ccr = st->ccr & ~J5D_CCR_Z;
                if (!tested) ccr |= J5D_CCR_Z;
                st->ccr = ccr;
                pc += 4; continue;
            }
        }

        /* ============================ [J5g] LINE4 misc =============================
         * clr.l/neg.l/not.l/tst.l Dn, swap Dn, ext.l Dn. All EA = Dn (mode 0) for the
         * demanding program. */
        /* clr.l Dn : 4280 | dn  (clears Dn; N=0,Z=1,V=0,C=0; X kept) */
        if ((op & 0xFFF8u) == 0x4280u) {
            unsigned dn = op & 7u; st->d[dn] = 0;
            st->ccr = (st->ccr & J5D_CCR_X) | J5D_CCR_Z;
            pc += 2; continue;
        }
        /* neg.l Dn : 4480 | dn  (Dn = 0 - Dn) */
        if ((op & 0xFFF8u) == 0x4480u) {
            unsigned dn = op & 7u; uint32_t a = st->d[dn]; uint32_t res = 0u - a;
            uint32_t cc = 0;
            if (a != 0) cc |= J5D_CCR_C | J5D_CCR_X;       /* borrow out of 0-a */
            if (a == 0x80000000u) cc |= J5D_CCR_V;          /* overflow */
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }
        /* not.l Dn : 4680 | dn  (one's complement; N,Z; V=C=0; X kept) */
        if ((op & 0xFFF8u) == 0x4680u) {
            unsigned dn = op & 7u; uint32_t res = ~st->d[dn];
            st->d[dn] = res; st->ccr = logic_ccr(res, st->ccr);
            pc += 2; continue;
        }
        /* tst.l Dn : 4A80 | dn  (flags from Dn; N,Z; V=C=0; X kept) */
        if ((op & 0xFFF8u) == 0x4A80u) {
            unsigned dn = op & 7u; st->ccr = logic_ccr(st->d[dn], st->ccr);
            pc += 2; continue;
        }
        /* swap Dn : 4840 | dn  (swap the 16-bit halves; N,Z from 32-bit result; V=C=0) */
        if ((op & 0xFFF8u) == 0x4840u) {
            unsigned dn = op & 7u; uint32_t v = st->d[dn];
            uint32_t res = (v >> 16) | (v << 16);
            st->d[dn] = res; st->ccr = logic_ccr(res, st->ccr);
            pc += 2; continue;
        }
        /* ext.l Dn : 48C0 | dn  (sign-extend word->long; N,Z; V=C=0) */
        if ((op & 0xFFF8u) == 0x48C0u) {
            unsigned dn = op & 7u;
            uint32_t res = (uint32_t)(int32_t)(int16_t)(st->d[dn] & 0xFFFFu);
            st->d[dn] = res; st->ccr = logic_ccr(res, st->ccr);
            pc += 2; continue;
        }
        /* ext.w Dn : 4880 | dn  (sign-extend byte->word, low 16 bits; N,Z; V=C=0) */
        if ((op & 0xFFF8u) == 0x4880u) {
            unsigned dn = op & 7u;
            uint16_t w = (uint16_t)(int16_t)(int8_t)(st->d[dn] & 0xFFu);
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | w;
            uint32_t ccr = st->ccr & J5D_CCR_X;
            if (w == 0) ccr |= J5D_CCR_Z;
            if (w & 0x8000u) ccr |= J5D_CCR_N;
            st->ccr = ccr;
            pc += 2; continue;
        }

        /* ============================ [J5g] LINEE shifts/rotates ====================
         * Immediate-count register shifts/rotates, .L (size field = 10):
         *   op = 1110 ccc d ii s tt rrr  where ccc=count(1..8, 0=>8), d=dir(1=left),
         *   ii=size(10=.L), s=0 (immediate count), tt=type(00 as,01 ls,10 rox,11 ro),
         *   rrr=Dn.  We drive asl/asr/lsl/lsr/rol/ror (rox* not in the program). */
        if ((op & 0xF000u) == 0xE000u && ((op >> 6) & 3u) == 2u && ((op >> 5) & 1u) == 0u) {
            unsigned dn = op & 7u;
            unsigned cnt = (op >> 9) & 7u; if (cnt == 0) cnt = 8;
            unsigned dir = (op >> 8) & 1u;          /* 1 = left */
            unsigned type = (op >> 3) & 3u;         /* 0 as,1 ls,2 rox,3 ro */
            uint32_t a = st->d[dn], res = a;
            uint32_t cc = st->ccr & J5D_CCR_X;      /* most keep X unless they set it */
            unsigned last_out = 0;
            if (type == 1) {                        /* LSL/LSR */
                if (dir) { last_out = (a >> (32 - cnt)) & 1u; res = a << cnt; }
                else     { last_out = (a >> (cnt - 1)) & 1u; res = a >> cnt; }
                cc = 0;
                if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                cc |= nz32(res, 0) & (J5D_CCR_N | J5D_CCR_Z);
                /* X: shifts SET X = C (count != 0); keep none of the old X. */
                st->d[dn] = res; st->ccr = cc; pc += 2; continue;
            } else if (type == 0) {                 /* ASL/ASR */
                if (dir) {
                    last_out = (a >> (32 - cnt)) & 1u; res = a << cnt;
                    /* V set if the sign bit changed at any point during the shift. */
                    uint32_t signbits_mask = 0xFFFFFFFFu << (31 - cnt);  /* top cnt+1 bits */
                    uint32_t top = a & signbits_mask;
                    int vbit = !(top == 0 || top == signbits_mask);
                    cc = 0;
                    if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                    if (vbit) cc |= J5D_CCR_V;
                    cc |= nz32(res, 0) & (J5D_CCR_N | J5D_CCR_Z);
                } else {
                    last_out = (a >> (cnt - 1)) & 1u;
                    res = (uint32_t)(((int32_t)a) >> cnt);   /* arithmetic */
                    cc = 0;
                    if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                    cc |= nz32(res, 0) & (J5D_CCR_N | J5D_CCR_Z);   /* V=0 for ASR */
                }
                st->d[dn] = res; st->ccr = cc; pc += 2; continue;
            } else if (type == 3) {                 /* ROL/ROR (no X) */
                cnt &= 31u; /* rotate amount mod 32 for the carry/result */
                if (dir) { res = (a << cnt) | (a >> ((32 - cnt) & 31)); last_out = res & 1u; }
                else     { res = (a >> cnt) | (a << ((32 - cnt) & 31)); last_out = (res >> 31) & 1u; }
                cc = st->ccr & J5D_CCR_X;            /* ROx do NOT affect X */
                if (last_out) cc |= J5D_CCR_C;
                cc |= nz32(res, 0) & (J5D_CCR_N | J5D_CCR_Z);   /* V=0 */
                st->d[dn] = res; st->ccr = cc; pc += 2; continue;
            }
            /* type 2 (ROXL/ROXR) not driven by the demanding program. */
        }

        IFAIL("interp: out-of-subset opcode");
    }
#undef IFAIL
}
