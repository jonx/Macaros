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

/* ---- EMIT_SaveRegFrame / EMIT_RestoreRegFrame: no-op (driven ops never call them; only the
 * un-driven .x/.p/transcendental blr paths would). Authored as a documented no-op — NOT copied
 * from Emu68's RegisterAllocator64.c. */
uint32_t *EMIT_SaveRegFrame   (uint32_t *ptr, uint32_t mask) { (void)mask; return ptr; }
uint32_t *EMIT_RestoreRegFrame(uint32_t *ptr, uint32_t mask) { (void)mask; return ptr; }

/* ---- transcendental math — the FPU's FSIN/FCOS/FETOX/FLOGN/... ops. OUT OF SCOPE this
 * increment (the next FP increment drives them). The verbatim M68k_LINEF.c references these by
 * name (its own libm — declared in emu68/math/libm.h — NOT the system libm; the names overlap
 * but the decoder calls THESE). None is on the [J5o] driven path, so they are abort stubs: a
 * future increment that drives a transcendental will trip the abort and must port a real
 * implementation. (Signatures EXACTLY match emu68/math/libm.h so the decls agree.) ---------- */
int    my_log10(double v) { (void)v; abort(); }   /* Emu68's integer-log10 (used by .p)  */
double my_pow10(int e)    { (void)e; abort(); }
double scalbn(double x, int n)   { (void)x;(void)n; abort(); }
double floor(double x)           { (void)x; abort(); }
double tan(double x)             { (void)x; abort(); }
double tanh(double x)            { (void)x; abort(); }
double exp(double x)             { (void)x; abort(); }
double exp10(double x)           { (void)x; abort(); }
double exp2(double x)            { (void)x; abort(); }
double expm1(double x)           { (void)x; abort(); }
double cosh(double x)            { (void)x; abort(); }
double log(double x)             { (void)x; abort(); }
double atan(double x)            { (void)x; abort(); }
double sinh(double x)            { (void)x; abort(); }
double asin(double x)            { (void)x; abort(); }
double acos(double x)            { (void)x; abort(); }
double atanh(double x)           { (void)x; abort(); }
double log1p(double x)           { (void)x; abort(); }
double log10(double x)           { (void)x; abort(); }
double log2(double x)            { (void)x; abort(); }
double modf(double x, double *iptr) { (void)x;(void)iptr; abort(); }
double sin(double x)             { (void)x; abort(); }
double cos(double x)             { (void)x; abort(); }
struct double2 sincos(double x)           { (void)x;        abort(); }
struct rq      remquo(double x, double y) { (void)x;(void)y; abort(); }

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
