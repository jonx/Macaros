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
/* [J5l] word (16-bit) sandbox access — movem.w transfers one word per register, big-endian. */
static uint16_t mem_rd16(j5d_sandbox *sb, uint32_t addr, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 2 > (uint64_t)sb->origin + sb->size) { *oob = 1; return 0; }
    return be16(sb->host_mem + (addr - sb->origin));
}
static void mem_wr16(j5d_sandbox *sb, uint32_t addr, uint16_t v, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 2 > (uint64_t)sb->origin + sb->size) { *oob = 1; return; }
    uint8_t *p = sb->host_mem + (addr - sb->origin);
    p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v;
}
/* byte (8-bit) sandbox access — a byte lands at its exact address (endianness-free). */
static uint8_t mem_rd8(j5d_sandbox *sb, uint32_t addr, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 1 > (uint64_t)sb->origin + sb->size) { *oob = 1; return 0; }
    return sb->host_mem[addr - sb->origin];
}
static void mem_wr8(j5d_sandbox *sb, uint32_t addr, uint8_t v, int *oob)
{
    if (addr < sb->origin || (uint64_t)addr + 1 > (uint64_t)sb->origin + sb->size) { *oob = 1; return; }
    sb->host_mem[addr - sb->origin] = v;
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

/* ============================ [J5h] X-chain CCR (the multi-precision rule) ==========
 * addx/subx/negx (and the .w/.b sizes) follow the M68000 PRM (4th ed., the ADDX/SUBX/
 * NEGX instruction pages):
 *   - X = C = carry/borrow OUT of the MSB of the size operated on.
 *   - N from the result's MSB; V from the (sized) two's-complement overflow.
 *   - THE MULTI-PRECISION Z RULE: "Cleared if the result is nonzero; UNCHANGED otherwise."
 *     i.e. Z is NEVER set by these ops — it is only ever CLEARED (ANDed in across the words
 *     of a multi-precision value), so a chain of addx/subx/negx leaves Z set only if EVERY
 *     word was zero. This is the whole point of the X-chain: the running Z accumulates.
 * `sized_x_ccr` builds the CCR from a sized result with explicit carry/overflow and the
 * PRIOR ccr (consulted ONLY to carry Z forward when the result is zero; X is always rewritten
 * here as X=C). `bits` is the operand width (8/16/32); `res` is already masked to that width. */
static uint32_t sized_x_ccr(uint32_t res, unsigned bits, int carry, int overflow,
                            uint32_t prior_ccr)
{
    uint32_t msb = 1u << (bits - 1);
    uint32_t cc = 0;
    if (carry)        cc |= J5D_CCR_C | J5D_CCR_X;   /* X = C = carry/borrow out      */
    if (overflow)     cc |= J5D_CCR_V;
    if (res & msb)    cc |= J5D_CCR_N;
    /* Multi-precision Z: cleared if result nonzero, UNCHANGED otherwise (never set here). */
    if (res != 0)     cc &= ~J5D_CCR_Z;              /* (Z not in cc yet; explicit)   */
    else              cc |= (prior_ccr & J5D_CCR_Z); /* result==0: keep the prior Z   */
    return cc;
}

/* ============================== [J5i] the oracle's SR / EXCEPTION model =============
 * An INDEPENDENT re-derivation of the same 68k exception dispatch the engine does (NO
 * Emu68; the engine's path is in j5d_engine.c). It uses the SHARED j5d_pack_sr / unpack
 * (pure CCR-bit reordering, deterministic, declared in j5d_jit68k.h) so the SR pushed in
 * the frame is bit-identical, then independently builds the frame + reads the vector +
 * logs the record — the test asserts the two logs agree. */
static j5i_exc_log *gi_exc_log = NULL;
void j5d_interp_set_exc_log(j5i_exc_log *log) { gi_exc_log = log; }

static void iunpack_sr(struct j5d_m68k_state *st, uint16_t sr)
{
    uint32_t i = 0;
    if (sr & 0x01u) i |= J5D_CCR_C;
    if (sr & 0x02u) i |= J5D_CCR_V;
    if (sr & 0x04u) i |= J5D_CCR_Z;
    if (sr & 0x08u) i |= J5D_CCR_N;
    if (sr & 0x10u) i |= J5D_CCR_X;
    st->ccr = i;
    st->sr_high = (uint16_t)((sr >> 8) & 0xFFu);
}

/* Read a vector slot (BE longword at J5I_VBR + vnum*4). Returns 1 if out of sandbox. */
static int iread_vector(j5d_sandbox *sb, unsigned vnum, uint32_t *handler)
{
    uint32_t va = J5I_VBR + vnum * 4u;
    if (va < sb->origin || (uint64_t)va + 4 > (uint64_t)sb->origin + sb->size) return 1;
    *handler = be32(sb->host_mem + (va - sb->origin));
    return 0;
}

/* Build the 68k short frame on a7 + set S + log; return the handler PC. 0 ok, 1 error. */
static int iraise(j5d_sandbox *sb, struct j5d_m68k_state *st, unsigned vnum,
                  uint32_t return_pc, uint32_t *handler_out, char *errbuf, unsigned errlen)
{
    uint32_t handler;
    if (iread_vector(sb, vnum, &handler)) {
        if (errbuf) snprintf(errbuf, errlen, "exc: vector %u out of sandbox", vnum); return 1;
    }
    uint16_t sr = j5d_pack_sr(st);
    uint32_t a7 = st->a[7] - 6u;
    if (a7 < sb->origin || (uint64_t)a7 + 6 > (uint64_t)sb->origin + sb->size) {
        if (errbuf) snprintf(errbuf, errlen, "exc: frame a7=%08x out of sandbox", a7); return 1;
    }
    uint8_t *p = sb->host_mem + (a7 - sb->origin);
    p[0]=(uint8_t)(sr>>8); p[1]=(uint8_t)sr;
    p[2]=(uint8_t)(return_pc>>24); p[3]=(uint8_t)(return_pc>>16);
    p[4]=(uint8_t)(return_pc>>8);  p[5]=(uint8_t)return_pc;
    st->a[7] = a7;
    st->sr_high |= (J5D_SR_S >> 8);
    st->exc_count++;
    if (gi_exc_log && gi_exc_log->n < J5I_MAX_EXC) {
        j5i_exc_record *r = &gi_exc_log->rec[gi_exc_log->n++];
        r->vector=(uint8_t)vnum; r->frame_sr=sr; r->frame_pc=return_pc;
        r->a7_at_entry=a7; r->handler_pc=handler;
    }
    *handler_out = handler;
    return 0;
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
        /* [J5i] address/bus error from a bad PC (mirror of the engine dispatcher). */
        if ((pc & 1u) && (pc >= sb->origin) && ((uint64_t)pc + 2 <= (uint64_t)sb->origin + sb->size)) {
            uint32_t h; if (iraise(sb, st, J5I_VEC_ADDRESS_ERROR, pc, &h, errbuf, errlen)) return 1;
            pc = h; continue;
        }
        if (pc < sb->origin || (uint64_t)pc + 2 > (uint64_t)sb->origin + sb->size) {
            uint32_t h; if (iraise(sb, st, J5I_VEC_BUS_ERROR, pc, &h, errbuf, errlen)) return 1;
            pc = h; continue;
        }
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

        /* ============================ [J5i] the exception causes + rte =============== */
        if (op == 0x4E73u) {                                     /* rte -> pop frame */
            uint32_t a7 = st->a[7];
            if (a7 < sb->origin || (uint64_t)a7 + 6 > (uint64_t)sb->origin + sb->size)
                IFAIL("rte: frame a7 out of sandbox");
            const uint8_t *fp = sb->host_mem + (a7 - sb->origin);
            uint16_t sr = (uint16_t)((fp[0] << 8) | fp[1]);
            uint32_t rpc = be32(fp + 2);
            st->a[7] = a7 + 6u;
            iunpack_sr(st, sr);
            pc = rpc; continue;
        }
        if ((op & 0xFFF0u) == 0x4E40u) {                         /* TRAP #n -> 32+n  */
            unsigned n = op & 0xFu; uint32_t h;
            if (iraise(sb, st, J5I_VEC_TRAP_BASE + n, pc + 2, &h, errbuf, errlen)) return 1;
            pc = h; continue;
        }
        if (op == 0x4AFCu) {                                     /* ILLEGAL -> 4     */
            uint32_t h;
            if (iraise(sb, st, J5I_VEC_ILLEGAL, pc, &h, errbuf, errlen)) return 1;
            pc = h; continue;
        }
        if ((op & 0xFFC0u) == 0x80C0u || (op & 0xFFC0u) == 0x81C0u) { /* divu.w/divs.w */
            int is_signed = ((op & 0xFFC0u) == 0x81C0u);
            unsigned dn = (op >> 9) & 7u, mode = (op >> 3) & 7u, srcreg = op & 7u;
            uint32_t divisor; uint32_t after;
            if (mode == 0)                  { divisor = st->d[srcreg] & 0xFFFFu; after = pc + 2; }
            else if (mode == 7 && srcreg==4){ divisor = be16(ip + 2);           after = pc + 4; }
            else IFAIL("divu/divs: unsupported source EA in oracle");
            if (divisor == 0) {
                uint32_t h;
                if (iraise(sb, st, J5I_VEC_DIV_BY_ZERO, after, &h, errbuf, errlen)) return 1;
                pc = h; continue;
            }
            uint32_t dividend = st->d[dn], quot, rem;
            if (is_signed) {
                quot = (uint32_t)((int32_t)dividend / (int32_t)(int16_t)divisor);
                rem  = (uint32_t)((int32_t)dividend % (int32_t)(int16_t)divisor);
            } else { quot = dividend / divisor; rem = dividend % divisor; }
            uint32_t cc = st->ccr & J5D_CCR_X;
            int ovf = is_signed ? ((int32_t)quot < -32768 || (int32_t)quot > 32767)
                                : (quot > 0xFFFFu);
            if (ovf) { st->ccr = cc | J5D_CCR_V; }
            else {
                st->d[dn] = ((rem & 0xFFFFu) << 16) | (quot & 0xFFFFu);
                uint16_t qw = (uint16_t)quot;
                if (qw == 0)      cc |= J5D_CCR_Z;
                if (qw & 0x8000u) cc |= J5D_CCR_N;
                st->ccr = cc;
            }
            pc = after; continue;
        }

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

        /* ============================ [J5h] addx Dy,Dx (register-direct) =============
         * 1101 xxx 1 ss 00 0 yyy : xxx=Dx(dst), ss=size(00 b,01 w,10 l), yyy=Dy(src),
         * mode bits 5-3 = 000 (register). Dx = Dx + Dy + X. X=C=carry out of the sized
         * MSB; multi-precision Z (cleared if nonzero, else unchanged). Matches Emu68's
         * REAL EMIT_ADDX .l/.w/.b register paths byte-exact (verified empirically). */
        if ((op & 0xF138u) == 0xD100u) {
            unsigned dx = (op >> 9) & 7u, dy = op & 7u;
            unsigned sz = (op >> 6) & 3u;                 /* 0=b 1=w 2=l */
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            uint32_t xin = (st->ccr & J5D_CCR_X) ? 1u : 0u;
            uint32_t a = st->d[dx] & mask, b = st->d[dy] & mask;
            uint64_t s = (uint64_t)a + b + xin;
            uint32_t res = (uint32_t)s & mask;
            uint32_t msb = 1u << (bits - 1);
            int carry = (s >> bits) & 1u;
            int overflow = (((a ^ res) & (b ^ res)) & msb) != 0;     /* add overflow */
            st->d[dx] = (st->d[dx] & ~mask) | res;                   /* keep upper bytes */
            st->ccr = sized_x_ccr(res, bits, carry, overflow, st->ccr);
            pc += 2; continue;
        }

        /* ============================ [J5j] ALU with an IMMEDIATE source EA ==========
         * add.l/sub.l/cmp.l #imm32,Dn : the LINED/LINEB/LINEB "<op>.l #imm,Dn" forms the
         * Mandelbrot capstone uses (ADD/SUB/CMP with EA = mode 7 reg 4 = immediate), as
         * opposed to the ADDI/SUBI/CMPI (LINE0) forms or the register-source forms the
         * earlier corpus used. The JIT engine already translates these via the REAL Emu68
         * EMIT_lineD/EMIT_lineB decoders; the oracle was missing them (the [J5j] coverage
         * gap the substantial program surfaced). Flag rules are identical to the register-
         * source add/sub/cmp (same EMU68-internal CCR layout), only the source operand is the
         * 32-bit immediate and the instruction is 6 bytes.
         *   add.l #imm,Dn : 1101 ddd 0 10 111 100 = 0xD0BC | dn<<9
         *   sub.l #imm,Dn : 1001 ddd 0 10 111 100 = 0x90BC | dn<<9
         *   cmp.l #imm,Dn : 1011 ddd 0 10 111 100 = 0xB0BC | dn<<9   */
        if ((op & 0xF1FFu) == 0xD0BCu) {                 /* add.l #imm,Dn */
            unsigned dn = (op >> 9) & 7u; uint32_t a = st->d[dn], b = be32(ip + 2);
            uint64_t s = (uint64_t)a + b; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 6; continue;
        }
        if ((op & 0xF1FFu) == 0x90BCu) {                 /* sub.l #imm,Dn */
            unsigned dn = (op >> 9) & 7u; uint32_t a = st->d[dn], b = be32(ip + 2);
            uint32_t res = a - b; uint32_t cc = 0;
            if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 6; continue;
        }
        if ((op & 0xF1FFu) == 0xB0BCu) {                 /* cmp.l #imm,Dn (flags only; X kept) */
            unsigned dn = (op >> 9) & 7u; uint32_t a = st->d[dn], b = be32(ip + 2);
            uint32_t res = a - b; uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a) cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            if (res == 0)          cc |= J5D_CCR_Z;
            if (res & 0x80000000u) cc |= J5D_CCR_N;
            st->ccr = cc; pc += 6; continue;
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

        /* add.l (An),Dn : D090 | dn<<9 | an  (longword load from (An), no post-increment) */
        if ((op & 0xF1F8u) == 0xD090u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int oob = 0;
            uint32_t b = mem_rd32(sb, st->a[an], &oob);
            if (oob) IFAIL("add.l (An): address out of sandbox");
            uint32_t a = st->d[dn]; uint64_t s = (uint64_t)a + b; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 2; continue;
        }

        /* ============================ [J5h] subx Dy,Dx (register-direct) =============
         * 1001 xxx 1 ss 00 0 yyy : Dx = Dx - Dy - X. X=C=borrow out of the sized MSB;
         * multi-precision Z. Matches Emu68's REAL EMIT_SUBX register paths byte-exact. */
        if ((op & 0xF138u) == 0x9100u) {
            unsigned dx = (op >> 9) & 7u, dy = op & 7u;
            unsigned sz = (op >> 6) & 3u;
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            uint32_t xin = (st->ccr & J5D_CCR_X) ? 1u : 0u;
            uint32_t a = st->d[dx] & mask, b = st->d[dy] & mask;
            uint64_t diff = (uint64_t)a - b - xin;        /* borrow in bit `bits` */
            uint32_t res = (uint32_t)diff & mask;
            uint32_t msb = 1u << (bits - 1);
            int borrow = ((uint64_t)b + xin) > a;
            int overflow = (((a ^ b) & (a ^ res)) & msb) != 0;        /* sub overflow */
            st->d[dx] = (st->d[dx] & ~mask) | res;
            st->ccr = sized_x_ccr(res, bits, borrow, overflow, st->ccr);
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

        /* [J5i] addq.l #imm,An : 5088 | q<<9 | an  (q==0 => 8). ADDA form: full 32-bit add
         * to the address register, NO condition codes (PRM ADDQ-to-An). Used by the J5i
         * illegal handler to pop its own exception frame (addq.l #6,a7). */
        if ((op & 0xF1F8u) == 0x5088u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, an = op & 7u;
            st->a[an] = st->a[an] + imm; pc += 2; continue;
        }
        /* [J5i] subq.l #imm,An : 5188 | q<<9 | an  (SUBA form, no flags). */
        if ((op & 0xF1F8u) == 0x5188u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, an = op & 7u;
            st->a[an] = st->a[an] - imm; pc += 2; continue;
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
            else if (src_mode == 6) {                 /* (d8,An,Xn) with 68020 scale */
                uint16_t brief = be16(ext); ext += 2;
                int8_t d8 = (int8_t)(brief & 0xFFu);
                unsigned ix = (brief >> 12) & 7u;
                unsigned scale = (brief >> 9) & 3u;
                uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index; /* W index */
                index <<= scale;
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
            else if (src_mode == 7 && src_reg == 4) {  /* #imm32 */
                srcval = be32(ext); ext += 4;
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
                else if (dst_mode == 6) {             /* (d8,An,Xn) with 68020 scale */
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    unsigned scale = (brief >> 9) & 3u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= scale;
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

        /* ============================ [J5l] movem (move-multiple) ===================
         * The opcode every compiler-generated 68k function uses in its prologue/epilogue.
         * Encoding: 0100 1d00 1s mmm rrr  where d=dir (0 reg->mem store, 1 mem->reg load),
         * s=size (0 .w, 1 .l), mmm/rrr = the destination/source EA. A register-list MASK
         * word follows the opcode; then the EA's own extension words.
         *
         * THE TWO MASK ORDERS (the load-bearing subtlety, M68000 PRM "MOVEM"):
         *   - PREDECREMENT store -(An) (mode 4): the mask is REVERSED — bit0=A7, bit1=A6,
         *     ..., bit7=A0, bit8=D7, ..., bit15=D0. Registers are stored high-to-low toward
         *     DECREASING addresses (An is predecremented by `size` before each store).
         *   - ALL OTHER modes (control store, every load incl. (An)+): the mask is NORMAL —
         *     bit0=D0, ..., bit7=D7, bit8=A0, ..., bit15=A7. Transfer ascends from the EA.
         * (An)+ postincrement bumps An by size*count AFTER; -(An) leaves An at the lowest slot.
         * .w LOADS sign-extend word->long into the full 32-bit register; .w STORES write the
         * low 16 bits. movem does NOT affect the condition codes.
         * We drive the SAME modes the JIT does (the engine drives Emu68's REAL EMIT_MOVEM);
         * this is the independent oracle that makes the byte-exact check complete. */
        /* NOTE the mode>=2 guard: mode 0 (Dn direct) of this pattern is ext.w/ext.l
         * (0x4880|dn / 0x48C0|dn), handled below — movem requires a memory/control EA. */
        if ((op & 0xFB80u) == 0x4880u && ((op >> 3) & 7u) >= 2u) {  /* movem reg<->mem */
            unsigned dir  = (op >> 10) & 1u;          /* 0 store (reg->mem), 1 load (mem->reg) */
            unsigned size = (op >> 6) & 1u;           /* 0 .w, 1 .l                            */
            unsigned mode = (op >> 3) & 7u;
            unsigned ereg =  op       & 7u;
            unsigned step = size ? 4u : 2u;
            uint16_t mask = be16(ip + 2);
            const uint8_t *ext = ip + 4;              /* EA extension words follow the mask    */
            int oob = 0;
            unsigned popcount = 0;
            for (unsigned i = 0; i < 16; i++) if (mask & (1u << i)) popcount++;

            /* helper: read/write one 68k register file slot by index 0..15 (D0..D7, A0..A7) */
            #define MV_RDREG(idx) ((idx) < 8 ? st->d[(idx)] : st->a[(idx) - 8])
            #define MV_WRREG(idx, v) do { if ((idx) < 8) st->d[(idx)] = (v); else st->a[(idx) - 8] = (v); } while (0)

            if (dir == 0) {
                /* ---- STORE: registers -> memory ---- */
                if (mode == 4) {
                    /* PREDECREMENT -(An), REVERSED mask. Iterate i=0..15 => reg index
                     * (15 - i) in normal order: bit0 maps to reg 15 (A7), bit15 to reg 0
                     * (D0). For each set bit predecrement An then store the mapped register. */
                    uint32_t an = st->a[ereg];
                    for (unsigned i = 0; i < 16 && !oob; i++) {
                        if (!(mask & (1u << i))) continue;
                        unsigned ridx = 15u - i;                 /* reversed: A7..A0,D7..D0 */
                        an -= step;
                        if (size) mem_wr32(sb, an, MV_RDREG(ridx), &oob);
                        else      mem_wr16(sb, an, (uint16_t)MV_RDREG(ridx), &oob);
                    }
                    if (!oob) st->a[ereg] = an;                  /* An ends at the lowest slot */
                } else {
                    /* CONTROL store ((An) / (d16,An) / abs), NORMAL mask, ascending. */
                    uint32_t addr; int handled = 1;
                    if (mode == 2) { addr = st->a[ereg]; }
                    else if (mode == 5) { int16_t d16 = be16s(ext); ext += 2; addr = st->a[ereg] + (uint32_t)(int32_t)d16; }
                    else if (mode == 7 && ereg == 0) { addr = (uint32_t)(int32_t)be16s(ext); ext += 2; }
                    else if (mode == 7 && ereg == 1) { addr = be32(ext); ext += 4; }
                    else { handled = 0; addr = 0; }
                    if (!handled) IFAIL("movem: unsupported store EA mode");
                    for (unsigned ridx = 0; ridx < 16 && !oob; ridx++) {
                        if (!(mask & (1u << ridx))) continue;
                        if (size) mem_wr32(sb, addr, MV_RDREG(ridx), &oob);
                        else      mem_wr16(sb, addr, (uint16_t)MV_RDREG(ridx), &oob);
                        addr += step;
                    }
                }
            } else {
                /* ---- LOAD: memory -> registers, NORMAL mask, ascending ---- */
                uint32_t addr; int handled = 1; int postinc = 0;
                if (mode == 3) { addr = st->a[ereg]; postinc = 1; }       /* (An)+ */
                else if (mode == 2) { addr = st->a[ereg]; }               /* (An)  */
                else if (mode == 5) { int16_t d16 = be16s(ext); ext += 2; addr = st->a[ereg] + (uint32_t)(int32_t)d16; }
                else if (mode == 7 && ereg == 0) { addr = (uint32_t)(int32_t)be16s(ext); ext += 2; }
                else if (mode == 7 && ereg == 1) { addr = be32(ext); ext += 4; }
                else if (mode == 7 && ereg == 2) {                        /* (d16,PC) */
                    uint32_t pcbase = pc + (uint32_t)(ext - ip);
                    int16_t d16 = be16s(ext); ext += 2; addr = pcbase + (uint32_t)(int32_t)d16;
                }
                else { handled = 0; addr = 0; }
                if (!handled) IFAIL("movem: unsupported load EA mode");
                for (unsigned ridx = 0; ridx < 16 && !oob; ridx++) {
                    if (!(mask & (1u << ridx))) continue;
                    if (size) {
                        uint32_t v = mem_rd32(sb, addr, &oob);
                        if (!oob) MV_WRREG(ridx, v);
                    } else {
                        uint16_t w = mem_rd16(sb, addr, &oob);
                        if (!oob) MV_WRREG(ridx, (uint32_t)(int32_t)(int16_t)w);  /* sign-extend */
                    }
                    addr += step;
                }
                if (!oob && postinc) st->a[ereg] += step * popcount;      /* An += size*count */
            }
            #undef MV_RDREG
            #undef MV_WRREG
            if (oob) IFAIL("movem: EA access out of sandbox");
            pc += (uint32_t)(ext - ip); continue;
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
        /* ============================ [J5h] negx Dn (register-direct) ================
         * 0100 0000 ss 000 rrr : Dn = 0 - Dn - X. ss=size (00 b,01 w,10 l). X=C=borrow;
         * V from the sized overflow; multi-precision Z (cleared if nonzero, else kept).
         * Matches Emu68's REAL EMIT_NEGX register .l/.w/.b paths byte-exact.
         * mask 0xFF38: high byte == 0x40 (op field 0000) + mode bits 5-3 == 000; any size. */
        if ((op & 0xFF38u) == 0x4000u) {
            unsigned dn = op & 7u, sz = (op >> 6) & 3u;
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            uint32_t xin = (st->ccr & J5D_CCR_X) ? 1u : 0u;
            uint32_t a = st->d[dn] & mask;
            uint64_t diff = (uint64_t)0 - a - xin;
            uint32_t res = (uint32_t)diff & mask;
            uint32_t msb = 1u << (bits - 1);
            int borrow = (a + xin) != 0;                  /* borrow out of 0 - a - X */
            int overflow = (a & res & msb) != 0;          /* 0-a-X overflow: both src&res neg */
            st->d[dn] = (st->d[dn] & ~mask) | res;
            st->ccr = sized_x_ccr(res, bits, borrow, overflow, st->ccr);
            pc += 2; continue;
        }

        /* neg.b/.w/.l Dn : 0100 0100 ss 000 rrr  (Dn = 0 - Dn). NOT a multi-precision op:
         * Z is set normally (cleared if nonzero, SET if zero). X=C=(result!=0); V overflow.
         * [J5h] generalised to all sizes (was .l-only); mask 0xFF38 -> high byte 0x44, mode
         * 000. Matches Emu68's REAL EMIT_NEG register paths byte-exact. */
        if ((op & 0xFF38u) == 0x4400u) {
            unsigned dn = op & 7u, sz = (op >> 6) & 3u;
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            uint32_t msb = 1u << (bits - 1);
            uint32_t a = st->d[dn] & mask;
            uint32_t res = (0u - a) & mask;
            uint32_t cc = 0;
            if (a != 0)   cc |= J5D_CCR_C | J5D_CCR_X;       /* borrow out of 0-a */
            if (a == msb) cc |= J5D_CCR_V;                    /* overflow: only 0x80.. */
            if (res == 0) cc |= J5D_CCR_Z;
            if (res & msb) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & ~mask) | res;
            st->ccr = cc;
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

        /* ============================ [J5m] adda.w/suba.w #imm,An ===================
         * 1001/1101 AAA 011 111 100 : sub/adda WORD-immediate to An. The .w source is
         * SIGN-EXTENDED to 32 bits, then added/subtracted to the full 32-bit An. ADDA/SUBA
         * affect NO condition codes (PRM ADDA/SUBA). 6 bytes (opcode + word imm).
         *   suba.w #imm,An : 1001 AAA 011 111 100 = 0x90FC | An<<9
         *   adda.w #imm,An : 1101 AAA 011 111 100 = 0xD0FC | An<<9 */
        if ((op & 0xF1FFu) == 0x90FCu) {                 /* suba.w #imm,An */
            unsigned an = (op >> 9) & 7u;
            uint32_t s = (uint32_t)(int32_t)be16s(ip + 2);
            st->a[an] = st->a[an] - s; pc += 4; continue;
        }
        if ((op & 0xF1FFu) == 0xD0FCu) {                 /* adda.w #imm,An */
            unsigned an = (op >> 9) & 7u;
            uint32_t s = (uint32_t)(int32_t)be16s(ip + 2);
            st->a[an] = st->a[an] + s; pc += 4; continue;
        }
        /* adda.l/suba.l #imm,An : opmode 111 with .l-immediate (mode 7 reg 4). 8 bytes. */
        if ((op & 0xF1FFu) == 0x91FCu) {                 /* suba.l #imm,An */
            unsigned an = (op >> 9) & 7u;
            st->a[an] = st->a[an] - be32(ip + 2); pc += 6; continue;
        }
        if ((op & 0xF1FFu) == 0xD1FCu) {                 /* adda.l #imm,An */
            unsigned an = (op >> 9) & 7u;
            st->a[an] = st->a[an] + be32(ip + 2); pc += 6; continue;
        }
        /* adda.w/suba.w/adda.l/suba.l An/Dn,An (register source). opmode 011=.w(add to An),
         * 111=.l. No CCR. .w source sign-extended to 32.
         *   adda.w <ea>,An : 1101 AAA 011 mmm rrr ; adda.l : 1101 AAA 111 mmm rrr
         *   suba.w <ea>,An : 1001 AAA 011 mmm rrr ; suba.l : 1001 AAA 111 mmm rrr
         * Source EA = Dn (mode 0) or An (mode 1). */
        if ((op & 0xF1C0u) == 0xD0C0u || (op & 0xF1C0u) == 0xD1C0u ||
            (op & 0xF1C0u) == 0x90C0u || (op & 0xF1C0u) == 0x91C0u) {
            unsigned an = (op >> 9) & 7u;
            unsigned is_sub = ((op & 0xF000u) == 0x9000u);
            unsigned islong = (((op >> 6) & 7u) == 7u);       /* opmode 111 = .l (else 011 = .w) */
            unsigned sm = (op >> 3) & 7u, sr = op & 7u;
            uint32_t s;
            if (sm == 0) s = st->d[sr];
            else if (sm == 1) s = st->a[sr];
            else IFAIL("adda/suba: unsupported source EA in oracle");
            if (!islong) s = (uint32_t)(int32_t)(int16_t)s;   /* .w sign-extend */
            st->a[an] = is_sub ? (st->a[an] - s) : (st->a[an] + s);
            pc += 2; continue;
        }

        /* ============================ [J5m] addq.w/subq.w #d,An & #d,Dn =============
         * Quick word forms. To An: full 32-bit add/sub (per ADDQ-to-An), NO CCR. To Dn:
         * word op on the low 16 (rest preserved), sets CCR like add/sub on the word size.
         *   addq.w #d,An : 0101 ddd 0 01 001 AAA = 0x5048 | d<<9 | An
         *   subq.w #d,An : 0101 ddd 1 01 001 AAA = 0x5148 | d<<9 | An
         *   addq.w #d,Dn : 0101 ddd 0 01 000 DDD = 0x5040 | d<<9 | Dn
         *   subq.w #d,Dn : 0101 ddd 1 01 000 DDD = 0x5140 | d<<9 | Dn */
        if ((op & 0xF1F8u) == 0x5048u) {                 /* addq.w #d,An */
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, an = op & 7u;
            st->a[an] = st->a[an] + imm; pc += 2; continue;
        }
        if ((op & 0xF1F8u) == 0x5148u) {                 /* subq.w #d,An */
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, an = op & 7u;
            st->a[an] = st->a[an] - imm; pc += 2; continue;
        }
        if ((op & 0xF1F8u) == 0x5040u) {                 /* addq.w #d,Dn */
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            uint16_t a = (uint16_t)st->d[dn]; uint32_t s = (uint32_t)a + imm;
            uint16_t res = (uint16_t)s; uint32_t cc = 0;
            if (s & 0x10000u) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (imm ^ res)) & 0x8000u) cc |= J5D_CCR_V;
            if (res == 0) cc |= J5D_CCR_Z;
            if (res & 0x8000u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | res; st->ccr = cc;
            pc += 2; continue;
        }
        if ((op & 0xF1F8u) == 0x5140u) {                 /* subq.w #d,Dn */
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            uint16_t a = (uint16_t)st->d[dn]; uint16_t res = (uint16_t)(a - imm);
            uint32_t cc = 0;
            if (imm > a) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ imm) & (a ^ res)) & 0x8000u) cc |= J5D_CCR_V;
            if (res == 0) cc |= J5D_CCR_Z;
            if (res & 0x8000u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | res; st->ccr = cc;
            pc += 2; continue;
        }

        /* ============================ [J5m] extb.l Dn ===============================
         * 0100 1001 11 000 rrr = 0x49C0 | Dn : sign-extend BYTE->LONG. N,Z from result;
         * V=C=0; X kept. (68020 EXTB.L.) */
        if ((op & 0xFFF8u) == 0x49C0u) {
            unsigned dn = op & 7u;
            uint32_t res = (uint32_t)(int32_t)(int8_t)(st->d[dn] & 0xFFu);
            st->d[dn] = res; st->ccr = logic_ccr(res, st->ccr);
            pc += 2; continue;
        }

        /* ============================ [J5m] tst.b / tst.w / tst.l with EA ===========
         * tst <ea> : 0100 1010 ss mmm rrr. ss=00 .b, 01 .w, 10 .l. N,Z from operand at
         * its size; V=C=0; X kept. EA = Dn(0), An(1, .l only — tst.l An), (An)(2). */
        if ((op & 0xFF00u) == 0x4A00u && ((op >> 6) & 3u) != 3u) {
            unsigned sz = (op >> 6) & 3u, mode = (op >> 3) & 7u, reg = op & 7u;
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            uint32_t v; int oob = 0; uint32_t adv = 2;
            if (mode == 0) v = st->d[reg];
            else if (mode == 1) v = st->a[reg];                 /* tst An (32-bit) */
            else if (mode == 2) {                               /* (An) */
                if (bits == 8)       v = mem_rd8(sb, st->a[reg], &oob);
                else if (bits == 16) v = mem_rd16(sb, st->a[reg], &oob);
                else                 v = mem_rd32(sb, st->a[reg], &oob);
            }
            else if (mode == 3) {                               /* (An)+ */
                if (bits == 8)       { v = mem_rd8(sb, st->a[reg], &oob);  if (!oob) st->a[reg] += 1u; }
                else if (bits == 16) { v = mem_rd16(sb, st->a[reg], &oob); if (!oob) st->a[reg] += 2u; }
                else                 { v = mem_rd32(sb, st->a[reg], &oob); if (!oob) st->a[reg] += 4u; }
            }
            else IFAIL("tst: unsupported EA in oracle");
            if (oob) IFAIL("tst: EA access out of sandbox");
            uint32_t masked = (bits == 32) ? v : (v & ((1u << bits) - 1u));
            uint32_t msb = 1u << (bits - 1);
            uint32_t ccr = st->ccr & J5D_CCR_X;
            if (masked == 0)   ccr |= J5D_CCR_Z;
            if (masked & msb)  ccr |= J5D_CCR_N;
            st->ccr = ccr; pc += adv; continue;
        }

        /* ============================ [J5m] pea <ea> ================================
         * 0100 1000 01 mmm rrr = 0x4840 | EA : compute EA, push the 32-bit address (a7-=4),
         * big-endian. NO CCR. EA = abs.l (7.1), (d16,An) (5), (d16,PC) (7.2), (An) (2). */
        if ((op & 0xFFC0u) == 0x4840u) {
            unsigned mode = (op >> 3) & 7u, reg = op & 7u;
            const uint8_t *ext = ip + 2; uint32_t ea; int handled = 1;
            if (mode == 2) { ea = st->a[reg]; }
            else if (mode == 5) { int16_t d16 = be16s(ext); ext += 2; ea = st->a[reg] + (uint32_t)(int32_t)d16; }
            else if (mode == 7 && reg == 0) { ea = (uint32_t)(int32_t)be16s(ext); ext += 2; }
            else if (mode == 7 && reg == 1) { ea = be32(ext); ext += 4; }
            else if (mode == 7 && reg == 2) {                   /* (d16,PC) */
                uint32_t pcbase = pc + (uint32_t)(ext - ip);
                int16_t d16 = be16s(ext); ext += 2; ea = pcbase + (uint32_t)(int32_t)d16;
            }
            else { handled = 0; ea = 0; }
            if (handled) {
                uint32_t sp = st->a[7] - 4u; int oob = 0;
                mem_wr32(sb, sp, ea, &oob);
                if (oob) IFAIL("pea: push out of sandbox");
                st->a[7] = sp; pc += (uint32_t)(ext - ip); continue;
            }
        }

        /* ============================ [J5m] cmp/cmpa.l with EA source ===============
         * cmp.l <ea>,Dn : 1011 DDD 010 mmm rrr (opmode 010 = .l, Dn dest).
         * cmpa.l <ea>,An : 1011 AAA 111 mmm rrr (opmode 111 = cmpa.l, An dest).
         * Compares dest - source; sets N,Z,V,C from the 32-bit subtraction; X kept.
         * EA = Dn(0), An(1), (d8,An,Xn) with scale (6). */
        if ((op & 0xF1C0u) == 0xB080u || (op & 0xF1C0u) == 0xB1C0u) {
            unsigned is_a = ((op & 0x00C0u) == 0x00C0u);        /* opmode 111 = cmpa.l */
            unsigned dst = (op >> 9) & 7u;
            unsigned sm = (op >> 3) & 7u, sr = op & 7u;
            const uint8_t *ext = ip + 2; uint32_t b; int oob = 0; int handled = 1;
            if (sm == 0) b = st->d[sr];
            else if (sm == 1) b = st->a[sr];
            else if (sm == 6) {                                 /* (d8,An,Xn) scaled */
                uint16_t brief = be16(ext); ext += 2;
                int8_t d8 = (int8_t)(brief & 0xFFu);
                unsigned ix = (brief >> 12) & 7u;
                unsigned scale = (brief >> 9) & 3u;
                uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                index <<= scale;
                uint32_t addr = st->a[sr] + (uint32_t)(int32_t)d8 + index;
                b = mem_rd32(sb, addr, &oob);
            }
            else { handled = 0; b = 0; }
            if (!handled) IFAIL("cmp.l: unsupported source EA in oracle");
            if (oob) IFAIL("cmp.l: EA access out of sandbox");
            uint32_t a = is_a ? st->a[dst] : st->d[dst];
            uint32_t res = a - b; uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a) cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            if (res == 0)          cc |= J5D_CCR_Z;
            if (res & 0x80000000u) cc |= J5D_CCR_N;
            st->ccr = cc; pc += (uint32_t)(ext - ip); continue;
        }

        /* ============================ [J5m] add.b Dm,Dn (byte add, reg->reg) ========
         * 1101 DDD 000 000 SSS = 0xD000 | Dn<<9 | Dm : Dn.b = Dn.b + Dm.b. Sets full CCR
         * on the byte; only the low byte of Dn changes. */
        if ((op & 0xF1F8u) == 0xD000u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint8_t a = (uint8_t)st->d[dn], b = (uint8_t)st->d[dm];
            uint32_t s = (uint32_t)a + b; uint8_t res = (uint8_t)s; uint32_t cc = 0;
            if (s & 0x100u) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80u) cc |= J5D_CCR_V;
            if (res == 0) cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res; st->ccr = cc;
            pc += 2; continue;
        }

        /* ============================ [J5m] 68020 MULU.L/MULS.L <ea>,Dl =============
         * opcode 0x4C00 | EA ; extension word: bit15=0, 14-12 Dl, bit11 signed, bit10 size
         * (0 => 32-bit low product into Dl; 1 => 64-bit Dh:Dl, not used here), 2-0 Dh.
         * 32x32 -> low 32 product into Dl. CCR: N,Z from the 32-bit result; V=0 (for the
         * 32-bit form the product never overflows 32 bits by definition of "low 32"); C=0;
         * X kept. EA = Dn(0) or #imm(7.4). */
        if ((op & 0xFFC0u) == 0x4C00u) {
            unsigned mode = (op >> 3) & 7u, reg = op & 7u;
            uint16_t ext = be16(ip + 2);
            unsigned dl = (ext >> 12) & 7u;
            unsigned is_signed = (ext >> 11) & 1u;
            unsigned size64 = (ext >> 10) & 1u;
            uint32_t src; uint32_t adv;
            if (mode == 0)                  { src = st->d[reg]; adv = 4; }
            else if (mode == 7 && reg == 4) { src = be32(ip + 4); adv = 8; }
            else IFAIL("mul.l: unsupported source EA in oracle");
            if (size64) IFAIL("mul.l: 64-bit product form not in subset");
            uint32_t a = st->d[dl];
            uint32_t res;
            if (is_signed) res = (uint32_t)((int32_t)a * (int32_t)src);
            else           res = a * src;
            st->d[dl] = res; st->ccr = logic_ccr(res, st->ccr);   /* N,Z; V=C=0; X kept */
            pc += adv; continue;
        }

        /* ============================ [J5m] 68020 DIVU.L/DIVS.L/DIVxL.L <ea> ========
         * opcode 0x4C40 | EA ; extension word: bit15=0, 14-12 Dq, bit11 signed, bit10 size
         * (0 => 32-bit dividend in Dq, 1 => 64-bit Dr:Dq dividend — not used here), 2-0 Dr.
         *   size=0, Dr==Dq : 32/32 -> quotient in Dq (remainder discarded) [divu.l/divs.l].
         *   size=0, Dr!=Dq : 32/32 -> quotient in Dq, remainder in Dr [divul.l/divsl.l].
         * CCR: N,Z from the quotient; V on overflow (and result unchanged); C=0; X kept.
         * EA = Dn(0) or #imm(7.4). */
        if ((op & 0xFFC0u) == 0x4C40u) {
            unsigned mode = (op >> 3) & 7u, reg = op & 7u;
            uint16_t ext = be16(ip + 2);
            unsigned dq = (ext >> 12) & 7u;
            unsigned is_signed = (ext >> 11) & 1u;
            unsigned size64 = (ext >> 10) & 1u;
            unsigned dr = ext & 7u;
            uint32_t divisor; uint32_t adv;
            if (mode == 0)                  { divisor = st->d[reg]; adv = 4; }
            else if (mode == 7 && reg == 4) { divisor = be32(ip + 4); adv = 8; }
            else IFAIL("div.l: unsupported source EA in oracle");
            if (size64) IFAIL("div.l: 64-bit dividend form not in subset");
            if (divisor == 0) {
                uint32_t h;
                if (iraise(sb, st, J5I_VEC_DIV_BY_ZERO, pc + adv, &h, errbuf, errlen)) return 1;
                pc = h; continue;
            }
            uint32_t dividend = st->d[dq], quot, rem;
            if (is_signed) {
                quot = (uint32_t)((int32_t)dividend / (int32_t)divisor);
                rem  = (uint32_t)((int32_t)dividend % (int32_t)divisor);
            } else { quot = dividend / divisor; rem = dividend % divisor; }
            /* 32/32 never overflows the 32-bit quotient; V always 0 here. */
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (quot == 0)          cc |= J5D_CCR_Z;
            if (quot & 0x80000000u) cc |= J5D_CCR_N;
            st->d[dq] = quot;
            if (dr != dq) st->d[dr] = rem;            /* divul/divsl: remainder in Dr */
            st->ccr = cc; pc += adv; continue;
        }

        /* ============================ [J5m] LINEE word/byte shifts (lsl.w/lsr.w etc.) ====
         * Generalise the LINEE static-count register shift to .b (size 00) and .w (size 01).
         * op = 1110 ccc d ii s tt rrr ; ii=size, s=0 (immediate count). We cover the types
         * the corpus uses on smaller sizes (LSL.W). Sets C/X from last-bit-out, N/Z from the
         * sized result, V=0. */
        if ((op & 0xF000u) == 0xE000u && ((op >> 5) & 1u) == 0u && ((op >> 6) & 3u) != 2u) {
            unsigned sz = (op >> 6) & 3u;                       /* 0 .b, 1 .w (2 .l handled above) */
            if (sz != 3u) {                                     /* sz==3 is mem-shift, not this */
                unsigned dn = op & 7u;
                unsigned cnt = (op >> 9) & 7u; if (cnt == 0) cnt = 8;
                unsigned dir = (op >> 8) & 1u;                 /* 1 = left */
                unsigned type = (op >> 3) & 3u;                /* 0 as,1 ls,2 rox,3 ro */
                unsigned bits = (sz == 0) ? 8u : 16u;
                uint32_t fullmask = (1u << bits) - 1u;
                uint32_t a = st->d[dn] & fullmask, res = a;
                uint32_t msb = 1u << (bits - 1);
                unsigned last_out = 0; uint32_t cc;
                if (type == 1) {                               /* LSL/LSR */
                    if (dir) { last_out = (cnt <= bits) ? ((a >> (bits - cnt)) & 1u) : 0u; res = (a << cnt) & fullmask; }
                    else     { last_out = (a >> (cnt - 1)) & 1u; res = a >> cnt; }
                    cc = 0;
                    if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                    if (res == 0) cc |= J5D_CCR_Z;
                    if (res & msb) cc |= J5D_CCR_N;
                    st->d[dn] = (st->d[dn] & ~fullmask) | res; st->ccr = cc;
                    pc += 2; continue;
                } else if (type == 0) {                        /* ASL/ASR */
                    if (dir) {
                        last_out = (cnt <= bits) ? ((a >> (bits - cnt)) & 1u) : 0u; res = (a << cnt) & fullmask;
                        uint32_t signbits = (fullmask << (bits - 1 - cnt)) & fullmask;
                        uint32_t top = a & signbits;
                        int vbit = !(top == 0 || top == signbits);
                        cc = 0;
                        if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                        if (vbit) cc |= J5D_CCR_V;
                    } else {
                        last_out = (a >> (cnt - 1)) & 1u;
                        int32_t sa = (int32_t)(a << (32 - bits)) >> (32 - bits);  /* sign-extend */
                        res = ((uint32_t)(sa >> cnt)) & fullmask;
                        cc = 0;
                        if (last_out) cc |= J5D_CCR_C | J5D_CCR_X;
                    }
                    if (res == 0) cc |= J5D_CCR_Z;
                    if (res & msb) cc |= J5D_CCR_N;
                    st->d[dn] = (st->d[dn] & ~fullmask) | res; st->ccr = cc;
                    pc += 2; continue;
                } else if (type == 3) {                        /* ROL/ROR (no X) */
                    unsigned r = cnt % bits;
                    if (dir) { res = ((a << r) | (a >> ((bits - r) % bits))) & fullmask; last_out = res & 1u; }
                    else     { res = ((a >> r) | (a << ((bits - r) % bits))) & fullmask; last_out = (res >> (bits - 1)) & 1u; }
                    cc = st->ccr & J5D_CCR_X;
                    if (last_out) cc |= J5D_CCR_C;
                    if (res == 0) cc |= J5D_CCR_Z;
                    if (res & msb) cc |= J5D_CCR_N;
                    st->d[dn] = (st->d[dn] & ~fullmask) | res; st->ccr = cc;
                    pc += 2; continue;
                }
            }
        }

        /* ============================ [J5m] move.b with EA src/dst ==================
         * move.b <ea>,<ea> : 0001 DDD ddd sss rrr (size field 01). Byte moves: to Dn only
         * the low byte changes; to memory one byte; sets N/Z from the byte, V=C=0, X kept
         * (movea has no byte form). Source EA: Dn(0), (An)(2), (An)+(3); dest EA: Dn(0),
         * (An)(2), (d8,An,Xn) scaled (6). */
        if ((op & 0xF000u) == 0x1000u) {
            unsigned dst_reg  = (op >> 9) & 7u;
            unsigned dst_mode = (op >> 6) & 7u;
            unsigned src_mode = (op >> 3) & 7u;
            unsigned src_reg  =  op       & 7u;
            const uint8_t *ext = ip + 2;
            int oob = 0; uint8_t srcb; int handled = 1;
            if (src_mode == 0) srcb = (uint8_t)st->d[src_reg];
            else if (src_mode == 2) srcb = mem_rd8(sb, st->a[src_reg], &oob);
            else if (src_mode == 3) { srcb = mem_rd8(sb, st->a[src_reg], &oob); if (!oob) st->a[src_reg] += 1u; }
            else if (src_mode == 5) { int16_t d16 = be16s(ext); ext += 2;
                                      srcb = mem_rd8(sb, st->a[src_reg] + (uint32_t)(int32_t)d16, &oob); }
            else if (src_mode == 7 && src_reg == 1) { uint32_t addr = be32(ext); ext += 4;
                                      srcb = mem_rd8(sb, addr, &oob); }
            else if (src_mode == 7 && src_reg == 4) { srcb = ext[1]; ext += 2; }  /* #imm byte (low byte of word) */
            else { handled = 0; srcb = 0; }
            if (handled && !oob) {
                if (dst_mode == 0) {
                    st->d[dst_reg] = (st->d[dst_reg] & 0xFFFFFF00u) | srcb;
                    uint32_t ccr = st->ccr & J5D_CCR_X;
                    if (srcb == 0) ccr |= J5D_CCR_Z;
                    if (srcb & 0x80u) ccr |= J5D_CCR_N;
                    st->ccr = ccr;
                }
                else if (dst_mode == 2) { mem_wr8(sb, st->a[dst_reg], srcb, &oob); }
                else if (dst_mode == 3) { mem_wr8(sb, st->a[dst_reg], srcb, &oob); if (!oob) st->a[dst_reg] += 1u; }
                else if (dst_mode == 5) { int16_t d16 = be16s(ext); ext += 2;
                                          mem_wr8(sb, st->a[dst_reg] + (uint32_t)(int32_t)d16, srcb, &oob); }
                else if (dst_mode == 6) {                       /* (d8,An,Xn) scaled */
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    unsigned scale = (brief >> 9) & 3u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= scale;
                    uint32_t addr = st->a[dst_reg] + (uint32_t)(int32_t)d8 + index;
                    mem_wr8(sb, addr, srcb, &oob);
                }
                else if (dst_mode == 7 && dst_reg == 1) { uint32_t addr = be32(ext); ext += 4;
                                          mem_wr8(sb, addr, srcb, &oob); }
                else handled = 0;
                /* move.b to memory ALSO sets N/Z/V/C (movea has no byte form). */
                if (handled && dst_mode != 0) {
                    uint32_t ccr = st->ccr & J5D_CCR_X;
                    if (srcb == 0) ccr |= J5D_CCR_Z;
                    if (srcb & 0x80u) ccr |= J5D_CCR_N;
                    st->ccr = ccr;
                }
            }
            if (handled) {
                if (oob) IFAIL("move.b EA access out of sandbox");
                pc += (uint32_t)(ext - ip); continue;
            }
        }

        IFAIL("interp: out-of-subset opcode");
    }
#undef IFAIL
}
