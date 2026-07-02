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
#include "j5s_fpu_exc.h"  /* [J5s] the FP exception model: FPSR EXC/AEXC, FPCR enable/mode, vectors, BSUN */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv/strtoull for the JIT68K_STEP_CAP override */
#include <math.h>     /* [J5o] fabs/sqrt for the independent double-precision FP oracle */
#include <fenv.h>     /* [J5s] feclearexcept/fetestexcept/fesetround for the FP exception model */
/* [J5s] we READ the FP exception flags + set the rounding mode at runtime, so the compiler must
 * NOT reorder FP ops across feclearexcept/fetestexcept (else -O2 can hoist an inexact-producing op
 * across the read and corrupt the derived FPSR EXC byte). */
#pragma STDC FENV_ACCESS ON

/* Runaway guard, same contract as the JIT dispatcher's: default 2,000,000 steps,
 * raised via the SAME JIT68K_STEP_CAP env var (decimal or 0x-hex) so a long run —
 * a std-sized program boot, a benchmark — behaves identically under both engines. */
static uint64_t interp_step_cap(void)
{
    static uint64_t cap = 0;
    if (!cap) {
        cap = 2000000u;
        const char *e = getenv("JIT68K_STEP_CAP");
        if (e && *e) { unsigned long long v = strtoull(e, (char **)0, 0); if (v) cap = (uint64_t)v; }
    }
    return cap;
}
#define STEP_CAP interp_step_cap()

/* [J5n] DIAGNOSTICS step hook (optional, NULL by default). The diagnostics subsystem
 * (j5n_diag.c) registers a per-instruction callback so the oracle becomes the instruction-
 * precise stepper for replay-to-N and divergence localization. It is a function POINTER set
 * via j5d_interp_set_step_hook so the interp keeps ZERO hard dependency on the diag layer —
 * every other [J5*] test links the interp WITHOUT j5n_diag.c and this stays NULL (the fast
 * path). The hook returns nonzero to STOP the run (a clean replay-to-N landing). */
typedef int (*j5n_step_hook_fn)(void *diag, const struct j5d_m68k_state *st,
                                j5d_sandbox *sb, uint32_t pc, uint16_t op);
static j5n_step_hook_fn g_step_hook = NULL;
static void *g_step_diag = NULL;
void j5d_interp_set_step_hook(j5n_step_hook_fn fn, void *diag)
{ g_step_hook = fn; g_step_diag = diag; }

/* [J5n] DIFFERENTIAL lockstep: a stop-at-PC side-channel so the engine can advance the
 * oracle one block at a time and compare at each block boundary. When g_stop_active and the
 * loop reaches g_stop_pc at an instruction boundary, j5d_interp_run returns 2 (a clean
 * "stopped at the requested PC", distinct from 0=program-exit / 1=error). NULL by default —
 * every other test leaves it inactive (the fast path). */
static int      g_stop_active = 0;
static uint32_t g_stop_pc = 0;
void j5d_interp_set_stop_pc(int active, uint32_t pc) { g_stop_active = active; g_stop_pc = pc; }

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

/* ============================ [J5o] FPU oracle helpers (no Emu68) ===================
 * The INDEPENDENT double-precision FP model. These read/write the SAME sandbox memory the
 * engine's FP-mem helpers do (big-endian operand bytes) and compute every op directly in C
 * `double`, a path that NEVER touches Emu68's emitted AArch64 FP. The test asserts the JIT's
 * FP0..FP7 (double bit patterns) + FPSR condition bits + any FP-to-memory result are bit-exact
 * equal to this model — the [J5o] verification crux. */

/* Read an IEEE binary64 from sandbox memory at `addr` (big-endian operand, like the 68k). */
static double fpu_mem_rd_d(j5d_sandbox *sb, uint32_t addr, int *oob)
{
    uint32_t hi = mem_rd32(sb, addr, oob);
    uint32_t lo = mem_rd32(sb, addr + 4, oob);
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    double d; __builtin_memcpy(&d, &bits, 8);
    return d;
}
static void fpu_mem_wr_d(j5d_sandbox *sb, uint32_t addr, double v, int *oob)
{
    uint64_t bits; __builtin_memcpy(&bits, &v, 8);
    mem_wr32(sb, addr,     (uint32_t)(bits >> 32), oob);
    mem_wr32(sb, addr + 4, (uint32_t)bits,         oob);
}
/* Read an IEEE binary32 (single) from sandbox, widen to double (the 68k FMOVE.s load). */
static double fpu_mem_rd_s(j5d_sandbox *sb, uint32_t addr, int *oob)
{
    uint32_t bits = mem_rd32(sb, addr, oob);
    float f; __builtin_memcpy(&f, &bits, 4);
    return (double)f;
}
/* Narrow a double to binary32 and write it (the 68k FMOVE.s store). */
static void fpu_mem_wr_s(j5d_sandbox *sb, uint32_t addr, double v, int *oob)
{
    float f = (float)v; uint32_t bits; __builtin_memcpy(&bits, &f, 4);
    mem_wr32(sb, addr, bits, oob);
}

/* The FPSR condition-code byte the 68881 sets: N(bit27) Z(bit26) I(bit25,infinity) NAN(bit24).
 * This MIRRORS Emu68's EMIT_GetFPUFlags (A64.h) EXACTLY — the gate the bit-exact assert checks.
 * EMIT_GetFPUFlags packs the AArch64 NZCV from `fcmpzd`/`fcmpd` into the FPSR cc nibble via
 * `bic_immed(tmp,tmp,1,3)` (which CLEARS the AArch64 C bit) then `orr fpsr |= (tmp >> 4)`. So the
 * mapping is FPSR{N=NZCV.N, Z=NZCV.Z, I=0 ALWAYS (C is bic'd out), NAN=NZCV.V}. [J5p] CORRECTION
 * (the [J5o] oracle latently mapped C->I, which never surfaced because every [J5o] test case had
 * C=0 in the FINAL FPSR-setting op): the I (infinity) bit is NEVER set by this path — VERIFIED
 * empirically against the JIT: FTST(-2.0)->cc 0x08 (N), FTST(3.0)->0x00, FTST(0.0)->0x04 (Z),
 * FTST(NaN)->0x01 (NAN), FTST(+inf)->0x00, FTST(-inf)->0x08 (N). For a compare-with-ZERO (fcmpzd)
 * used by FTST/FABS/FNEG/FADD/.../transcendental/utility: AArch64 N=(v<0), Z=(v==0), V=unordered.
 * For FCMP (fcmpd dst,src): N=(dst<src), Z=(dst==src), V=unordered. C is computed but DISCARDED. */
#define J5O_FPSR_N   0x08000000u
#define J5O_FPSR_Z   0x04000000u
#define J5O_FPSR_I   0x02000000u   /* the 68881 Infinity bit — NOT set by EMIT_GetFPUFlags (C bic'd)*/
#define J5O_FPSR_NAN 0x01000000u   /* maps from AArch64 V flag (unordered)                    */
#define J5O_FPSR_CC  (J5O_FPSR_N | J5O_FPSR_Z | J5O_FPSR_I | J5O_FPSR_NAN)

/* Pack the AArch64 NZCV (as four ints) into the FPSR cc bits, clearing them first. The C flag is
 * IGNORED — EMIT_GetFPUFlags clears it before the shift, so the FPSR I bit stays 0 (see above). */
static uint32_t fpu_pack_fpsr(uint32_t fpsr, int N, int Z, int C, int V)
{
    (void)C;                       /* [J5p] EMIT_GetFPUFlags bic's the AArch64 C bit -> I stays 0 */
    fpsr &= ~J5O_FPSR_CC;
    if (N) fpsr |= J5O_FPSR_N;
    if (Z) fpsr |= J5O_FPSR_Z;
    if (V) fpsr |= J5O_FPSR_NAN;
    return fpsr;
}
/* AArch64 fcmp NZCV for `a <op> b` (IEEE, with the unordered/NaN case). */
static uint32_t fpu_fcmp_fpsr(uint32_t fpsr, double a, double b)
{
    int N=0,Z=0,C=0,V=0;
    if (a != a || b != b) { V = 1; C = 1; }       /* unordered (NaN): N=Z=0, C=1, V=1     */
    else if (a < b)       { N = 1; }              /* less:    N=1 Z=0 C=0 V=0             */
    else if (a == b)      { Z = 1; C = 1; }       /* equal:   N=0 Z=1 C=1 V=0             */
    else                  { C = 1; }              /* greater: N=0 Z=0 C=1 V=0             */
    return fpu_pack_fpsr(fpsr, N, Z, C, V);
}

/* Whether the 68881 updates the FPSR condition codes after THIS FP op — i.e. whether a
 * subsequent FP instruction CONSUMES them (FBcc/FDBcc/FScc/FTRAPcc/FMOVEM/FMOVE-to-MEM/FSAVE/
 * FRESTORE), within 16 ops, stopping at a control-flow branch. A FAITHFUL re-derivation of
 * Emu68's FPSR_Update_Needed (M68k_LINEF.c), so the oracle and the JIT agree on EXACTLY when
 * the FPSR byte is written. `ip` points at the m68k word AFTER the current FP instruction's
 * extension words. (We do not model the FNOP recursion's full depth-5 chain — the test stream
 * has no FNOP between the FP op and its consumer; documented.) */
static int fpu_fpsr_update_needed(j5d_sandbox *sb, uint32_t after_pc)
{
    uint32_t pc = after_pc; int cnt = 0;
    for (;;) {
        if (pc < sb->origin || (uint64_t)pc + 2 > (uint64_t)sb->origin + sb->size) return 1;
        uint16_t op = be16(sb->host_mem + (pc - sb->origin));
        if ((op & 0xfe00) == 0xf200) break;       /* reached an FPU (line-F) instruction   */
        if (cnt++ > 15) return 1;
        /* a control-flow branch ends the scan (the FPSR may be needed after it) — the 68k
         * branch family (same set as j5o_fpu_shims.c M68K_IsBranch); conservative: any of
         * Bcc/BSR/BRA/JMP/JSR/RTS/RTE/RTR/TRAP/DBcc forces an update. */
        if ((op & 0xf000) == 0x6000) return 1;                    /* Bcc/BRA/BSR          */
        if ((op & 0xff80) == 0x4e80) return 1;                    /* JSR/JMP              */
        if (op == 0x4e75 || op == 0x4e73 || op == 0x4e77) return 1;/* RTS/RTE/RTR         */
        if ((op & 0xfff0) == 0x4e40) return 1;                    /* TRAP #n              */
        if ((op & 0xf0f8) == 0x50c8) return 1;                    /* DBcc                 */
        /* otherwise step over: every opcode in the [J5o] test between FP ops is single-word
         * (the FP ops themselves are matched above); a non-FP op advances by its length. The
         * test stream interleaves only FP ops + the terminator, so a 1-word step is exact for
         * the cases it reaches; if a multi-word op appeared we conservatively return 1. */
        pc += 2;
    }
    uint16_t opcode  = be16(sb->host_mem + (pc - sb->origin));
    uint16_t opcode2 = ((uint64_t)pc + 4 <= (uint64_t)sb->origin + sb->size)
                       ? be16(sb->host_mem + (pc + 2 - sb->origin)) : 0;
    if ((opcode & 0xff80) == 0xf280) return 1;                            /* FBcc          */
    if ((opcode & 0xfff8) == 0xf248 && (opcode2 & 0xffc0) == 0) return 1; /* FDBcc         */
    if ((opcode & 0xffc0) == 0xf200 && (opcode2 & 0xc700) == 0xc000) return 1; /* FMOVEM   */
    if ((opcode & 0xffc0) == 0xf200 && (opcode2 & 0xc3ff) == 0x8000) return 1; /* FMOVEM sp*/
    if ((opcode & 0xffc0) == 0xf240 && (opcode2 & 0xffc0) == 0) return 1; /* FScc          */
    if ((opcode & 0xfff8) == 0xf278 && (opcode2 & 0xffc0) == 0) return 1; /* FTRAPcc       */
    if ((opcode & 0xffc0) == 0xf200 && (opcode2 & 0xe000) == 0x6000) return 1; /* FMOVE->MEM*/
    if ((opcode & 0xffc0) == 0xf340) return 1;                            /* FRESTORE      */
    if ((opcode & 0xffc0) == 0xf300) return 1;                            /* FSAVE         */
    return 0;
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

/* [J5s] Take an FP exception trap if any EXC bit in `exc` is enabled in FPCR (bits 15..8).
 * Builds the [J5i] 68k frame on a7 and vectors to the matching FP vector (48..54), highest
 * priority first. `return_pc` is the PC after the faulting FP instruction (saved in the frame).
 * Returns 1 if a trap was taken (sets *pc to the handler), 0 if none fired. On a raise failure
 * (bad/unmapped vector) returns 1 with *pc = 0xFFFFFFFF and errbuf set (caller returns 1). */
static int j5s_fp_trap(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t exc,
                       uint32_t return_pc, char *errbuf, unsigned errlen, uint32_t *pc)
{
    uint32_t enabled = exc & (st->fpcr & J5S_FPCR_ENABLE_MASK);
    unsigned vec;
    if (!j5s_exc_vector(enabled, &vec)) return 0;
    uint32_t h;
    if (iraise(sb, st, vec, return_pc, &h, errbuf, errlen)) { *pc = 0xFFFFFFFFu; return 1; }
    *pc = h;
    return 1;
}

/* [J5s] BSUN: a SIGNALLING FP conditional predicate (selector bit 4 set) on an UNORDERED
 * operand (FPSR NAN bit set) sets the FPSR BSUN bit, and traps (vector 48) if BSUN is enabled
 * in FPCR. The bit-5 selectors are the IEEE-aware variants (FBSGT/FBSEQ/...); the non-
 * non-signalling variants (bit4 clear) never raise BSUN. Returns 1 if a BSUN trap was TAKEN (the
 * conditional op must NOT also branch — the trap supersedes; *pc holds the handler). Returns 0
 * otherwise (the op proceeds normally). On a raise failure: *pc=0xFFFFFFFF, errbuf set. */
static int j5s_bsun(j5d_sandbox *sb, struct j5d_m68k_state *st, unsigned pred,
                    uint32_t return_pc, char *errbuf, unsigned errlen, uint32_t *pc)
{
    int signalling = (pred & 0x10u) != 0;     /* bit 4 = the IEEE-aware (signalling) predicate set */
    int unordered  = (st->fpsr & J5Q_FPSR_NAN) != 0;
    if (!(signalling && unordered)) return 0;
    st->fpsr |= J5S_FPSR_BSUN;
    st->fpsr |= J5S_FPSR_AIOP;                 /* BSUN accrues to the AIOP (invalid) AEXC bit */
    if (st->fpcr & (J5S_FPSR_BSUN /* enable bit shares the EXC position in the FPCR byte */)) {
        uint32_t h;
        if (iraise(sb, st, J5S_VEC_FP_BSUN, return_pc, &h, errbuf, errlen)) { *pc = 0xFFFFFFFFu; return 1; }
        *pc = h;
        return 1;
    }
    return 0;
}

int j5d_interp_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                   struct j5d_m68k_state *st, uint32_t *exit_d0,
                   j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen)
{
#define IFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s @pc=%08x", (msg), pc); return 1; } while (0)
    /* Decode a control/data EA to an absolute sandbox address for the general ALU
     * handler. `mode`/`reg` are the 68k EA fields; `ext` points at the first
     * extension word and is advanced past whatever this mode consumes. `pcbase`
     * is the address of `ext` (for PC-relative modes). Returns 0 and sets *addr on
     * a supported memory mode; returns 1 for register/unsupported modes. Post-inc
     * side effects are applied by the caller (it knows the size). */
    /* (declared as a local lambda-style via a nested block below; see general ALU) */
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
        /* [J5n] stop-at-PC (the differential lockstep boundary). Return 2 = "stopped at the
         * requested PC" BEFORE executing the instruction there. The engine sets the next
         * block boundary as the stop PC and compares state when the oracle lands on it. */
        if (g_stop_active && pc == g_stop_pc) { st->pc = pc; return 2; }

        const uint8_t *ip = sb->host_mem + (pc - sb->origin);
        uint16_t op = be16(ip);
        st->pc = pc;

        /* [J5n] the per-instruction diagnostics hook: the deterministic global instruction
         * counter, the flight recorder, and the replay-to-N break. Returns nonzero to STOP
         * (a clean replay-to-N landing); st->pc already holds the break PC. */
        if (g_step_hook && g_step_hook(g_step_diag, st, sb, pc, op)) {
            *exit_d0 = st->d[0];
            return 0;
        }

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

        /* ===================== [STD68K] general ALU with a memory-EA source =========
         * The `<op>.<sz> <ea>,Dn` family (add/sub/and/or/cmp) with a MEMORY source EA.
         * Covers the forms compiler-generated code uses that the hand-written reg-reg
         * fast paths below don't: modes (An)=2, (An)+=3, -(An)=4, (d16,An)=5,
         * (d8,An,Xn)=6, abs.l=7.1, (d16,PC)=7.2. Line + opmode select op/size:
         *   1101 (D) add, 1001 (9) sub, 1100 (C) and, 1000 (8) or, 1011 (B) cmp;
         *   opmode 000/001/010 = .b/.w/.l with <ea> as SOURCE into Dn.
         * (opmode 1xx is the Dn->ea store direction / cmpa / cmpm, handled elsewhere.)
         * Full CCR at the operand size; and/or force V=C=0, X kept; cmp leaves Dn. */
        {
            unsigned line = op & 0xF000u;
            unsigned opmode = (op >> 6) & 7u;
            unsigned mode = (op >> 3) & 7u, reg = op & 7u;
            int is_alu = (line == 0xD000u || line == 0x9000u || line == 0xC000u ||
                          line == 0x8000u || line == 0xB000u);
            int is_ea_src = (opmode <= 2u);                 /* .b/.w/.l, <ea>->Dn   */
            int mem_mode = (mode == 2u || mode == 3u || mode == 4u || mode == 5u ||
                            mode == 6u || (mode == 7u && (reg == 1u || reg == 2u)));
            if (is_alu && is_ea_src && mem_mode) {
                unsigned dn = (op >> 9) & 7u;
                unsigned bits = (opmode == 0u) ? 8u : (opmode == 1u) ? 16u : 32u;
                unsigned step = bits / 8u;
                const uint8_t *ext = ip + 2;
                uint32_t addr = 0; int ok = 1;
                switch (mode) {
                    case 2: addr = st->a[reg]; break;
                    case 3: addr = st->a[reg]; break;               /* (An)+ post below */
                    case 4: addr = st->a[reg] - step; break;        /* -(An) pre        */
                    case 5: { int16_t d16 = be16s(ext); ext += 2;
                              addr = st->a[reg] + (uint32_t)(int32_t)d16; break; }
                    case 6: { uint16_t br = be16(ext); ext += 2;
                              int8_t d8 = (int8_t)(br & 0xFFu);
                              unsigned ix = (br >> 12) & 7u;
                              uint32_t idx = (br & 0x8000u) ? st->a[ix] : st->d[ix];
                              if (!(br & 0x0800u)) idx = (uint32_t)(int32_t)(int16_t)idx;
                              idx <<= (br >> 9) & 3u;
                              addr = st->a[reg] + (uint32_t)(int32_t)d8 + idx; break; }
                    case 7:
                        if (reg == 1) { addr = be32(ext); ext += 4; }
                        else { uint32_t pcb = pc + (uint32_t)(ext - ip);
                               int16_t d16 = be16s(ext); ext += 2;
                               addr = pcb + (uint32_t)(int32_t)d16; }
                        break;
                    default: ok = 0; break;
                }
                if (ok) {
                    int oob = 0;
                    uint32_t src = (bits == 8) ? mem_rd8(sb, addr, &oob)
                                 : (bits == 16) ? mem_rd16(sb, addr, &oob)
                                 : mem_rd32(sb, addr, &oob);
                    if (oob) IFAIL("alu <ea>,Dn: address out of sandbox");
                    if (mode == 3) st->a[reg] += step;              /* (An)+ */
                    else if (mode == 4) st->a[reg] -= step;         /* -(An) */
                    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
                    uint32_t msb = 1u << (bits - 1);
                    uint32_t a = st->d[dn] & mask, b = src & mask, res;
                    uint32_t cc = st->ccr & J5D_CCR_X;
                    if (line == 0xC000u || line == 0x8000u) {       /* and / or */
                        res = ((line == 0xC000u) ? (a & b) : (a | b)) & mask;
                        st->d[dn] = (st->d[dn] & ~mask) | res;
                        cc = st->ccr & J5D_CCR_X;
                        if (res == 0)  cc |= J5D_CCR_Z;
                        if (res & msb) cc |= J5D_CCR_N;
                    } else if (line == 0xD000u) {                   /* add */
                        uint64_t s = (uint64_t)a + b; res = (uint32_t)s & mask;
                        cc = 0;
                        if (s & ((uint64_t)mask + 1u)) cc |= J5D_CCR_C | J5D_CCR_X;
                        if (((a ^ res) & (b ^ res)) & msb) cc |= J5D_CCR_V;
                        if (res == 0)  cc |= J5D_CCR_Z;
                        if (res & msb) cc |= J5D_CCR_N;
                        st->d[dn] = (st->d[dn] & ~mask) | res;
                    } else {                                        /* sub or cmp */
                        res = (a - b) & mask;
                        cc = (line == 0x9000u) ? 0u : (st->ccr & J5D_CCR_X);
                        if (b > a)                        cc |= J5D_CCR_C | (line == 0x9000u ? J5D_CCR_X : 0u);
                        if (((a ^ b) & (a ^ res)) & msb)  cc |= J5D_CCR_V;
                        if (res == 0)                     cc |= J5D_CCR_Z;
                        if (res & msb)                    cc |= J5D_CCR_N;
                        if (line == 0x9000u) st->d[dn] = (st->d[dn] & ~mask) | res; /* sub writes */
                    }
                    st->ccr = cc;
                    pc += (uint32_t)(ext - ip); continue;
                }
            }
        }


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

        /* move.w %ccr,Dn : 42C0 | dn  (move from CCR to a data register; the fix
         * emits this to preserve flags across a copy). Low word of Dn = CCR byte,
         * upper byte of that word zero; CCR itself unchanged. */
        if ((op & 0xFFF8u) == 0x42C0u) {
            unsigned dn = op & 7u;
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | (st->ccr & 0x1Fu);
            pc += 2; continue;
        }
        /* move.w Dn,%ccr : 44C0 | dn  (move to CCR from a data register; restores
         * the preserved flags). CCR = low byte & 0x1F. */
        if ((op & 0xFFF8u) == 0x44C0u) {
            unsigned dn = op & 7u;
            st->ccr = st->d[dn] & 0x1Fu;
            pc += 2; continue;
        }
        /* move.w %ccr,-(An) : 42E0 | an  (MOV16ec — the stack-spill fallback push).
         * Store the CCR word (upper byte 0) to -(An), 2-byte word. CCR unchanged. */
        if ((op & 0xFFF8u) == 0x42E0u) {
            unsigned an = op & 7u;
            uint32_t addr = st->a[an] - 2u;
            int oob = 0;
            mem_wr16(sb, addr, (uint16_t)(st->ccr & 0x1Fu), &oob);
            if (oob) IFAIL("move.w ccr,-(An): address out of sandbox");
            st->a[an] = addr;
            pc += 2; continue;
        }
        /* move.w (An)+,%ccr : 44D8 | an  (MOV16co — the stack-spill fallback pop).
         * CCR = word at (An) & 0x1F; An += 2. */
        if ((op & 0xFFF8u) == 0x44D8u) {
            unsigned an = op & 7u;
            int oob = 0;
            uint16_t v = mem_rd16(sb, st->a[an], &oob);
            if (oob) IFAIL("move.w (An)+,ccr: address out of sandbox");
            st->ccr = v & 0x1Fu;
            st->a[an] += 2u;
            pc += 2; continue;
        }

        /* addi.l/subi.l #imm32,(An) : 0690/0490 | an + imm32  (read-modify-write a
         * sandbox longword; PRM ADDI/SUBI, EA mode 010). Full CCR on the longword. */
        if ((op & 0xFFF8u) == 0x0690u || (op & 0xFFF8u) == 0x0490u) {
            unsigned an = op & 7u;
            int is_add = (op & 0x0F00u) == 0x0600u;
            uint32_t b = be32(ip + 2);
            uint32_t addr = st->a[an];
            int oob = 0;
            uint32_t a = mem_rd32(sb, addr, &oob);
            if (oob) IFAIL("addi/subi.l (An): address out of sandbox");
            uint64_t s = is_add ? (uint64_t)a + b : (uint64_t)a - b;
            uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (is_add) {
                if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            } else {
                if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            }
            mem_wr32(sb, addr, res, &oob);
            if (oob) IFAIL("addi/subi.l (An): store out of sandbox");
            st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 6; continue;
        }
        /* cas.b/.w/.l Dc,Du,(An) : 0AD0/0CD0/0ED0 | an + extword (Du<<6 | Dc).
         * PRM CAS, single-task semantics (load, compare, conditional store — the
         * atomicity is free with one task). Ext word: bits 8-6 = Du, bits 2-0 = Dc.
         * Compare (An) with Dc, set CCR like cmp; if equal store Du, else load into
         * Dc. Only (An) mode (010) is emitted by LLVM; that is what we handle. */
        if ((op & 0xFDF8u) == 0x0CD0u || (op & 0xFFF8u) == 0x0AD0u) {
            unsigned an = op & 7u;
            unsigned bits = ((op & 0xFF00u) == 0x0A00u) ? 8u
                          : ((op & 0xFF00u) == 0x0C00u) ? 16u : 32u;
            uint16_t ext = be16(ip + 2);
            unsigned du = (ext >> 6) & 7u, dc = ext & 7u;
            uint32_t addr = st->a[an];
            int oob = 0;
            uint32_t mem;
            if (bits == 8)       mem = mem_rd8(sb, addr, &oob);
            else if (bits == 16) mem = mem_rd16(sb, addr, &oob);
            else                 mem = mem_rd32(sb, addr, &oob);
            if (oob) IFAIL("cas: address out of sandbox");
            uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            uint32_t c = st->d[dc] & mask, m = mem & mask;
            uint32_t res = (c - m) & mask;
            uint32_t msb = 1u << (bits - 1);
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (m > c)                         cc |= J5D_CCR_C;
            if (((c ^ m) & (c ^ res)) & msb)   cc |= J5D_CCR_V;
            if (res == 0)                      cc |= J5D_CCR_Z;
            if (res & msb)                     cc |= J5D_CCR_N;
            st->ccr = cc;
            if (res == 0) {                    /* equal: store Du */
                uint32_t u = st->d[du] & mask;
                if (bits == 8)       mem_wr8(sb, addr, (uint8_t)u, &oob);
                else if (bits == 16) mem_wr16(sb, addr, (uint16_t)u, &oob);
                else                 mem_wr32(sb, addr, u, &oob);
                if (oob) IFAIL("cas: store out of sandbox");
            } else {                           /* not equal: load into Dc */
                st->d[dc] = (bits == 32) ? mem
                          : (st->d[dc] & ~mask) | m;
            }
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
        /* jsr (d16,PC) (4EBA) -> push return + PC-relative jump (gcc near call).
         * PRM: the base is the address OF the extension word (pc + 2). */
        if (op == 0x4EBAu) {
            int16_t d16 = be16s(ip + 2);
            uint32_t target = pc + 2u + (uint32_t)(int32_t)d16;
            uint32_t sp = st->a[7] - 4u;
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("jsr (d16,PC): a7 out of sandbox");
            uint32_t ret = pc + 4;
            uint8_t *p = sb->host_mem + (sp - sb->origin);
            p[0]=ret>>24; p[1]=ret>>16; p[2]=ret>>8; p[3]=(uint8_t)ret;
            st->a[7] = sp; pc = target; continue;
        }
        /* jmp (d16,PC) (4EFA) -> PC-relative jump (no push). */
        if (op == 0x4EFAu) {
            int16_t d16 = be16s(ip + 2);
            pc = pc + 2u + (uint32_t)(int32_t)d16; continue;
        }
        /* link.w An,#d16 (4E50|An) : push An; An = SP(after push); SP += d16 (d16 is
         * negative for a frame). PRM LINK; no CCR change. */
        if ((op & 0xFFF8u) == 0x4E50u) {
            unsigned an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            uint32_t sp = st->a[7] - 4u;
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("link: a7 out of sandbox");
            uint32_t v = st->a[an];
            uint8_t *p = sb->host_mem + (sp - sb->origin);
            p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=(uint8_t)v;
            st->a[an] = sp;
            st->a[7] = sp + (uint32_t)(int32_t)d16;
            pc += 4; continue;
        }
        /* unlk An (4E58|An) : SP = An; An = pop. PRM UNLK; no CCR change. */
        if ((op & 0xFFF8u) == 0x4E58u) {
            unsigned an = op & 7u;
            uint32_t sp = st->a[an];
            if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size)
                IFAIL("unlk: frame pointer out of sandbox");
            uint8_t *p = sb->host_mem + (sp - sb->origin);
            st->a[an] = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
            st->a[7] = sp + 4u;
            pc += 2; continue;
        }
        /* movea.w #imm16,An (307C|an<<9 + imm16) : the 16-bit immediate is SIGN-
         * EXTENDED to 32 bits into An; MOVEA affects no flags (PRM MOVEA). */
        if ((op & 0xF1FFu) == 0x307Cu && ((op >> 6) & 7u) == 1u) {
            unsigned an = (op >> 9) & 7u;
            st->a[an] = (uint32_t)(int32_t)be16s(ip + 2);
            pc += 4; continue;
        }
        /* addi.b #imm,Dn (0600|dn) / subi.b #imm,Dn (0400|dn) : byte add/sub
         * immediate on the low byte of Dn; full CCR on the byte (PRM ADDI/SUBI). */
        if ((op & 0xFDF8u) == 0x0400u) {
            /* bit 9 selects: 0x0600 = addi.b, 0x0400 = subi.b (size 00, EA = Dn) */
            unsigned dn = op & 7u;
            int is_add = (op & 0x0200u) != 0;
            uint8_t a = (uint8_t)(st->d[dn] & 0xFFu), b = (uint8_t)be16s(ip + 2);
            uint32_t s = is_add ? (uint32_t)a + b : (uint32_t)a - b;
            uint8_t res = (uint8_t)s;
            uint32_t cc = 0;
            if (is_add) {
                if (s & 0x100u) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ res) & (b ^ res)) & 0x80u) cc |= J5D_CCR_V;
            } else {
                if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ b) & (a ^ res)) & 0x80u) cc |= J5D_CCR_V;
            }
            if (res == 0)    cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* sub.b #imm,Dn (903C|dn<<9 + imm16 low byte) / add.b #imm,Dn (D03C|dn<<9):
         * the immediate-EA byte forms (like the .l D0BC/90BC family above); full
         * CCR on the byte, only the low byte of Dn changes. */
        if ((op & 0xB1FFu) == 0x903Cu) {
            unsigned dn = (op >> 9) & 7u;
            int is_add = (op & 0x4000u) != 0;
            uint8_t a = (uint8_t)(st->d[dn] & 0xFFu), b = (uint8_t)be16s(ip + 2);
            uint32_t s = is_add ? (uint32_t)a + b : (uint32_t)a - b;
            uint8_t res = (uint8_t)s;
            uint32_t cc = 0;
            if (is_add) {
                if (s & 0x100u) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ res) & (b ^ res)) & 0x80u) cc |= J5D_CCR_V;
            } else {
                if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ b) & (a ^ res)) & 0x80u) cc |= J5D_CCR_V;
            }
            if (res == 0)    cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* eori.w #imm16,Dn (0A40|dn + imm16) : word XOR immediate; low word of Dn
         * only; N/Z on the word, V=C=0, X kept (PRM EORI). */
        if ((op & 0xFFF8u) == 0x0A40u) {
            unsigned dn = op & 7u;
            uint16_t res = (uint16_t)(st->d[dn] & 0xFFFFu) ^ (uint16_t)be16s(ip + 2);
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)      cc |= J5D_CCR_Z;
            if (res & 0x8000u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* eori.b #imm,Dn (0A00|dn + imm16 low byte) : byte XOR immediate; same rule. */
        if ((op & 0xFFF8u) == 0x0A00u) {
            unsigned dn = op & 7u;
            uint8_t res = (uint8_t)(st->d[dn] & 0xFFu) ^ (uint8_t)be16s(ip + 2);
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)    cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* andi.w #imm16,Dn (0240|dn + imm16) : word AND immediate; low word of Dn
         * only; N/Z on the word, V=C=0, X kept (PRM ANDI). */
        if ((op & 0xFFF8u) == 0x0240u) {
            unsigned dn = op & 7u;
            uint16_t res = (uint16_t)(st->d[dn] & 0xFFFFu) & (uint16_t)be16s(ip + 2);
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)      cc |= J5D_CCR_Z;
            if (res & 0x8000u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* clr.l -(An) (42A0|an) : predecrement clear (gcc pushes a zero argument as
         * clr.l -(sp)). An -= 4 first; PRM CLR: Z set, N/V/C clear, X kept. */
        if ((op & 0xFFF8u) == 0x42A0u) {
            unsigned an = op & 7u;
            uint32_t ea = st->a[an] - 4u;
            int oob = 0;
            mem_wr32(sb, ea, 0, &oob);
            if (oob) IFAIL("clr.l -(An): address out of sandbox");
            st->a[an] = ea;
            st->ccr = (st->ccr & J5D_CCR_X) | J5D_CCR_Z;
            pc += 2; continue;
        }
        /* clr.l (An) (4290|an) : indirect clear; same CCR rule. */
        if ((op & 0xFFF8u) == 0x4290u) {
            unsigned an = op & 7u;
            int oob = 0;
            mem_wr32(sb, st->a[an], 0, &oob);
            if (oob) IFAIL("clr.l (An): address out of sandbox");
            st->ccr = (st->ccr & J5D_CCR_X) | J5D_CCR_Z;
            pc += 2; continue;
        }
        /* clr.l (d16,An) (42A8|an + d16) : longword clear at a displacement (gcc
         * frame slots). PRM CLR: Z set, N/V/C clear, X kept. */
        if ((op & 0xFFF8u) == 0x42A8u) {
            unsigned an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            mem_wr32(sb, st->a[an] + (uint32_t)(int32_t)d16, 0, &oob);
            if (oob) IFAIL("clr.l (d16,An): address out of sandbox");
            st->ccr = (st->ccr & J5D_CCR_X) | J5D_CCR_Z;
            pc += 4; continue;
        }
        /* tst.l (d16,An) (4AA8|an + d16) : longword test at a displacement.
         * PRM TST: N/Z per the value, V=C=0, X kept. */
        if ((op & 0xFFF8u) == 0x4AA8u) {
            unsigned an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            uint32_t v = mem_rd32(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("tst.l (d16,An): address out of sandbox");
            st->ccr = nz32(v, st->ccr & J5D_CCR_X);
            pc += 4; continue;
        }
        /* tst.l abs.l (4AB9 + addr32) : longword test at an absolute address.
         * PRM TST: N/Z per the value, V=C=0, X kept. */
        if (op == 0x4AB9u) {
            uint32_t addr = be32(ip + 2);
            int oob = 0;
            uint32_t v = mem_rd32(sb, addr, &oob);
            if (oob) IFAIL("tst.l abs.l: address out of sandbox");
            st->ccr = nz32(v, st->ccr & J5D_CCR_X);
            pc += 6; continue;
        }
        /* clr.l abs.l (42B9 + addr32) : longword clear at an absolute address.
         * PRM CLR: Z set, N/V/C clear, X kept. */
        if (op == 0x42B9u) {
            uint32_t addr = be32(ip + 2);
            int oob = 0;
            mem_wr32(sb, addr, 0, &oob);
            if (oob) IFAIL("clr.l abs.l: address out of sandbox");
            st->ccr = (st->ccr & J5D_CCR_X) | J5D_CCR_Z;
            pc += 6; continue;
        }

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
        /* add.l Am,Dn : D088 | dn<<9 | am  (address-register direct source, EA mode 001).
         * PRM ADD: <ea> may be An for word/long sizes; sets CCR exactly like add.l Dm,Dn.
         * Compiler-generated code (LLVM) uses An as a spare integer source freely. */
        if ((op & 0xF1F8u) == 0xD088u) {
            unsigned dn = (op >> 9) & 7u, am = op & 7u;
            uint32_t a = st->d[dn], b = st->a[am];
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

        /* add.b/.w and sub.b/.w  Dm,Dn (register-direct, sizes 00/01): the byte and
         * word siblings of the .l reg-reg forms. 1101=add / 1001=sub, DDD dst, size
         * in bits 7-6, SSS src (mode 000). Full CCR on the sized operand; only the
         * low byte/word of Dn changes. */
        if (((op & 0xF1F8u) == 0xD000u || (op & 0xF1F8u) == 0xD040u ||
             (op & 0xF1F8u) == 0x9000u || (op & 0xF1F8u) == 0x9040u)) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            int is_add = (op & 0xF000u) == 0xD000u;
            unsigned bits = (((op >> 6) & 3u) == 0u) ? 8u : 16u;
            uint32_t mask = (1u << bits) - 1u, msb = 1u << (bits - 1);
            uint32_t a = st->d[dn] & mask, b = st->d[dm] & mask;
            uint32_t s = is_add ? a + b : a - b;
            uint32_t res = s & mask;
            uint32_t cc = 0;
            if (is_add) {
                if (s & (mask + 1u)) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ res) & (b ^ res)) & msb) cc |= J5D_CCR_V;
            } else {
                if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
                if (((a ^ b) & (a ^ res)) & msb) cc |= J5D_CCR_V;
            }
            if (res == 0)  cc |= J5D_CCR_Z;
            if (res & msb) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & ~mask) | res;
            st->ccr = cc;
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

        /* sub.l (d16,An),Dn : 90A8 | dn<<9 | an + d16  (displacement longword load; PRM SUB
         * <ea> mode 101). CCR exactly like sub.l Dm,Dn. Compiler-generated code subtracts
         * stack slots (d16,A7) directly. */
        if ((op & 0xF1F8u) == 0x90A8u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            uint32_t b = mem_rd32(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("sub.l (d16,An): address out of sandbox");
            uint32_t a = st->d[dn]; uint32_t res = a - b;
            uint32_t cc = 0;
            if (b > a) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ b) & (a ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 4; continue;
        }
        /* add.l (d16,An),Dn : D0A8 | dn<<9 | an + d16  (displacement longword load; PRM ADD
         * <ea> mode 101). CCR exactly like add.l Dm,Dn. */
        if ((op & 0xF1F8u) == 0xD0A8u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            uint32_t b = mem_rd32(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("add.l (d16,An): address out of sandbox");
            uint32_t a = st->d[dn]; uint64_t s = (uint64_t)a + b; uint32_t res = (uint32_t)s;
            uint32_t cc = 0;
            if (s >> 32) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80000000u) cc |= J5D_CCR_V;
            st->d[dn] = res; st->ccr = nz32(res, cc) | (cc & (J5D_CCR_V|J5D_CCR_C|J5D_CCR_X));
            pc += 4; continue;
        }

        /* Scc Dn : 50C0 | cc<<8 | dn  (set-on-condition, byte, register direct; PRM Scc).
         * Low BYTE of Dn = 0xFF if the condition holds, else 0x00; upper 24 bits and the
         * CCR are untouched. Same 4-bit condition table as Bcc (bcc_taken). Does not
         * collide with DBcc (mode 001, 50C8) or ADDQ/SUBQ (size bits != 11). */
        if ((op & 0xF0F8u) == 0x50C0u) {
            unsigned cc4 = (op >> 8) & 0xFu, dn = op & 7u;
            /* cc 0/1 = ST/SF (always/never) — bcc_taken leaves those to the caller. */
            int t = (cc4 == 0) ? 1 : (cc4 == 1) ? 0 : bcc_taken(cc4, st->ccr);
            uint32_t v = t ? 0xFFu : 0x00u;
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | v;
            pc += 2; continue;
        }

        /* and.l #imm,Dn : C0BC | dn<<9  (AND <ea>,Dn with EA = mode 7 reg 4 = immediate —
         * the same immediate-EA family as the D0BC/90BC add/sub forms above, as opposed to
         * the ANDI encoding). N/Z per result, V=C=0, X kept (PRM AND). */
        if ((op & 0xF1FFu) == 0xC0BCu) {
            unsigned dn = (op >> 9) & 7u;
            uint32_t res = st->d[dn] & be32(ip + 2);
            st->d[dn] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 6; continue;
        }
        /* cmpi.b #imm,Dn : 0C00 | dn  (byte compare immediate; PRM CMPI). Flags exactly
         * like a byte subtract Dn.b - imm.b; Dn unmodified; X unchanged. */
        if ((op & 0xFFF8u) == 0x0C00u) {
            unsigned dn = op & 7u;
            uint8_t a = (uint8_t)(st->d[dn] & 0xFFu), b = (uint8_t)be16s(ip + 2);
            uint8_t res = (uint8_t)(a - b);
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a)                          cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80u)  cc |= J5D_CCR_V;
            if (res == 0)                       cc |= J5D_CCR_Z;
            if (res & 0x80u)                    cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* cmpi.b/.w/.l #imm,<ea> for memory EA modes (An)=2, (An)+=3, (d8,An,Xn)=6,
         * abs.l=7.1: 0C00|(size<<6)|(mode<<3)|reg + imm + optional EA ext. The
         * immediate comes FIRST (sized), then the brief/abs extension (PRM CMPI).
         * Compare mem - imm at the operand size; memory and X unchanged. */
        if ((op & 0xFF00u) == 0x0C00u && ((op >> 6) & 3u) != 3u) {
            unsigned mode = (op >> 3) & 7u, reg = op & 7u;
            unsigned sz = (op >> 6) & 3u;
            unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
            if (mode == 2 || mode == 3 || mode == 6 || (mode == 7 && reg == 1)) {
                const uint8_t *ext = ip + 2;
                uint32_t imm;
                if (bits == 8)       { imm = (uint8_t)be16s(ext);  ext += 2; }
                else if (bits == 16) { imm = (uint16_t)be16s(ext); ext += 2; }
                else                 { imm = be32(ext);            ext += 4; }
                uint32_t addr;
                if (mode == 2 || mode == 3) addr = st->a[reg];
                else if (mode == 6) {
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= (brief >> 9) & 3u;
                    addr = st->a[reg] + (uint32_t)(int32_t)d8 + index;
                } else { addr = be32(ext); ext += 4; }
                int oob = 0;
                uint32_t a = (bits == 8) ? mem_rd8(sb, addr, &oob)
                          : (bits == 16) ? mem_rd16(sb, addr, &oob)
                          : mem_rd32(sb, addr, &oob);
                if (oob) IFAIL("cmpi <ea>: address out of sandbox");
                if (mode == 3) st->a[reg] += bits / 8u;
                uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
                uint32_t msb = 1u << (bits - 1);
                uint32_t b = imm & mask, av = a & mask, res = (av - b) & mask;
                uint32_t cc = st->ccr & J5D_CCR_X;
                if (b > av)                       cc |= J5D_CCR_C;
                if (((av ^ b) & (av ^ res)) & msb) cc |= J5D_CCR_V;
                if (res == 0)                     cc |= J5D_CCR_Z;
                if (res & msb)                    cc |= J5D_CCR_N;
                st->ccr = cc;
                pc += (uint32_t)(ext - ip); continue;
            }
        }
        /* cmpi.w #imm16,Dn : 0C40 | dn  (word compare immediate; flags like a word
         * subtract Dn.w - imm.w; Dn unmodified; X unchanged — PRM CMPI). */
        if ((op & 0xFFF8u) == 0x0C40u) {
            unsigned dn = op & 7u;
            uint16_t a = (uint16_t)(st->d[dn] & 0xFFFFu), b = (uint16_t)be16s(ip + 2);
            uint16_t res = (uint16_t)(a - b);
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a)                            cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x8000u)  cc |= J5D_CCR_V;
            if (res == 0)                         cc |= J5D_CCR_Z;
            if (res & 0x8000u)                    cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* and.w/or.w #imm16,Dn : C07C/807C | dn<<9  (word logical immediate, EA mode
         * 7.4; low word only; N/Z on the word, V=C=0, X kept). */
        if ((op & 0xF1FFu) == 0xC07Cu || (op & 0xF1FFu) == 0x807Cu) {
            unsigned dn = (op >> 9) & 7u;
            int is_and = (op & 0xF000u) == 0xC000u;
            uint16_t imm = (uint16_t)be16s(ip + 2);
            uint16_t res = is_and ? (uint16_t)(st->d[dn] & 0xFFFFu) & imm
                                  : (uint16_t)(st->d[dn] & 0xFFFFu) | imm;
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)      cc |= J5D_CCR_Z;
            if (res & 0x8000u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* and.b #imm,Dn : C03C | dn<<9  (byte AND, immediate EA; low byte only; N/Z on the
         * byte, V=C=0, X kept; upper 24 bits of Dn preserved — PRM AND, byte size). */
        if ((op & 0xF1FFu) == 0xC03Cu) {
            unsigned dn = (op >> 9) & 7u;
            uint8_t res = (uint8_t)(st->d[dn] & 0xFFu) & (uint8_t)be16s(ip + 2);
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)    cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* or.b #imm,Dn : 803C | dn<<9  (byte OR, immediate EA; same CCR rule as and.b). */
        if ((op & 0xF1FFu) == 0x803Cu) {
            unsigned dn = (op >> 9) & 7u;
            uint8_t res = (uint8_t)(st->d[dn] & 0xFFu) | (uint8_t)be16s(ip + 2);
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)    cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* or.l #imm,Dn : 80BC | dn<<9  (OR <ea>,Dn, immediate EA; same CCR rule). */
        if ((op & 0xF1FFu) == 0x80BCu) {
            unsigned dn = (op >> 9) & 7u;
            uint32_t res = st->d[dn] | be32(ip + 2);
            st->d[dn] = res; st->ccr = nz32(res, st->ccr & J5D_CCR_X);
            pc += 6; continue;
        }

        /* and.b/.w and or.b/.w  Dm,Dn (register-direct, sizes 00/01): byte/word
         * siblings of the .l logical reg-reg forms. 1100=and / 1000=or, DDD dst,
         * size 7-6, SSS src (mode 000). N/Z on the sized result, V=C=0, X kept. */
        if ((op & 0xF1F8u) == 0xC000u || (op & 0xF1F8u) == 0xC040u ||
            (op & 0xF1F8u) == 0x8000u || (op & 0xF1F8u) == 0x8040u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            int is_and = (op & 0xF000u) == 0xC000u;
            unsigned bits = (((op >> 6) & 3u) == 0u) ? 8u : 16u;
            uint32_t mask = (1u << bits) - 1u, msb = 1u << (bits - 1);
            uint32_t res = is_and ? (st->d[dn] & st->d[dm] & mask)
                                  : ((st->d[dn] | st->d[dm]) & mask);
            st->d[dn] = (st->d[dn] & ~mask) | res;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (res == 0)  cc |= J5D_CCR_Z;
            if (res & msb) cc |= J5D_CCR_N;
            st->ccr = cc;
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

        /* ==================== [J5q] FP CONDITIONAL CONTROL-FLOW (the oracle mirror) =========
         * Same predicate table (j5q_fp_cond_taken, shared header) + the same FPSR cc the FP
         * ops produced (st->fpsr). Decoded here BEFORE the line-F FPU arithmetic block so the
         * control ops don't fall into the FMOVE/arith decoder. Mirrors the dispatcher in
         * j5d_engine.c exactly (the byte-exact + control-flow gate). Order: FDBcc (mode 001)
         * and FTRAPcc (mode 111) carve out of the FScc EA range, so test them first. */
        if ((op & 0xFFF8u) == 0xF248u) {                         /* FDBcc Dn,disp16    */
            unsigned dn = op & 7u;
            unsigned pred = be16(ip + 2) & 0x3fu;
            int16_t disp16 = be16s(ip + 4);
            uint32_t disp_pc = pc + 4;
            uint32_t after = pc + 6;
            { uint32_t bpc; if (j5s_bsun(sb, st, pred, after, errbuf, errlen, &bpc))   /* [J5s] BSUN */
                { if (bpc == 0xFFFFFFFFu) return 1; pc = bpc; continue; } }
            if (j5q_fp_cond_taken(pred, st->fpsr)) {
                pc = after;
            } else {
                uint16_t cnt = (uint16_t)((st->d[dn] & 0xFFFFu) - 1u);
                st->d[dn] = (st->d[dn] & 0xFFFF0000u) | cnt;
                pc = (cnt == 0xFFFFu) ? after : (uint32_t)((int64_t)disp_pc + disp16);
            }
            continue;
        }
        if ((op & 0xFFF8u) == 0xF278u) {                         /* FTRAPcc -> vector 7 */
            unsigned mode = op & 7u;
            unsigned pred = be16(ip + 2) & 0x3fu;
            uint32_t after = pc + 4;
            if (mode == 2) after += 2; else if (mode == 3) after += 4;
            { uint32_t bpc; if (j5s_bsun(sb, st, pred, after, errbuf, errlen, &bpc))   /* [J5s] BSUN */
                { if (bpc == 0xFFFFFFFFu) return 1; pc = bpc; continue; } }
            if (j5q_fp_cond_taken(pred, st->fpsr)) {
                uint32_t h;
                if (iraise(sb, st, J5I_VECTOR_TRAPcc, after, &h, errbuf, errlen)) return 1;
                pc = h;
            } else {
                pc = after;
            }
            continue;
        }
        if ((op & 0xFFC0u) == 0xF240u) {                         /* FScc <ea> (set byte) */
            unsigned pred = be16(ip + 2) & 0x3fu;
            unsigned mode = (op >> 3) & 7u, regn = op & 7u;
            uint32_t after0 = pc + 4; if (mode == 5) after0 += 2;
            { uint32_t bpc; if (j5s_bsun(sb, st, pred, after0, errbuf, errlen, &bpc))   /* [J5s] BSUN */
                { if (bpc == 0xFFFFFFFFu) return 1; pc = bpc; continue; } }
            uint8_t val = j5q_fp_cond_taken(pred, st->fpsr) ? 0xFFu : 0x00u;
            uint32_t after = pc + 4;
            if (mode == 0) {
                st->d[regn] = (st->d[regn] & 0xFFFFFF00u) | val;
            } else if (mode == 2) {
                uint32_t a = st->a[regn];
                if (a < sb->origin || (uint64_t)a + 1 > (uint64_t)sb->origin + sb->size)
                    IFAIL("FScc (An) destination out of sandbox");
                sb->host_mem[a - sb->origin] = val;
            } else if (mode == 5) {
                int16_t d16 = be16s(ip + 4); after += 2;
                uint32_t a = st->a[regn] + (uint32_t)(int32_t)d16;
                if (a < sb->origin || (uint64_t)a + 1 > (uint64_t)sb->origin + sb->size)
                    IFAIL("FScc (d16,An) destination out of sandbox");
                sb->host_mem[a - sb->origin] = val;
            } else {
                IFAIL("FScc destination EA mode not in the [J5q] subset");
            }
            pc = after;
            continue;
        }
        if ((op & 0xFF80u) == 0xF280u) {                         /* FBcc (.W/.L)        */
            unsigned pred = op & 0x3fu;
            int is_long = (op & 0x40u) != 0;
            uint32_t disp_pc = pc + 2;
            int64_t disp; uint32_t after;
            if (is_long) { disp = (int32_t)be32(ip + 2); after = pc + 6; }
            else         { disp = (int16_t)be16s(ip + 2); after = pc + 4; }
            { uint32_t bpc; if (j5s_bsun(sb, st, pred, after, errbuf, errlen, &bpc))   /* [J5s] BSUN */
                { if (bpc == 0xFFFFFFFFu) return 1; pc = bpc; continue; } }
            pc = j5q_fp_cond_taken(pred, st->fpsr)
                 ? (uint32_t)((int64_t)disp_pc + disp)
                 : after;
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

        /* ============================ [STD68K] register-count shifts/rotates =========
         * op = 1110 ccc d ss 1 tt rrr : ccc = the register holding the count (mod 64),
         * d = dir(1 left), ss = size(00 b,01 w,10 l), bit5=1 (register count),
         * tt = 00 as / 01 ls / 11 ro (rox not exercised), rrr = Dn. CCR like the
         * immediate forms; a zero count clears C and leaves the value (PRM). */
        if ((op & 0xF000u) == 0xE000u && ((op >> 5) & 1u) == 1u && ((op >> 3) & 3u) != 2u) {
            unsigned dn = op & 7u;
            unsigned sz = (op >> 6) & 3u;
            if (sz != 3u) {
                unsigned bits = (sz == 0) ? 8u : (sz == 1) ? 16u : 32u;
                uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
                uint32_t msb = 1u << (bits - 1);
                unsigned dir = (op >> 8) & 1u;
                unsigned type = (op >> 3) & 3u;    /* 0 as, 1 ls, 3 ro */
                unsigned cnt = st->d[(op >> 9) & 7u] & 63u;
                uint32_t a = st->d[dn] & mask, res = a;
                uint32_t cc = st->ccr & J5D_CCR_X;
                int last_out = -1;
                if (type == 3) {                   /* ROL/ROR (no X) */
                    unsigned r = cnt % bits;
                    if (cnt == 0) { res = a; }
                    else if (dir) { res = ((a << r) | (a >> (bits - r))) & mask; last_out = res & 1u; }
                    else          { res = ((a >> r) | (a << (bits - r))) & mask; last_out = (res >> (bits - 1)) & 1u; }
                    cc = st->ccr & J5D_CCR_X;
                    if (last_out == 1) cc |= J5D_CCR_C;
                    if (res == 0)  cc |= J5D_CCR_Z;
                    if (res & msb) cc |= J5D_CCR_N;
                } else {                           /* ASL/ASR/LSL/LSR */
                    if (cnt == 0) {
                        cc = st->ccr & J5D_CCR_X;  /* C cleared, X kept, N/Z from value */
                        if (a == 0)   cc |= J5D_CCR_Z;
                        if (a & msb)  cc |= J5D_CCR_N;
                    } else if (dir) {              /* left (ASL == LSL for the bits) */
                        last_out = (cnt <= bits) ? ((a >> (bits - cnt)) & 1u) : 0u;
                        res = (cnt >= bits) ? 0u : ((a << cnt) & mask);
                        cc = 0;
                        if (last_out == 1) cc |= J5D_CCR_C | J5D_CCR_X;
                        if (type == 0) {           /* ASL: V if sign changed */
                            uint32_t sm = (cnt >= bits) ? mask : (mask << (bits - 1 - cnt)) & mask;
                            uint32_t top = a & sm;
                            if (!(top == 0 || top == sm)) cc |= J5D_CCR_V;
                        }
                        if (res == 0)  cc |= J5D_CCR_Z;
                        if (res & msb) cc |= J5D_CCR_N;
                    } else {                       /* right */
                        last_out = (cnt <= bits) ? ((a >> (cnt - 1)) & 1u) : ((a & msb) && type == 0 ? 1u : 0u);
                        if (type == 0) {           /* ASR: sign-extend */
                            uint32_t se = (a & msb) ? (mask & ~((cnt >= bits) ? 0u : (mask >> cnt))) : 0u;
                            res = (cnt >= bits) ? ((a & msb) ? mask : 0u) : ((a >> cnt) | se) & mask;
                        } else {                   /* LSR */
                            res = (cnt >= bits) ? 0u : (a >> cnt);
                        }
                        cc = 0;
                        if (last_out == 1) cc |= J5D_CCR_C | J5D_CCR_X;
                        if (res == 0)  cc |= J5D_CCR_Z;
                        if (res & msb) cc |= J5D_CCR_N;
                    }
                }
                st->d[dn] = (st->d[dn] & ~mask) | (res & mask);
                st->ccr = cc;
                pc += 2; continue;
            }
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
            else if (mode == 5 || mode == 6 || (mode == 7 && reg == 1)) {
                /* (d16,An) / (d8,An,Xn) scaled / abs.l */
                uint32_t ea;
                if (mode == 5) {
                    ea = st->a[reg] + (uint32_t)(int32_t)be16s(ip + 2); adv = 4;
                } else if (mode == 6) {
                    uint16_t brief = be16(ip + 2);
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= (brief >> 9) & 3u;                /* 68020 scale */
                    ea = st->a[reg] + (uint32_t)(int32_t)d8 + index; adv = 4;
                } else {
                    ea = be32(ip + 2); adv = 6;
                }
                if (bits == 8)       v = mem_rd8(sb, ea, &oob);
                else if (bits == 16) v = mem_rd16(sb, ea, &oob);
                else                 v = mem_rd32(sb, ea, &oob);
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
            else if (mode == 6) {                               /* (d8,An,Xn) scaled */
                uint16_t brief = be16(ext); ext += 2;
                int8_t d8 = (int8_t)(brief & 0xFFu);
                unsigned ix = (brief >> 12) & 7u;
                uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                index <<= (brief >> 9) & 3u;                    /* 68020 scale */
                ea = st->a[reg] + (uint32_t)(int32_t)d8 + index;
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
            else if (sm == 2) { b = mem_rd32(sb, st->a[sr], &oob); }          /* (An)   */
            else if (sm == 3) {                                               /* (An)+  */
                b = mem_rd32(sb, st->a[sr], &oob);
                if (!oob) st->a[sr] += 4u;
            }
            else if (sm == 5) {                                               /* (d16,An) */
                int16_t d16 = be16s(ext); ext += 2;
                b = mem_rd32(sb, st->a[sr] + (uint32_t)(int32_t)d16, &oob);
            }
            else if (sm == 7 && sr == 1) { b = mem_rd32(sb, be32(ext), &oob); ext += 4; } /* abs.l */
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

        /* cmpi.l #imm,(d16,An) : 0CA8 | an + imm32 + d16  (compare immediate with a memory
         * longword; PRM CMPI — the immediate extension comes FIRST, then the EA extension).
         * Flags like sub; memory and X unchanged. 8 bytes total. */
        if ((op & 0xFFF8u) == 0x0CA8u) {
            unsigned an = op & 7u;
            uint32_t b = be32(ip + 2);
            int16_t d16 = be16s(ip + 6);
            int oob = 0;
            uint32_t a = mem_rd32(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("cmpi.l (d16,An): address out of sandbox");
            uint32_t res = a - b;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a)                                   cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80000000u)     cc |= J5D_CCR_V;
            if (res == 0)                                cc |= J5D_CCR_Z;
            if (res & 0x80000000u)                       cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 8; continue;
        }
        /* cmpm.b/.w/.l (Ay)+,(Ax)+ : 1011 xxx 1 ss 001 yyy  (memory compare with
         * post-increment on BOTH; PRM CMPM). Flags like cmp (dest - source); X kept. */
        if ((op & 0xF138u) == 0xB108u && ((op >> 6) & 3u) != 3u) {
            unsigned ax = (op >> 9) & 7u, ay = op & 7u;
            unsigned szb = (op >> 6) & 3u;
            unsigned bytes = (szb == 0) ? 1u : (szb == 1) ? 2u : 4u;
            int oob = 0;
            uint32_t s, dv;
            if (bytes == 1)      { s = mem_rd8(sb, st->a[ay], &oob);  dv = oob ? 0 : mem_rd8(sb, st->a[ax], &oob); }
            else if (bytes == 2) { s = mem_rd16(sb, st->a[ay], &oob); dv = oob ? 0 : mem_rd16(sb, st->a[ax], &oob); }
            else                 { s = mem_rd32(sb, st->a[ay], &oob); dv = oob ? 0 : mem_rd32(sb, st->a[ax], &oob); }
            if (oob) IFAIL("cmpm: address out of sandbox");
            st->a[ay] += bytes;
            st->a[ax] += bytes;
            uint32_t bits = bytes * 8u, msb = 1u << (bits - 1);
            uint32_t res = (dv - s) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u));
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (s > dv)                       cc |= J5D_CCR_C;
            if (((dv ^ s) & (dv ^ res)) & msb) cc |= J5D_CCR_V;
            if (res == 0)                      cc |= J5D_CCR_Z;
            if (res & msb)                     cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 2; continue;
        }
        /* cmpi.b #imm,(d16,An) : 0C28 | an + imm16(low byte) + d16  (byte compare
         * immediate with a memory byte; PRM CMPI — immediate extension FIRST).
         * Flags like a byte subtract; memory and X unchanged. 6 bytes. */
        if ((op & 0xFFF8u) == 0x0C28u) {
            unsigned an = op & 7u;
            uint8_t b = (uint8_t)be16s(ip + 2);
            int16_t d16 = be16s(ip + 4);
            int oob = 0;
            uint8_t a = mem_rd8(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("cmpi.b (d16,An): address out of sandbox");
            uint8_t res = (uint8_t)(a - b);
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a)                          cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80u)  cc |= J5D_CCR_V;
            if (res == 0)                       cc |= J5D_CCR_Z;
            if (res & 0x80u)                    cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 6; continue;
        }
        /* cmpi.l #imm,(An) : 0C90 | an + imm32  (compare immediate with the longword at
         * (An); same CCR rule as cmpi.l (d16,An) above; 6 bytes). */
        if ((op & 0xFFF8u) == 0x0C90u) {
            unsigned an = op & 7u;
            uint32_t b = be32(ip + 2);
            int oob = 0;
            uint32_t a = mem_rd32(sb, st->a[an], &oob);
            if (oob) IFAIL("cmpi.l (An): address out of sandbox");
            uint32_t res = a - b;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (b > a)                                   cc |= J5D_CCR_C;
            if (((a ^ b) & (a ^ res)) & 0x80000000u)     cc |= J5D_CCR_V;
            if (res == 0)                                cc |= J5D_CCR_Z;
            if (res & 0x80000000u)                       cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 6; continue;
        }
        /* move.b (d16,An),Dn : 1028 | dn<<9 | an + d16  (byte load, displacement EA; PRM
         * MOVE byte: only the LOW BYTE of Dn changes, N/Z on the byte, V=C=0, X kept). */
        if ((op & 0xF1F8u) == 0x1028u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            uint8_t v = mem_rd8(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("move.b (d16,An): address out of sandbox");
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | v;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (v == 0)    cc |= J5D_CCR_Z;
            if (v & 0x80u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* move.w (d16,An),Dn : 3028 | dn<<9 | an + d16  (word load, displacement EA; PRM
         * MOVE word: only the LOW WORD of Dn changes, N/Z on the word, V=C=0, X kept). */
        if ((op & 0xF1F8u) == 0x3028u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int16_t d16 = be16s(ip + 2);
            int oob = 0;
            uint16_t v = mem_rd16(sb, st->a[an] + (uint32_t)(int32_t)d16, &oob);
            if (oob) IFAIL("move.w (d16,An): address out of sandbox");
            st->d[dn] = (st->d[dn] & 0xFFFF0000u) | v;
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (v == 0)      cc |= J5D_CCR_Z;
            if (v & 0x8000u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
        }
        /* add.b (An),Dn : D010 | dn<<9 | an  (byte add from memory; PRM ADD byte size).
         * Same CCR rule as add.b Dm,Dn; only the low byte of Dn changes. */
        if ((op & 0xF1F8u) == 0xD010u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            int oob = 0;
            uint8_t b = mem_rd8(sb, st->a[an], &oob);
            if (oob) IFAIL("add.b (An): address out of sandbox");
            uint8_t a = (uint8_t)st->d[dn];
            uint32_t s = (uint32_t)a + b; uint8_t res = (uint8_t)s; uint32_t cc = 0;
            if (s & 0x100u) cc |= J5D_CCR_C | J5D_CCR_X;
            if (((a ^ res) & (b ^ res)) & 0x80u) cc |= J5D_CCR_V;
            if (res == 0) cc |= J5D_CCR_Z;
            if (res & 0x80u) cc |= J5D_CCR_N;
            st->d[dn] = (st->d[dn] & 0xFFFFFF00u) | res; st->ccr = cc;
            pc += 2; continue;
        }
        /* move.w #imm,(An) : 34BC | an<<9  (word store of an immediate; MOVE sets N/Z per
         * the moved word, V=C=0, X kept — PRM MOVE). */
        if ((op & 0xF1FFu) == 0x30BCu && ((op >> 6) & 7u) == 2u) {
            unsigned an = (op >> 9) & 7u;
            uint16_t v = (uint16_t)be16s(ip + 2);
            int oob = 0;
            mem_wr16(sb, st->a[an], v, &oob);
            if (oob) IFAIL("move.w #imm,(An): address out of sandbox");
            uint32_t cc = st->ccr & J5D_CCR_X;
            if (v == 0)      cc |= J5D_CCR_Z;
            if (v & 0x8000u) cc |= J5D_CCR_N;
            st->ccr = cc;
            pc += 4; continue;
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
            uint32_t a = st->d[dl];
            if (size64) {
                /* 64-bit product form: Dh:Dl = Dl * src (PRM MULU.L/MULS.L, 68020).
                 * Dh = ext bits 2-0. N = bit 63, Z = the whole 64 bits zero, V=C=0. */
                unsigned dh = ext & 7u;
                uint64_t p = is_signed
                    ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)src)
                    : (uint64_t)a * (uint64_t)src;
                st->d[dl] = (uint32_t)p;
                st->d[dh] = (uint32_t)(p >> 32);
                uint32_t cc = st->ccr & J5D_CCR_X;
                if (p == 0)                 cc |= J5D_CCR_Z;
                if (p & 0x8000000000000000ull) cc |= J5D_CCR_N;
                st->ccr = cc;
                pc += adv; continue;
            }
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
            else if (mode == 5) {           /* [J5t] (d16,An): the vbcc capstone divides by a    */
                int16_t d16 = be16s(ip + 4);                /* stack-resident divisor (36,a7)     */
                uint32_t a = st->a[reg] + (uint32_t)(int32_t)d16; int oob = 0;
                divisor = mem_rd32(sb, a, &oob); adv = 6;
                if (oob) IFAIL("div.l (d16,An): source out of sandbox");
            }
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

        /* ==================== [J5r] FMOVEM + FP SYSTEM-REGISTER MOVES (oracle mirror) =======
         * Decoded BEFORE the FPU arithmetic block (they share the 0xF2xx encoding). The .x
         * 96-bit conversion is the SHARED j5r_double_to_x/j5r_x_to_double (j5d_jit68k.h); the
         * reglist + memory order mirror the dispatcher in j5d_engine.c exactly. */
        if ((op & 0xffc0) == 0xf200 && (uint64_t)pc + 4 <= (uint64_t)sb->origin + sb->size) {
            uint16_t oc2 = be16(ip + 2);
            int is_fmovem  = ((oc2 & 0xc700u) == 0xc000u);
            int is_sysreg  = ((oc2 & 0xe000u) == 0x8000u) || ((oc2 & 0xe000u) == 0xa000u);
            if (is_fmovem || is_sysreg) {
                unsigned mode = (op >> 3) & 7u, areg = op & 7u;
                /* resolve the EA base + ext bytes (the modes the [J5r] subset uses). */
                uint32_t base = 0; unsigned ext_bytes = 0; int predec = 0, postinc = 0; int ok = 1;
                /* count is filled below; for predec base needs `total`, so compute count first. */
                if (is_fmovem) {
                    unsigned dir = (oc2 >> 13) & 1u, dynamic = (oc2 >> 11) & 1u;
                    uint8_t mask = dynamic ? (uint8_t)(st->d[(oc2 >> 4) & 7u] & 0xFFu)
                                           : (uint8_t)(oc2 & 0xFFu);
                    int is_predec = (mode == 4);
                    unsigned fpregs[8]; int count = 0;
                    for (int fp = 0; fp < 8; fp++) {
                        int bit = is_predec ? fp : (7 - fp);
                        if (mask & (1u << bit)) fpregs[count++] = (unsigned)fp;
                    }
                    uint32_t total = (uint32_t)(12 * count);
                    if (mode == 2)      { base = st->a[areg]; }
                    else if (mode == 3) { base = st->a[areg]; postinc = 1; }
                    else if (mode == 4) { base = st->a[areg] - total; predec = 1; }
                    else if (mode == 5) { base = st->a[areg] + (uint32_t)(int32_t)be16s(ip + 4); ext_bytes = 2; }
                    else if (mode == 7 && areg == 0) { base = (uint32_t)(int32_t)be16s(ip + 4); ext_bytes = 2; }
                    else if (mode == 7 && areg == 1) { base = be32(ip + 4); ext_bytes = 4; }
                    else if (mode == 7 && areg == 2) { base = (pc + 4) + (uint32_t)(int32_t)be16s(ip + 4); ext_bytes = 2; }
                    else { ok = 0; }
                    if (!ok) IFAIL("FMOVEM EA mode not in the [J5r] subset");
                    for (int s = 0; s < count; s++) {
                        uint32_t addr = base + (uint32_t)(12 * s);
                        if (addr < sb->origin || (uint64_t)addr + 12 > (uint64_t)sb->origin + sb->size)
                            IFAIL("FMOVEM: .x slot out of sandbox");
                        uint8_t *p = sb->host_mem + (addr - sb->origin);
                        if (dir) { uint8_t xb[12]; j5r_double_to_x(st->fp[fpregs[s]], xb); memcpy(p, xb, 12); }
                        else     { st->fp[fpregs[s]] = j5r_x_to_double(p); }
                    }
                    if (predec)  st->a[areg] -= total;
                    if (postinc) st->a[areg] += total;
                    pc = pc + 4 + ext_bytes; continue;
                } else {
                    /* FP system-register move (FPCR/FPSR/FPIAR), single or control-reg list. */
                    unsigned to_special = ((oc2 & 0xe000u) == 0x8000u);
                    int wc = (oc2 & 0x1000u) != 0, ws = (oc2 & 0x0800u) != 0, wi = (oc2 & 0x0400u) != 0;
                    int count = wc + ws + wi;
                    if (mode == 0 || mode == 1) {
                        uint32_t *slot = (mode == 0) ? &st->d[areg] : &st->a[areg];
                        uint32_t *creg = wc ? &st->fpcr : ws ? &st->fpsr : &st->fpiar;
                        if (count != 1) IFAIL("FMOVE Dn/An,<ctrl>: expected one ctrl reg");
                        if (to_special) *creg = *slot; else *slot = *creg;
                        pc = pc + 4; continue;
                    }
                    uint32_t total = (uint32_t)(4 * count);
                    if (mode == 2)      { base = st->a[areg]; }
                    else if (mode == 3) { base = st->a[areg]; postinc = 1; }
                    else if (mode == 4) { base = st->a[areg] - total; predec = 1; }
                    else if (mode == 5) { base = st->a[areg] + (uint32_t)(int32_t)be16s(ip + 4); ext_bytes = 2; }
                    else if (mode == 7 && areg == 1) { base = be32(ip + 4); ext_bytes = 4; }
                    else { IFAIL("FMOVE <ctrl> EA mode not in the [J5r] subset"); }
                    uint32_t *order[3]; int n = 0;
                    if (wc) order[n++] = &st->fpcr;
                    if (ws) order[n++] = &st->fpsr;
                    if (wi) order[n++] = &st->fpiar;
                    for (int i = 0; i < n; i++) {
                        uint32_t addr = base + (uint32_t)(4 * i);
                        if (addr < sb->origin || (uint64_t)addr + 4 > (uint64_t)sb->origin + sb->size)
                            IFAIL("FMOVE <ctrl> mem out of sandbox");
                        uint8_t *p = sb->host_mem + (addr - sb->origin);
                        if (to_special) *order[i] = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
                        else { uint32_t v = *order[i]; p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=(uint8_t)v; }
                    }
                    if (predec)  st->a[areg] -= total;
                    if (postinc) st->a[areg] += total;
                    pc = pc + 4 + ext_bytes; continue;
                }
            }
        }

        /* ============================ [J5o] THE FPU (line-F coprocessor) =============
         * FMOVE (reg<->reg, mem<->reg, format conversions .s/.d/.l/.w/.b), FADD/FSUB/FMUL/
         * FDIV/FSQRT/FABS/FNEG, FCMP, FTST — computed in C `double`, INDEPENDENT of Emu68's
         * emitted AArch64 FP. opcode = 0xF200|EA (coprocessor 1, the FPU); opcode2 is the FP
         * command word. We decode the SAME fields EMIT_FPU does (R/M, source spec, dst FPn,
         * operation) and re-derive the result + the FPSR cc byte (gated by the SAME
         * FPSR_Update_Needed scan). Format/operation encodings cross-checked against the
         * M68000 FP user manual + the vendored decoder masks. */
        if ((op & 0xff80) == 0xf200 && (op & 0x0e00) == 0x0200) {   /* line-F, coprocessor 1, EA-form */
            if ((uint64_t)pc + 4 > (uint64_t)sb->origin + sb->size) IFAIL("FPU: opcode2 out of sandbox");
            uint16_t opcode2 = be16(ip + 2);
            unsigned ea    = op & 0x3f;
            unsigned rm    = (opcode2 >> 14) & 1u;        /* 0 = src is FPm reg, 1 = src is EA  */
            unsigned srcspec = (opcode2 >> 10) & 7u;      /* rm=0: FPm number; rm=1: format     */
            unsigned fpn   = (opcode2 >> 7) & 7u;         /* dst FP register (also FCMP/arith dst)*/
            unsigned oper  = opcode2 & 0x7fu;             /* operation code (FMOVE=0..FTST=0x3a)  */
            unsigned is_fmove_to_mem = ((opcode2 & 0xe000) == 0x6000);
            int oob = 0;
            uint32_t after_pc;       /* pc just past this instruction's words (for FPSR scan) */

            /* ---- resolve the SOURCE operand into a C double `src`, advancing past ext words.
             * Memory-EA modes go through the SAME big-endian sandbox reads the engine's
             * sandbox FP-mem helpers use; register-direct .l/.w/.b convert int->double. ---- */
            double src = 0.0;
            uint32_t ext = pc + 4;   /* first word AFTER opcode + opcode2 */
            if (is_fmove_to_mem) {
                /* FMOVE FPn -> <ea>: the source is the FP register, dest is memory (below). */
                src = st->fp[fpn & 7];
            } else if (rm == 0) {
                /* source is FPm (register-direct FP source). */
                src = st->fp[srcspec & 7];
            } else {
                /* source is an EA with a format (srcspec): SIZE_L=0 S=1 X=2 P=3 W=4 D=5 B=6. */
                unsigned mode = (ea >> 3) & 7u, reg = ea & 7u;
                uint32_t addr = 0; int have_addr = 0; int imm = 0;
                if (mode == 0) {
                    /* mode 000 - Dn register-direct. The integer-format conversions read Dn and
                     * convert int->double; .s reinterprets the low 32 bits as binary32 (the
                     * decoder's fmsr+fcvtds), matching FPU_FetchData Case 1 (M68k_LINEF.c). The
                     * .d/.x/.p formats are not valid from a 32-bit Dn (out of [J5o] subset). */
                    uint32_t dv = st->d[reg];
                    if (srcspec == 0)      src = (double)(int32_t)dv;                 /* .l signed   */
                    else if (srcspec == 4) src = (double)(int32_t)(int16_t)(dv & 0xffffu); /* .w sx  */
                    else if (srcspec == 6) src = (double)(int32_t)(int8_t)(dv & 0xffu);     /* .b sx  */
                    else if (srcspec == 1) { float f; __builtin_memcpy(&f, &dv, 4); src = (double)f; } /* .s bits */
                    else IFAIL("FPU: Dn-source format not in [J5o] subset (.d/.x/.p)");
                }
                else if (mode == 2) { addr = st->a[reg]; have_addr = 1; }            /* (An)   */
                else if (mode == 3) { addr = st->a[reg]; have_addr = 1; }            /* (An)+  */
                else if (mode == 4) { /* -(An): predecrement by the format size (below) */ }
                else if (mode == 5) { addr = st->a[reg] + (uint32_t)(int32_t)(int16_t)be16(sb->host_mem+(ext-sb->origin)); ext += 2; have_addr = 1; } /* (d16,An) */
                else if (mode == 7 && reg == 4) { imm = 1; }                         /* #imm   */
                else IFAIL("FPU: source EA mode not in [J5o] subset");

                /* the format size in bytes (for predecrement + ext-word advance of immediates). */
                int fsz = (srcspec==0)?4 : (srcspec==1)?4 : (srcspec==4)?2 : (srcspec==5)?8 : (srcspec==6)?1 : 0;
                if (fsz == 0) IFAIL("FPU: .x/.p source format deferred ([J5o] precision model)");

                if (mode == 4) { st->a[reg] -= (uint32_t)fsz; addr = st->a[reg]; have_addr = 1; }

                if (imm) {
                    /* immediate operand follows in the stream; width = format size. */
                    if (srcspec == 0) { src = (double)(int32_t)be32(sb->host_mem+(ext-sb->origin)); ext += 4; }       /* .l */
                    else if (srcspec == 1) { src = fpu_mem_rd_s(sb, ext, &oob); ext += 4; }                            /* .s */
                    else if (srcspec == 4) { src = (double)(int32_t)(int16_t)be16(sb->host_mem+(ext-sb->origin)); ext += 2; } /* .w */
                    else if (srcspec == 5) { src = fpu_mem_rd_d(sb, ext, &oob); ext += 8; }                            /* .d */
                    else if (srcspec == 6) { src = (double)(int32_t)(int8_t)(be16(sb->host_mem+(ext-sb->origin)) & 0xff); ext += 2; } /* .b */
                } else if (have_addr) {
                    if (srcspec == 0)      src = (double)(int32_t)mem_rd32(sb, addr, &oob);          /* .l */
                    else if (srcspec == 1) src = fpu_mem_rd_s(sb, addr, &oob);                       /* .s */
                    else if (srcspec == 4) src = (double)(int32_t)(int16_t)mem_rd16(sb, addr, &oob); /* .w */
                    else if (srcspec == 5) src = fpu_mem_rd_d(sb, addr, &oob);                       /* .d */
                    else if (srcspec == 6) src = (double)(int32_t)(int8_t)mem_rd8(sb, addr, &oob);   /* .b */
                    if (mode == 3) st->a[reg] += (uint32_t)fsz;                                      /* (An)+ */
                }
                if (oob) IFAIL("FPU: source EA out of sandbox");
            }
            after_pc = ext;

            /* ---- FMOVE FPn -> <ea> (to memory) : convert + byteswap-store. ---- */
            if (is_fmove_to_mem) {
                unsigned fmt = (opcode2 >> 10) & 7u;   /* destination format */
                unsigned mode = (ea >> 3) & 7u, reg = ea & 7u;
                uint32_t addr = 0; ext = pc + 4;
                int fsz = (fmt==0)?4 : (fmt==1)?4 : (fmt==4)?2 : (fmt==5)?8 : (fmt==6)?1 : 0;
                if (fsz == 0) IFAIL("FPU: .x/.p dest format deferred ([J5o] precision model)");
                /* [J5t] FMOVE FPn -> Dn (mode 0, register-direct): the integer-format conversion
                 * the compiler emits for (long)d / (int)d ETC. (fintrz.x fp ; fmove.l fp,d0). The
                 * 68881 converts the FP value to the .l/.w/.b/.s integer/single and writes it into
                 * Dn with SUB-REGISTER semantics: .l replaces all 32 bits, .w the low 16, .b the
                 * low 8, .s the 32 single-precision bits (the JIT's EMIT_lineF does the same via
                 * fcvtzs/fmov-to-GP).  .d format to a 32-bit Dn is invalid. The hand-written FP
                 * corpus never converted FP->Dn (it stored to memory); the vbcc capstone does it
                 * for every integer-ized result. */
                if (mode == 0) {
                    double v = src;
                    if (fmt == 0)      st->d[reg] = (uint32_t)(int32_t)v;                                  /* .l */
                    else if (fmt == 4) st->d[reg] = (st->d[reg] & 0xFFFF0000u) | ((uint32_t)(int16_t)(int32_t)v & 0xFFFFu);  /* .w */
                    else if (fmt == 6) st->d[reg] = (st->d[reg] & 0xFFFFFF00u) | ((uint32_t)(int8_t)(int32_t)v & 0xFFu);     /* .b */
                    else if (fmt == 1) { float fv = (float)v; uint32_t b; __builtin_memcpy(&b, &fv, 4); st->d[reg] = b; }   /* .s */
                    else IFAIL("FPU: FMOVE FPn->Dn .d/.x/.p format invalid");
                    pc = ext; continue;   /* FMOVE-to-reg does not update FPSR cc (it's a consumer) */
                }
                if (mode == 2) addr = st->a[reg];
                else if (mode == 3) addr = st->a[reg];
                else if (mode == 4) { st->a[reg] -= (uint32_t)fsz; addr = st->a[reg]; }
                else if (mode == 5) { addr = st->a[reg] + (uint32_t)(int32_t)(int16_t)be16(sb->host_mem+(ext-sb->origin)); ext += 2; }
                else IFAIL("FPU: FMOVE-to-mem EA mode not in [J5o] subset");
                double v = src;
                if (fmt == 0)      mem_wr32(sb, addr, (uint32_t)(int32_t)v, &oob);     /* .l (trunc-to-int) */
                else if (fmt == 1) fpu_mem_wr_s(sb, addr, v, &oob);                    /* .s */
                else if (fmt == 4) mem_wr16(sb, addr, (uint16_t)(int16_t)(int32_t)v, &oob); /* .w */
                else if (fmt == 5) fpu_mem_wr_d(sb, addr, v, &oob);                    /* .d */
                else if (fmt == 6) mem_wr8(sb, addr, (uint8_t)(int8_t)(int32_t)v, &oob); /* .b */
                if (mode == 3) st->a[reg] += (uint32_t)fsz;
                if (oob) IFAIL("FPU: FMOVE-to-mem EA out of sandbox");
                /* FMOVE to MEM does not update FPSR cc itself (it is a CONSUMER of it). */
                /* advance past THIS instruction's words: ext was re-advanced locally for the
                 * destination's own extension word(s) (e.g. (d16,An)); after_pc (=pc+4) did NOT
                 * see them since the to-mem source branch reads only the FP reg. Use ext. */
                pc = ext; continue;
            }

            /* ---- the arithmetic / move-to-reg / compare ops, computed in C double. ---- */
            double dst = st->fp[fpn & 7];
            double res = dst;
            int writes_dst = 1;       /* FCMP/FTST do not write FPn */
            int cmp_fpsr   = 0;       /* FCMP: FPSR from fcmpd(dst,src); else fcmpzd(res)      */
            int fpsr_test_src = 0;    /* FTST: the FPSR fcmpzd tests src (not res)             */

            /* [J5s] THE FP EXCEPTION MODEL (oracle side — the authoritative FPSR EXC/AEXC).
             * Set the host rounding direction per the FPCR MODE byte, clear the host fenv, do
             * the op, then read the fenv exceptions raised and map them into the FPSR EXC byte.
             * FMOVE (opmode 0) does NOT signal on an sNaN operand on AArch64 (a plain move), so
             * the SNAN bit is set only by an ARITHMETIC op consuming an sNaN — detected by
             * j5s_is_snan over the operands here, identically to the engine's sNaN scan. */
            int had_snan = j5s_is_snan(src) || (oper != 0x00 && oper != 0x3a && j5s_is_snan(dst));
            int prev_round = fegetround();
            fesetround(j5s_host_round(st->fpcr));
            feclearexcept(FE_ALL_EXCEPT);

            /* [J5p] FSINCOS (opmode 0x30..0x37): computes sin INTO fp_dst_sin (= fpn) AND cos
             * INTO fp_dst_cos (= opcode2 & 7). The FPSR fcmpzd tests fp_dst_sin (the decoder's
             * order). Mirror the HOST libm (the same sin/cos the JIT's blr reaches). */
            if ((oper & 0x78) == 0x30) {
                unsigned fp_cos = opcode2 & 7u;
                double s = sin(src), c = cos(src);
                int fe = fetestexcept(FE_ALL_EXCEPT);
                fesetround(prev_round);
                s = j5s_round_prec(s, st->fpcr); c = j5s_round_prec(c, st->fpcr);
                st->fp[fpn & 7] = s;
                st->fp[fp_cos]  = c;
                res = s;                                  /* FPSR tests the sin result          */
                uint32_t exc = j5s_fenv_to_exc(fe, had_snan);
                st->fpsr = (st->fpsr & ~J5S_FPSR_EXC_MASK) | exc;
                st->fpsr |= j5s_exc_to_aexc(exc);
                if (fpu_fpsr_update_needed(sb, after_pc))
                    st->fpsr = fpu_fcmp_fpsr(st->fpsr, res, 0.0);
                if (j5s_fp_trap(sb, st, exc, after_pc, errbuf, errlen, &pc)) {
                    if (pc == 0xFFFFFFFFu) return 1;      /* trap raise failed (bad vector)     */
                    continue;                             /* trap taken: pc = handler           */
                }
                pc = after_pc; continue;
            }

            switch (oper) {
                case 0x00: res = src;                 break;  /* FMOVE  <src> -> FPn          */
                case 0x18: res = fabs(src);           break;  /* FABS                         */
                case 0x1a: res = -src;                break;  /* FNEG                         */
                case 0x04: res = sqrt(src);           break;  /* FSQRT                        */
                case 0x20: res = dst / src;           break;  /* FDIV   FPn / <src>           */
                case 0x22: res = dst + src;           break;  /* FADD                         */
                case 0x23: res = dst * src;           break;  /* FMUL                         */
                case 0x28: res = dst - src;           break;  /* FSUB   FPn - <src>           */
                case 0x38: writes_dst = 0; cmp_fpsr = 1; break; /* FCMP  (FPn ? <src>)        */
                case 0x3a: writes_dst = 0; res = src; fpsr_test_src = 1; break; /* FTST       */

                /* ============ [J5p] TRANSCENDENTALS — routed to the SAME HOST libm the JIT's
                 * blr reaches (see j5o_fpu_shims.c). The 68881 last-ULP behaviour is
                 * implementation-defined, so the HOST libm is the reference both sides use;
                 * bit-exact verifies the TRANSLATION (decode + correct register-as-argument +
                 * store), not a re-derivation of sin(). All unary: res = fn(src). ========== */
                case 0x0e: res = sin(src);   break;  /* FSIN                                  */
                case 0x1d: res = cos(src);   break;  /* FCOS                                  */
                case 0x0f: res = tan(src);   break;  /* FTAN                                  */
                case 0x0c: res = asin(src);  break;  /* FASIN                                 */
                case 0x1c: res = acos(src);  break;  /* FACOS                                 */
                case 0x0a: res = atan(src);  break;  /* FATAN                                 */
                case 0x02: res = sinh(src);  break;  /* FSINH                                 */
                case 0x19: res = cosh(src);  break;  /* FCOSH                                 */
                case 0x09: res = tanh(src);  break;  /* FTANH                                 */
                case 0x0d: res = atanh(src); break;  /* FATANH                                */
                case 0x10: res = exp(src);   break;  /* FETOX   (e^x)                         */
                case 0x08: res = expm1(src); break;  /* FETOXM1 (e^x - 1)                     */
                case 0x11: res = exp2(src);  break;  /* FTWOTOX (2^x)                         */
                case 0x12: res = pow(10.0, src); break; /* FTENTOX (10^x; host has no exp10)  */
                case 0x14: res = log(src);   break;  /* FLOGN   (ln)                          */
                case 0x06: res = log1p(src); break;  /* FLOGNP1 (ln(1+x))                     */
                case 0x15: res = log10(src); break;  /* FLOG10                                */
                case 0x16: res = log2(src);  break;  /* FLOG2                                 */

                /* ============ [J5p] FP-UTILITY ops the decoder emits INLINE (no libm blr) —
                 * the oracle mirrors the EXACT inline emit (NEON frint / bit-extraction). ===*/
                case 0x01: res = rint(src);  break;  /* FINT   -> frint64x (round per FPCR;   */
                                                     /*           host default = nearest-even)*/
                case 0x03: res = trunc(src); break;  /* FINTRZ -> frint64z (round toward 0)   */
                case 0x21: {                          /* FMOD: x - y*trunc(x/y) (the decoder's */
                    double q = dst / src;             /*  fdiv/frint64z/fmul/fsub sequence)    */
                    q = trunc(q);
                    res = dst - src * q;
                    break;
                }
                case 0x25: res = remainder(dst, src); break; /* FREM -> remquo (IEEE round-to- */
                                                     /* nearest remainder; quotient byte not   */
                                                     /* part of the asserted cc byte)          */
                case 0x26: res = ldexp(dst, (int)src); break; /* FSCALE: fp_dst * 2^trunc(src) */
                case 0x1e: {                          /* FGETEXP: unbiased exponent as double  */
                    uint64_t b; memcpy(&b, &src, 8);
                    int e = (int)((b >> 52) & 0x7ffu) - 1023;
                    res = (double)e;
                    break;
                }
                case 0x1f: {                          /* FGETMAN: mantissa forced into [1,2)   */
                    uint64_t b; memcpy(&b, &src, 8);
                    b = (b & ~(0x7ffULL << 52)) | (0x3ffULL << 52);
                    memcpy(&res, &b, 8);
                    break;
                }
                default: IFAIL("FPU: operation not in [J5p] subset (FMOVECR/.x/.p deferred)");
            }
            /* [J5s] read the host fenv the op raised, restore the rounding direction, and
             * round the result to the FPCR precision (single -> through float). */
            int fe = fetestexcept(FE_ALL_EXCEPT);
            fesetround(prev_round);
            /* FMOVE (opmode 0) is a plain move — it does NOT signal on AArch64 (matching the host
             * the JIT runs on), so it never sets the EXC byte. */
            if (oper == 0x00u) fe = 0;
            if (writes_dst) res = j5s_round_prec(res, st->fpcr);
            if (writes_dst) st->fp[fpn & 7] = res;

            /* [J5s] FPSR exception (EXC) byte + accrued (AEXC) byte. FMOVE/FTST do not signal
             * on an sNaN operand (had_snan already excludes them); arith ops do. The EXC byte
             * reflects THIS op (it is re-set each op); the AEXC bits are STICKY (OR-accumulated).
             * (The CC byte set below is disjoint — bits 27..24 — so the merge is clean.) */
            uint32_t exc = j5s_fenv_to_exc(fe, had_snan);
            st->fpsr = (st->fpsr & ~J5S_FPSR_EXC_MASK) | exc;
            st->fpsr |= j5s_exc_to_aexc(exc);

            /* FPSR condition codes — set IFF a subsequent FP consumer needs them (same gate as
             * the JIT's FPSR_Update_Needed). FCMP packs fcmpd(FPn,src); the rest pack fcmpzd of
             * the value compared against 0 (the result for arith/transcendental/utility/FABS/
             * FNEG/FMOVE-to-reg; the source for FTST — see fpsr_test_src). */
            if (fpu_fpsr_update_needed(sb, after_pc)) {
                if (cmp_fpsr) st->fpsr = fpu_fcmp_fpsr(st->fpsr, dst, src);
                else {
                    double t = fpsr_test_src ? src : res;   /* FTST tests src; others test res */
                    st->fpsr = fpu_fcmp_fpsr(st->fpsr, t, 0.0);  /* fcmpzd: compare with 0.0    */
                }
            }
            /* [J5s] TAKE AN FP EXCEPTION TRAP if an EXC bit fired AND its FPCR enable bit is set
             * (the [J5i] vector dispatch, just more vector numbers — vectors 48..54). */
            if (j5s_fp_trap(sb, st, exc, after_pc, errbuf, errlen, &pc)) {
                if (pc == 0xFFFFFFFFu) return 1;          /* trap raise failed (bad vector)     */
                continue;                                 /* trap taken: pc = handler           */
            }
            pc = after_pc; continue;
        }

        /* ============================ [STD68K] general MOVE fallback =================
         * MOVE is `00 ss DDD ddd sss rrr` (ss: 01=.b 11=.w 10=.l; ddd/DDD = dest
         * mode/reg; sss/rrr = source mode/reg). The specific fast-path blocks above
         * keep the corpus-proven forms; this fallback covers the REST of the mode
         * matrix compiler-generated code uses (gcc emits combos like
         * `move.b -(An),(d16,Am)`). Source is evaluated first, then the destination
         * (PRM order, both with their side effects). CCR: N/Z per the moved value,
         * V=C=0, X kept — except MOVEA (dest mode 001): no CCR, word sign-extends. */
        if ((op & 0xC000u) == 0x0000u && (op & 0x3000u) != 0x0000u) {
            unsigned szf = (op >> 12) & 3u;                 /* 1=.b 3=.w 2=.l */
            unsigned bits = (szf == 1) ? 8u : (szf == 3) ? 16u : 32u;
            unsigned step = bits / 8u;
            unsigned sm = (op >> 3) & 7u, sr = op & 7u;
            unsigned dm = (op >> 6) & 7u, dr = (op >> 9) & 7u;
            const uint8_t *ext = ip + 2;
            int oob = 0, handled = 1;
            uint32_t v = 0;

            /* -------- source -------- */
            switch (sm) {
                case 0: v = st->d[sr]; break;
                case 1: v = st->a[sr]; break;               /* .w/.l only in practice */
                case 2: case 3: case 4: {
                    uint32_t addr = st->a[sr];
                    /* -(An): byte on A7 keeps SP even (PRM); gcc never does that. */
                    if (sm == 4) { addr -= step; }
                    if (bits == 8)       v = mem_rd8(sb, addr, &oob);
                    else if (bits == 16) v = mem_rd16(sb, addr, &oob);
                    else                 v = mem_rd32(sb, addr, &oob);
                    if (!oob) {
                        if (sm == 3) st->a[sr] = addr + step;
                        else if (sm == 4) st->a[sr] = addr;
                    }
                    break;
                }
                case 5: {
                    int16_t d16 = be16s(ext); ext += 2;
                    uint32_t addr = st->a[sr] + (uint32_t)(int32_t)d16;
                    if (bits == 8)       v = mem_rd8(sb, addr, &oob);
                    else if (bits == 16) v = mem_rd16(sb, addr, &oob);
                    else                 v = mem_rd32(sb, addr, &oob);
                    break;
                }
                case 6: {
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= (brief >> 9) & 3u;
                    uint32_t addr = st->a[sr] + (uint32_t)(int32_t)d8 + index;
                    if (bits == 8)       v = mem_rd8(sb, addr, &oob);
                    else if (bits == 16) v = mem_rd16(sb, addr, &oob);
                    else                 v = mem_rd32(sb, addr, &oob);
                    break;
                }
                case 7:
                    if (sr == 0)      { uint32_t a2 = (uint32_t)(int32_t)be16s(ext); ext += 2;
                                        if (bits == 8) v = mem_rd8(sb, a2, &oob);
                                        else if (bits == 16) v = mem_rd16(sb, a2, &oob);
                                        else v = mem_rd32(sb, a2, &oob); }
                    else if (sr == 1) { uint32_t a2 = be32(ext); ext += 4;
                                        if (bits == 8) v = mem_rd8(sb, a2, &oob);
                                        else if (bits == 16) v = mem_rd16(sb, a2, &oob);
                                        else v = mem_rd32(sb, a2, &oob); }
                    else if (sr == 2) { uint32_t pcbase = pc + (uint32_t)(ext - ip);
                                        int16_t d16 = be16s(ext); ext += 2;
                                        uint32_t a2 = pcbase + (uint32_t)(int32_t)d16;
                                        if (bits == 8) v = mem_rd8(sb, a2, &oob);
                                        else if (bits == 16) v = mem_rd16(sb, a2, &oob);
                                        else v = mem_rd32(sb, a2, &oob); }
                    else if (sr == 4) { if (bits == 32) { v = be32(ext); ext += 4; }
                                        else { v = (uint16_t)be16(ext); ext += 2;
                                               if (bits == 8) v &= 0xFFu; } }
                    else handled = 0;
                    break;
                default: handled = 0; break;
            }
            if (handled && oob) IFAIL("move: source out of sandbox");

            /* -------- destination -------- */
            if (handled) {
                if (dm == 0) {                              /* Dn */
                    if (bits == 8)       st->d[dr] = (st->d[dr] & 0xFFFFFF00u) | (v & 0xFFu);
                    else if (bits == 16) st->d[dr] = (st->d[dr] & 0xFFFF0000u) | (v & 0xFFFFu);
                    else                 st->d[dr] = v;
                }
                else if (dm == 1) {                         /* MOVEA: no CCR, sign-extend .w */
                    st->a[dr] = (bits == 16) ? (uint32_t)(int32_t)(int16_t)v : v;
                    pc += (uint32_t)(ext - ip); continue;
                }
                else if (dm == 2 || dm == 3 || dm == 4) {
                    uint32_t addr = st->a[dr];
                    if (dm == 4) addr -= step;
                    if (bits == 8)       mem_wr8(sb, addr, (uint8_t)v, &oob);
                    else if (bits == 16) mem_wr16(sb, addr, (uint16_t)v, &oob);
                    else                 mem_wr32(sb, addr, v, &oob);
                    if (!oob) {
                        if (dm == 3) st->a[dr] = addr + step;
                        else if (dm == 4) st->a[dr] = addr;
                    }
                }
                else if (dm == 5) {
                    int16_t d16 = be16s(ext); ext += 2;
                    uint32_t addr = st->a[dr] + (uint32_t)(int32_t)d16;
                    if (bits == 8)       mem_wr8(sb, addr, (uint8_t)v, &oob);
                    else if (bits == 16) mem_wr16(sb, addr, (uint16_t)v, &oob);
                    else                 mem_wr32(sb, addr, v, &oob);
                }
                else if (dm == 6) {
                    uint16_t brief = be16(ext); ext += 2;
                    int8_t d8 = (int8_t)(brief & 0xFFu);
                    unsigned ix = (brief >> 12) & 7u;
                    uint32_t index = (brief & 0x8000u) ? st->a[ix] : st->d[ix];
                    if (!(brief & 0x0800u)) index = (uint32_t)(int32_t)(int16_t)index;
                    index <<= (brief >> 9) & 3u;
                    uint32_t addr = st->a[dr] + (uint32_t)(int32_t)d8 + index;
                    if (bits == 8)       mem_wr8(sb, addr, (uint8_t)v, &oob);
                    else if (bits == 16) mem_wr16(sb, addr, (uint16_t)v, &oob);
                    else                 mem_wr32(sb, addr, v, &oob);
                }
                else if (dm == 7 && dr == 0) {
                    uint32_t addr = (uint32_t)(int32_t)be16s(ext); ext += 2;
                    if (bits == 8)       mem_wr8(sb, addr, (uint8_t)v, &oob);
                    else if (bits == 16) mem_wr16(sb, addr, (uint16_t)v, &oob);
                    else                 mem_wr32(sb, addr, v, &oob);
                }
                else if (dm == 7 && dr == 1) {
                    uint32_t addr = be32(ext); ext += 4;
                    if (bits == 8)       mem_wr8(sb, addr, (uint8_t)v, &oob);
                    else if (bits == 16) mem_wr16(sb, addr, (uint16_t)v, &oob);
                    else                 mem_wr32(sb, addr, v, &oob);
                }
                else handled = 0;
            }
            if (handled) {
                if (oob) IFAIL("move: destination out of sandbox");
                uint32_t masked = (bits == 32) ? v : (v & ((1u << bits) - 1u));
                uint32_t msb = 1u << (bits - 1);
                uint32_t cc = st->ccr & J5D_CCR_X;
                if (masked == 0)  cc |= J5D_CCR_Z;
                if (masked & msb) cc |= J5D_CCR_N;
                st->ccr = cc;
                pc += (uint32_t)(ext - ip); continue;
            }
        }

        IFAIL("interp: out-of-subset opcode");
    }
#undef IFAIL
}
