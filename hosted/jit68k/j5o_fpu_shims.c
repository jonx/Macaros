/* j5o_fpu_shims.c — [J5o] link stubs + small helpers for the NEWLY vendored Emu68 FPU
 * decoder (emu68/M68k_LINEF.c), so it compiles + links hosted (GLUE, OURS, AROS-licensed).
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS. It
 * DEFINES symbols the verbatim FPU decoder references; NO Emu68 function body is copied. Each
 * stub is the documented no-op / never-driven contract, or (M68K_IsBranch) is authored from
 * the PUBLISHED Motorola 68000 ISA branch-opcode encodings — facts, not Emu68 expression.
 * (MPL FAQ Q11-Q13: linking does not relicense this file.)
 *
 * WHAT THIS INCREMENT DRIVES (the [J5o] FP core): FMOVE (reg<->reg, mem<->reg, with format
 * conversions .s/.d and .l/.w/.b<->FP), FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG, FCMP, FTST. All
 * of those use only: the A64.h fp encoders (faddd/.../fcmpd/fcmpzd/fcvtds/fcvtsd/scvtf_32toD),
 * EMIT_GetFPUFlags (A64.h), the FPU register allocator (j5c_ra.c — d8..d15 file + memory-backed
 * FPCR/FPSR), the sandbox FP-memory helpers (j5d_ea_helpers.c j5d_fpu_*), and the SAME
 * EMIT_AdvancePC/FlushPC/GetOffsetPC/cache_read_N/EMIT_LoadFromEffectiveAddress the integer
 * decoders already use (j5c_shims.c + the vendored M68k_EA.c). NONE of the symbols defined in
 * THIS file are reached by the driven FP core — they exist purely so the monolithic verbatim
 * M68k_LINEF.c LINKS.
 *
 * WHY EACH STUB IS NEVER ON THE [J5o] DRIVEN PATH:
 *   Load96bit / Store96bit / reg_Load96 / reg_Save96
 *       — the 80-bit EXTENDED (.x) memory format. The [J5o] precision model is IEEE double;
 *         80-bit exactness is not bit-reproducible on AArch64, so the .x memory forms are
 *         DEFERRED (documented). The decoder's .x path does `blr reg_Load96` to these runtime
 *         routines; the test program uses no .x operand, so they are never called.
 *   PackedToDouble / DoubleToPacked
 *       — the packed-decimal (.p) memory format (FMOVE.p). DEFERRED this increment; not used.
 *   sin/cos/tan/asin/acos/atan/atanh/sinh/cosh/tanh/exp/exp2/exp10/expm1/log/log2/log10/log1p/
 *   remquo/sincos/my_log10/my_pow10/PolySine[Single]/PolyCosine[Single]
 *       — the TRANSCENDENTAL FPU ops (FSIN/FCOS/FETOX/FLOGN/...). OUT OF SCOPE this increment
 *         (next FP increment). Routed to the system libm where the C signature matches (so a
 *         FUTURE increment that drives them is already partly wired); the Emu68-internal ones
 *         (my_log10/my_pow10/sincos/remquo/Load96bit/Poly*) are abort-on-call stubs — never
 *         reached by the driven set, and loudly fail if a future test drives them un-ported.
 *   EMIT_SaveRegFrame / EMIT_RestoreRegFrame
 *       — Emu68's register-frame save/restore around the runtime blr math-helper calls
 *         (.x/.p/transcendental). The driven arithmetic/move/cmp ops do NOT call them; a no-op
 *         stub is safe for the [J5o] set. (NOT copied from RegisterAllocator64.c — authored as
 *         a documented no-op, since the frames it would save are only for the un-driven blr.)
 *   LRU / jit_tlsf / tlsf_free / tlsf_get_free_size / cache_invalidate_all /
 *   clear_entire_dcache / invalidate_entire_dcache / invalidate_instruction_cache /
 *   trampoline_icache_invalidate
 *       — bare-metal allocator + cache-maintenance the MOVE16/CINV/CPUSH branch of EMIT_lineF
 *         references (re-hosted as the host process / __clear_cache at integration). The [J5o]
 *         engine drives only the FPU coprocessor sub-decoder (EMIT_FPU), never MOVE16/CINV, so
 *         these are never reached.
 *   val_FPIAR
 *       — the FP instruction-address register the decoder snapshots for FTRAPcc; modeled as a
 *         plain global (not in the seam state — FTRAPcc is a later increment).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* [J5p] We deliberately DO NOT #include <math.h>: emu68/math/libm.h (included below for the
 * decoder's struct types) provides `static inline sqrt/fabs` + a 2-arg `remquo` decl that clash
 * with <math.h>. The HOST libm functions we call for the three wrappers are declared minimally
 * here (they resolve to the system libm at link time — the SAME reference the oracle uses). */
extern double pow(double, double);
extern double sin(double), cos(double);
extern double remainder(double, double);
extern double rint(double);

#include "emu68/cache.h"        /* enum CacheType + cache_read_16 (the [J5c] HOOK 2 fetch)   */
#include "math/libm.h"          /* struct double2 / struct rq + the transcendental decls. We
                                 * include the DARWINIZED build copy (asm-fixed) via the build's
                                 * -Ibuild/emu68-darwin path order, so the two host-incompatible
                                 * inline-asm helpers are the fixed variant. (Only the struct
                                 * types + decls are used here.) */

/* ---- M68K_IsBranch — authored from the PUBLISHED 68000 ISA branch-opcode encodings -------
 * FPSR_Update_Needed() (in the driven EMIT_FPU) scans the forward m68k stream for the FP
 * consumer that needs the FPSR; it calls this to stop scanning at a control-flow boundary.
 * For the [J5o] test the FP ops are consecutive (no branch between them) so this returns 0,
 * but we implement it faithfully so the JIT's FPSR_Update_Needed and the oracle's copy agree
 * for ANY stream. The opcode set below is the standard 68k branch/return/trap family (M68000
 * PRM): RTM/RTD/CALLM/Scc/DBcc/TRAPcc/TRAPV/ILLEGAL/TRAP#n/LINK/UNLK/RTS/RTE/RTR/JMP/JSR/Bcc.
 * (These encodings are an external ISA fact, not Emu68 expression — clean to author here.) */
int M68K_IsBranch(uint16_t *insn_stream)
{
    uint16_t opcode = cache_read_16(ICACHE, (uint32_t)(uintptr_t)&insn_stream[0]);
    if ( opcode == 0x007c || opcode == 0x027c || opcode == 0x0a7c   /* ORI/ANDI/EORI to SR  */
      || (opcode & 0xffc0) == 0x40c0 || (opcode & 0xffc0) == 0x46c0 /* MOVE from/to SR/CCR  */
      || (opcode & 0xfff8) == 0x4848                                /* BKPT                 */
      ||  opcode == 0x4afc                                          /* ILLEGAL              */
      || (opcode & 0xfff0) == 0x4e40 || (opcode & 0xfff0) == 0x4e60 /* TRAP #n / MOVE USP   */
      ||  opcode == 0x4e70 || opcode == 0x4e72 || opcode == 0x4e73  /* RESET/STOP/RTE       */
      ||  opcode == 0x4e74 || opcode == 0x4e75 || opcode == 0x4e76  /* RTD/RTS/TRAPV        */
      ||  opcode == 0x4e77                                          /* RTR                  */
      || (opcode & 0xfffe) == 0x4e7a                                /* MOVEC                */
      || (opcode & 0xff80) == 0x4e80                                /* JSR/JMP              */
      || (opcode & 0xf0f8) == 0x50c8                                /* DBcc                 */
      || (opcode & 0xf000) == 0x6000 )                              /* Bcc/BRA/BSR          */
        return 1;
    return 0;
}

/* ---- val_FPIAR: a plain global the decoder snapshots for FTRAPcc (later increment). ----- */
uint32_t val_FPIAR = 0;

/* ---- reg_Load96 / reg_Save96: the cached ARM reg holding &Load96bit / &Store96bit. The
 * decoder initialises them to 0xff and lazily materialises the helper address; the .x path
 * that consumes them is not driven, so leaving them 0xff is fine (never dereferenced here). */
uint8_t reg_Load96 = 0xff;
uint8_t reg_Save96 = 0xff;

/* ---- the 80-bit-extended (.x) runtime helpers — DEFERRED (precision model is double). ----
 * Abort if ever actually called (a future increment that drives .x must port these against a
 * real 80-bit<->double conversion). Never reached by the [J5o] driven set. (PackedToDouble /
 * DoubleToPacked are NOT here — they are DEFINED inside the verbatim M68k_LINEF.c itself; we
 * only provide the externals that file references: the exp10/my_x/sincos/remquo set below.) */
uint64_t Load96bit(uintptr_t ignore, uintptr_t base)  { (void)ignore; (void)base; abort(); }
uint64_t Store96bit(uintptr_t value,  uintptr_t base) { (void)value;  (void)base; abort(); }

/* ---- [J5p] EMIT_SaveRegFrame / EMIT_RestoreRegFrame: REAL save/restore around the
 * transcendental blr. Every driven transcendental + FSINCOS + FREM site wraps its `blr`
 * (to the host libm fn) in EMIT_SaveRegFrame(...) / EMIT_RestoreRegFrame(...). The blr clobbers
 * the AAPCS64 CALLER-SAVED registers (x0..x18 + the caller-saved SIMD lanes); the 68k FP file
 * (d8..d15) and the 68k integer file (x19..x28) are AAPCS64 CALLEE-SAVED so libm preserves them,
 * but OUR block keeps load-bearing values in CALLER-SAVED registers that MUST survive the call:
 *   - x1  = the struct j5d_m68k_state* (J5C_STATE_X, kept for the whole block — every later FP
 *           store + the FPCR/FPSR flush reads through it),
 *   - x12 = the sandbox base-adjust (J5D_BASEADJ_X = host_mem - origin — every later FP-mem
 *           store adds it),
 *   - the live RA scratch temps (RA_GetTempAllocMask() => x4..x11) and the REG_PROTECT set the
 *     decoder passes (A0..A4 = x13..x17, REG_PC = x18 — these hold live 68k A-registers / PC).
 * We push exactly those onto the host stack before the call and pop them after, so the JIT'd
 * block resumes with every live register intact. (Authored from the AAPCS64 caller/callee-save
 * rule + OUR register map — NOT copied from Emu68's RegisterAllocator64.c, which keeps the frame
 * in a different layout and assumes the bare-metal context.) The decoder no-op stub this
 * replaces was safe ONLY for the [J5o] core, which drives no blr site. */
#include "emu68/A64.h"   /* the adopted AArch64 encoders (stp/ldp); calling them copies no source */

uint32_t *EMIT_SaveRegFrame(uint32_t *ptr, uint32_t mask)
{
    /* Build the ordered list of x-registers to preserve: always x1 (state) + x12 (base_adjust),
     * then each register in `mask` (the RA temps + REG_PROTECT set), skipping the non-GP sentinel
     * bits (>=29: bit30 is REG_PROTECT's marker, not a register). x1/x12 are added explicitly so
     * a mask that omits them still protects them. */
    uint8_t regs[32]; int n = 0;
    regs[n++] = 1;    /* x1  = state ptr      */
    regs[n++] = 12;   /* x12 = base-adjust    */
    for (int r = 2; r <= 28; r++) {
        if (r == 12) continue;                 /* already added */
        if (mask & (1u << r)) regs[n++] = (uint8_t)r;
    }
    if (n & 1) regs[n++] = 31;                 /* pad to an even count with xzr (16-byte stp)   */
    /* push pairs, pre-decrementing sp by 16 each (LIFO; matches the postindex restore order).   */
    for (int i = 0; i < n; i += 2)
        *ptr++ = stp64_preindex(31 /*sp*/, regs[i], regs[i + 1], -16);
    return ptr;
}

uint32_t *EMIT_RestoreRegFrame(uint32_t *ptr, uint32_t mask)
{
    /* Rebuild the SAME list, then pop in REVERSE (LIFO) so the stack unwinds exactly. */
    uint8_t regs[32]; int n = 0;
    regs[n++] = 1;
    regs[n++] = 12;
    for (int r = 2; r <= 28; r++) {
        if (r == 12) continue;
        if (mask & (1u << r)) regs[n++] = (uint8_t)r;
    }
    if (n & 1) regs[n++] = 31;
    for (int i = n - 2; i >= 0; i -= 2)
        *ptr++ = ldp64_postindex(31 /*sp*/, regs[i], regs[i + 1], 16);
    return ptr;
}

/* ---- [J5p] TRANSCENDENTAL + FP-UTILITY math — ROUTED TO THE HOST libm. ------------------
 * The 68881/68882 transcendentals are implementation-defined in their last ULPs, so the
 * faithful hosted realization is: route Emu68's transcendental helper sites to the HOST libm
 * (<math.h>), and have the independent oracle (j5d_interp.c) use the SAME host libm — so the
 * assert is bit-exact and verifies the real thing under test (that the JIT correctly DECODES
 * each FP transcendental, passes the right register as the argument, and stores the result).
 * We are testing the TRANSLATION, not re-deriving sin(); host libm is the reference.
 *
 * HOW THE ROUTING WORKS (no quarantine edit): the verbatim decoder bakes the helper's ADDRESS
 * into the emitted block at translate time — `u.u64 = (uintptr_t)sin; ... blr`. So routing is
 * simply: do NOT shadow the standard libm names here; let them resolve to the HOST libm at link
 * time. The decoder then bakes in &sin == the host sin. The only symbols we DEFINE are the THREE
 * the host does not provide in the decoder's exact signature: exp10 (10^x — not standard C),
 * sincos (-> struct double2), and remquo (-> struct rq {double rem; uint64_t quo}). Those are
 * THIN WRAPPERS over the standard host functions (pow/sin+cos/remquo) — no Emu68 source copied;
 * the wrapping is ours.
 *
 * libm <-> 68881 opmode map driven this increment (the FPU command word opmode selects):
 *   FSIN->sin  FCOS->cos  FTAN->tan  FASIN->asin  FACOS->acos  FATAN->atan
 *   FSINH->sinh  FCOSH->cosh  FTANH->tanh  FATANH->atanh
 *   FETOX(e^x)->exp  FETOXM1->expm1  FTWOTOX(2^x)->exp2  FTENTOX(10^x)->exp10(=pow(10,x))
 *   FLOGN(ln)->log  FLOGNP1->log1p  FLOG10->log10  FLOG2->log2
 *   FSINCOS->sincos (sin AND cos into two FP regs)   FREM->remquo (IEEE remainder + quo byte)
 * The FP-UTILITY ops the decoder emits INLINE (no libm blr) are still ours-via-decoder and the
 * oracle mirrors them: FINT->frint64x (round per FPCR), FINTRZ->frint64z (trunc), FGETEXP/
 * FGETMAN (bit extraction), FMOD (fdiv/frint/fmul/fsub), FSCALE (build 2^n * x).
 *
 * STILL DEFERRED (abort-on-call): my_log10/my_pow10 are Emu68's integer log10/pow10 used ONLY
 * by the packed-decimal (.p) DoubleToPacked path — not driven (precision model is double; .p is
 * a deferred MEMORY format). They trip loudly if a future .p increment reaches them. ---------- */

/* The three non-standard signatures the host libm does not provide as-is. */
double exp10(double x) { return pow(10.0, x); }          /* FTENTOX: 10^x */

struct double2 sincos(double x)                          /* FSINCOS: sin AND cos */
{
    struct double2 r;
    r.d[0] = sin(x);
    r.d[1] = cos(x);
    return r;
}

struct rq remquo(double x, double y)                     /* FREM: IEEE remainder + quotient */
{
    /* The decoder calls a 2-arg struct-returning remquo; the host's standard remquo is the
     * 3-arg form. We can't recurse into the host's (this IS the symbol `remquo`), so we use
     * remainder() for the value (IEEE round-to-nearest, identical to FREM's r = x - y*N with
     * N = round(x/y)) and derive the quotient ourselves. The quotient's low 7 bits + sign go to
     * the FPSR quotient byte (NOT part of the N/Z/I/NAN cc byte the [J5p] test asserts). */
    struct rq r;
    r.rem = remainder(x, y);
    double q = (y != 0.0) ? rint(x / y) : 0.0;           /* round-to-nearest quotient */
    long long qi = (long long)q;
    r.quo = (uint64_t)(qi < 0 ? -qi : qi) & 0x7f;
    if (qi < 0) r.quo |= 0x80;
    return r;
}

/* Emu68's integer log10 / pow10 — used ONLY by the deferred packed-decimal (.p) path. */
int    my_log10(double v) { (void)v; abort(); }
double my_pow10(int e)    { (void)e; abort(); }

/* ---- the GNU-as Poly thunks were neutralised in the build-dir copy (darwinize --fpu-sandbox);
 * provide their global SYMBOLS as no-op stubs so the (un-driven) transcendental blr sites link. */
void PolySine(void)        { abort(); }
void PolySineSingle(void)  { abort(); }
void PolyCosine(void)      { abort(); }
void PolyCosineSingle(void){ abort(); }

/* ---- bare-metal allocator + cache maintenance (MOVE16/CINV branch — not driven). The MOVE16/
 * CINV path of EMIT_lineF is never reached by the [J5o] engine (it drives only the FPU sub-
 * decoder), so these are never CALLED; they exist only so the verbatim file LINKS. NOTE:
 * `invalidate_instruction_cache` is DEFINED inside M68k_LINEF.c itself (not here); we provide
 * only the externals THAT file references: LRU, jit_tlsf, the tlsf helpers, cache_invalidate_all,
 * clear_/invalidate_entire_dcache, and the GNU-as `trampoline_icache_invalidate` thunk (whose body
 * was neutralised in the build-dir copy by darwinize --fpu-sandbox; this provides the symbol). - */
void *jit_tlsf = NULL;
struct List { void *a, *b, *c; };   /* LINEF's `extern struct List LRU` — opaque, never read.  */
struct List LRU = { 0, 0, 0 };
void  tlsf_free(void *handle, void *ptr)              { (void)handle; (void)ptr; }
uintptr_t tlsf_get_free_size(void *memory)            { (void)memory; return 0; }
void cache_invalidate_all(enum CacheType cache)       { (void)cache; }
void clear_entire_dcache(void)                        { }
void invalidate_entire_dcache(void)                   { }
void trampoline_icache_invalidate(void)               { }
