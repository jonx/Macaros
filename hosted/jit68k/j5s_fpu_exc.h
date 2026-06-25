/* j5s_fpu_exc.h — [J5s] the 68881/68882 FP EXCEPTION MODEL: the FPSR exception (EXC) +
 * accrued (AEXC) bytes, the FPCR exception-enable + mode-control (rounding) bytes, the FP
 * exception vectors, and BSUN. Shared by the engine dispatcher (j5d_engine.c) AND the
 * independent oracle (j5d_interp.c) so the asserted FPSR is BIT-EXACT on both sides.
 * (OURS, AROS-licensed — NO Emu68 source. Bit layout + vectors grounded below.)
 *
 * ============================ GROUNDING (the bit layout + vectors) =====================
 * The 68881/68882 FPSR/FPCR byte layout + the FP exception vector numbers are grounded
 * against the AROS m68k FPSP sources (Motorola's own 68040/060 FP support package, in-tree
 * APL/LGPL — a [PUB]/[AROS] fact, not Emu68 expression):
 *
 *   arch/m68k-all/m680x0/fpsp/fpsp.h  (bit numbers within each FPSR/FPCR byte):
 *     FPSR EXC  byte (bits 15..8): bsun=15 snan=14 operr=13 ovfl=12 unfl=11 dz=10 inex2=9 inex1=8
 *     FPSR AEXC byte (bits  7..0): aiop=7 aovfl=6 aunfl=5 adz=4 ainex=3
 *     FPCR ENABLE byte = SAME bit positions as EXC (bits 15..8): a trap is taken when an
 *                        EXC bit fires AND the matching ENABLE bit is set.
 *     FPCR MODE byte (bits 7..0): PREC bits 7..6 (x=00 single=01 double=10), RND bits 5..4
 *                        (rn=00 rz=01 rm=10 rp=11)  — x_mode/s_mode/d_mode + rn/rz/rm/rp_mode.
 *   arch/m68k-all/m680x0/fpsp/FPSP.sa  (the FP exception VECTOR offsets, byte = vector*4):
 *     BSUN_VEC=$c0 (48)  INEX2_VEC=$c4 (49)  *DZ=$c8 (50)*  UNFL_VEC=$cc (51)
 *     OPERR_VEC=$d0 (52) OVFL_VEC=$d4 (53)   SNAN_VEC=$d8 (54)
 *   arch/m68k-all/include/aros/fenv.h confirms the C fenv<->68k mapping that drives this file:
 *     FE_INEXACT/DIVBYZERO/UNDERFLOW/OVERFLOW/INVALID are the AEXC bit positions; the four
 *     rounding modes use the FPCR MODE-byte RND values. (We re-derive against fpsp.h, the
 *     primary source, and use <fenv.h> on the host for detection — same mapping.)
 *
 * THE fenv MAPPING (host <fenv.h> -> 68k FPSR EXC), confirmed empirically on this Mac
 * (Apple silicon, macOS 26) with a probe:
 *     FE_DIVBYZERO -> DZ          (1.0/0.0)
 *     FE_OVERFLOW  -> OVFL        (+ FE_INEXACT, so INEX2 too)
 *     FE_UNDERFLOW -> UNFL        (+ FE_INEXACT)
 *     FE_INEXACT   -> INEX2
 *     FE_INVALID   -> OPERR  (0/0, inf-inf, sqrt(<0)) OR SNAN (a signalling-NaN operand).
 *       fenv cannot split SNAN from OPERR (both raise FE_INVALID), and a plain FMOVE of an
 *       sNaN does NOT raise FE_INVALID on AArch64 — so SNAN is detected SEPARATELY by an
 *       operand scan (j5s_is_snan), identically on both sides. When FE_INVALID fired: SNAN
 *       if a source operand was a signalling NaN, else OPERR.
 *
 * THE PRECISION MODEL (carried from [J5o]): FP regs are IEEE binary64. FPCR rounding MODE
 * (RN/RZ/RM/RP) is honoured exactly via fesetround on BOTH the JIT's native AArch64 FP run
 * AND the oracle. FPCR rounding PRECISION single (.s) is modelled by rounding the double
 * result through float (j5s_round_prec); extended (.x) and double (.d) are the native double.
 */
#ifndef J5S_FPU_EXC_H
#define J5S_FPU_EXC_H

#include <stdint.h>
#include <fenv.h>
#include <math.h>
#include <string.h>

/* ---- FPSR EXC byte (bits 15..8) — grounded fpsp.h *_bit + 8 ------------------------- */
#define J5S_FPSR_BSUN   0x00008000u   /* bit15 — branch/set on unordered (BSUN)        */
#define J5S_FPSR_SNAN   0x00004000u   /* bit14 — signalling NaN operand                */
#define J5S_FPSR_OPERR  0x00002000u   /* bit13 — operand error (0/0, inf-inf, sqrt<0)  */
#define J5S_FPSR_OVFL   0x00001000u   /* bit12 — overflow                              */
#define J5S_FPSR_UNFL   0x00000800u   /* bit11 — underflow                             */
#define J5S_FPSR_DZ     0x00000400u   /* bit10 — divide by zero                        */
#define J5S_FPSR_INEX2  0x00000200u   /* bit9  — inexact result                        */
#define J5S_FPSR_INEX1  0x00000100u   /* bit8  — inexact on decimal input (.p only)    */
#define J5S_FPSR_EXC_MASK 0x0000FF00u

/* ---- FPSR AEXC byte (bits 7..0) — grounded fpsp.h a*_bit -------------------------- */
#define J5S_FPSR_AIOP   0x00000080u   /* bit7 — accrued invalid operation             */
#define J5S_FPSR_AOVFL  0x00000040u   /* bit6 — accrued overflow                       */
#define J5S_FPSR_AUNFL  0x00000020u   /* bit5 — accrued underflow                      */
#define J5S_FPSR_ADZ    0x00000010u   /* bit4 — accrued divide by zero                 */
#define J5S_FPSR_AINEX  0x00000008u   /* bit3 — accrued inexact                        */
#define J5S_FPSR_AEXC_MASK 0x000000FFu

/* ---- FPCR ENABLE byte (bits 15..8) — same positions as the EXC byte --------------- */
#define J5S_FPCR_ENABLE_MASK 0x0000FF00u   /* a trap fires iff (fpsr_exc & fpcr) on these bits */

/* ---- FPCR MODE byte (bits 7..0) — PREC bits 7..6, RND bits 5..4 (fpsp.h x/s/d + rn..rp) */
#define J5S_FPCR_PREC_MASK 0x000000C0u
#define J5S_FPCR_PREC_X    0x00000000u   /* extended (modelled as double)                */
#define J5S_FPCR_PREC_S    0x00000040u   /* single                                       */
#define J5S_FPCR_PREC_D    0x00000080u   /* double                                       */
#define J5S_FPCR_RND_MASK  0x00000030u
#define J5S_FPCR_RND_RN    0x00000000u   /* round to nearest                             */
#define J5S_FPCR_RND_RZ    0x00000010u   /* round toward zero                            */
#define J5S_FPCR_RND_RM    0x00000020u   /* round toward minus infinity                  */
#define J5S_FPCR_RND_RP    0x00000030u   /* round toward plus infinity                   */

/* ---- The FP exception VECTOR numbers (byte offset = number*4) — grounded FPSP.sa ---- */
#define J5S_VEC_FP_BSUN   48u   /* $c0 */
#define J5S_VEC_FP_INEX   49u   /* $c4 (INEX2/INEX1)                                     */
#define J5S_VEC_FP_DZ     50u   /* $c8 */
#define J5S_VEC_FP_UNFL   51u   /* $cc */
#define J5S_VEC_FP_OPERR  52u   /* $d0 */
#define J5S_VEC_FP_OVFL   53u   /* $d4 */
#define J5S_VEC_FP_SNAN   54u   /* $d8 */

/* ---- FPCR RND byte -> the host fesetround direction (double-precision rounding MODE). */
static inline int j5s_host_round(uint32_t fpcr)
{
    switch (fpcr & J5S_FPCR_RND_MASK) {
        case J5S_FPCR_RND_RZ: return FE_TOWARDZERO;
        case J5S_FPCR_RND_RM: return FE_DOWNWARD;
        case J5S_FPCR_RND_RP: return FE_UPWARD;
        default:              return FE_TONEAREST;   /* RN */
    }
}

/* ---- single-precision rounding: round a double result through float per the FPCR PREC byte.
 * (Extended/double precision keep the native double.) The current host rounding direction
 * (set by j5s_host_round) governs the double->float step too. */
static inline double j5s_round_prec(double v, uint32_t fpcr)
{
    if ((fpcr & J5S_FPCR_PREC_MASK) == J5S_FPCR_PREC_S) {
        float f = (float)v;            /* double -> single rounds per the current direction */
        return (double)f;
    }
    return v;
}

/* ---- is `v`'s bit pattern a SIGNALLING NaN (binary64)? exp all-ones, fraction != 0,
 * and the quiet bit (fraction MSB = overall bit 51) == 0. Used to split SNAN from OPERR. */
static inline int j5s_is_snan(double v)
{
    uint64_t b; memcpy(&b, &v, 8);
    uint64_t exp  = (b >> 52) & 0x7ffull;
    uint64_t frac =  b        & 0xfffffffffffffull;
    return (exp == 0x7ffull) && (frac != 0) && ((b & (1ull << 51)) == 0);
}

/* ---- map the FPSR EXC bits just produced into the AEXC accrued bits (68881 standard,
 * grounded against the M68000 FP manual's EXC->AEXC table, restated here):
 *   AIOP  (accrued invalid)   <- BSUN | SNAN | OPERR
 *   AOVFL (accrued overflow)  <- OVFL
 *   AUNFL (accrued underflow) <- UNFL & INEX2     (underflow accrues only WITH inexact)
 *   ADZ   (accrued div-by-0)  <- DZ
 *   AINEX (accrued inexact)   <- INEX1 | INEX2 | OVFL  (overflow always accrues inexact)
 * The AEXC bits are STICKY: this returns the bits THIS op contributes; the caller ORs. */
static inline uint32_t j5s_exc_to_aexc(uint32_t exc)
{
    uint32_t a = 0;
    if (exc & (J5S_FPSR_BSUN | J5S_FPSR_SNAN | J5S_FPSR_OPERR)) a |= J5S_FPSR_AIOP;
    if (exc & J5S_FPSR_OVFL)                                    a |= J5S_FPSR_AOVFL;
    if ((exc & J5S_FPSR_UNFL) && (exc & J5S_FPSR_INEX2))        a |= J5S_FPSR_AUNFL;
    if (exc & J5S_FPSR_DZ)                                      a |= J5S_FPSR_ADZ;
    if (exc & (J5S_FPSR_INEX1 | J5S_FPSR_INEX2 | J5S_FPSR_OVFL))a |= J5S_FPSR_AINEX;
    return a;
}

/* ---- map a host fetestexcept() result + a sNaN-operand flag into the 68k FPSR EXC bits.
 * `had_snan_operand` distinguishes SNAN (a signalling-NaN source) from OPERR when FE_INVALID
 * fired. Returns the EXC bits (bits 15..8) for THIS op. */
static inline uint32_t j5s_fenv_to_exc(int fe, int had_snan_operand)
{
    uint32_t exc = 0;
    if (fe & FE_DIVBYZERO) exc |= J5S_FPSR_DZ;
    if (fe & FE_OVERFLOW)  exc |= J5S_FPSR_OVFL;
    if (fe & FE_UNDERFLOW) exc |= J5S_FPSR_UNFL;
    if (fe & FE_INEXACT)   exc |= J5S_FPSR_INEX2;
    if (fe & FE_INVALID)   exc |= (had_snan_operand ? J5S_FPSR_SNAN : J5S_FPSR_OPERR);
    return exc;
}

/* ---- the EXC bit -> its FP exception vector number (for trap dispatch). The 68881 priority
 * order when several enabled exceptions co-occur (M68000 FP manual): BSUN, SNAN, OPERR, OVFL,
 * UNFL, DZ, INEX2, INEX1. We return the HIGHEST-priority enabled+pending bit's vector. */
static inline int j5s_exc_vector(uint32_t pending_enabled, unsigned *vec_out)
{
    if (pending_enabled & J5S_FPSR_BSUN)  { *vec_out = J5S_VEC_FP_BSUN;  return 1; }
    if (pending_enabled & J5S_FPSR_SNAN)  { *vec_out = J5S_VEC_FP_SNAN;  return 1; }
    if (pending_enabled & J5S_FPSR_OPERR) { *vec_out = J5S_VEC_FP_OPERR; return 1; }
    if (pending_enabled & J5S_FPSR_OVFL)  { *vec_out = J5S_VEC_FP_OVFL;  return 1; }
    if (pending_enabled & J5S_FPSR_UNFL)  { *vec_out = J5S_VEC_FP_UNFL;  return 1; }
    if (pending_enabled & J5S_FPSR_DZ)    { *vec_out = J5S_VEC_FP_DZ;    return 1; }
    if (pending_enabled & (J5S_FPSR_INEX2 | J5S_FPSR_INEX1)) { *vec_out = J5S_VEC_FP_INEX; return 1; }
    return 0;
}

#endif /* J5S_FPU_EXC_H */
