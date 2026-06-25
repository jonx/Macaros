/* j3_marshal.c — [J3] the marshaller: 68k regs -> native AAPCS64 call (GLUE, OURS).
 *
 * This is the REVERSE of the H3 host-call shim (hosted/abishim.S). H3 marshals an
 * AROS_LH (register) call OUT to the variadic macOS C ABI; this marshals a 68k
 * library call IN to a native AArch64 AROS_LH stub. It is data-driven by the
 * per-LVO register map (the AROS_LHA/AROS_UFHA `reg` list, grounded in j3_jit68k.h
 * note B), NOT by a variadic stack walk.
 *
 * REALIZATION CHOICE: we EMIT the fixed marshal sequence per call site, via the
 * ADOPTED Emu68 emitter (the verbatim MPL-quarantined emu68/A64.h), into a [J1]
 * MAP_JIT jit_region — the spec's PREFERRED path, because it is the REAL
 * translated-code path and directly extends [J2] (same emitter, same W^X region,
 * same jit_region API). The alternative (a hand-written reverse-H3 AArch64 .S
 * trampoline) would prove the marshalling but NOT that it lives in the JIT pipeline;
 * emitting keeps the bridge inside the translator the way production will.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS
 * (AROS-licensed). It #includes the quarantined Emu68 emitter and CALLS its inline
 * encoders; per MPL-2.0 / Mozilla FAQ Q11-Q13 that does NOT relicense this file.
 * No Emu68 source is copied here. Only encoder functions are invoked; none of
 * Emu68's runtime (RA_*, MMU, cache, MainLoop) is linked.
 *
 * The emitted thunk's contract (AAPCS64): called as `void thunk(M68KState *st)`,
 * with st in x0. It reads the per-arg source 68k registers from *st, places them
 * in x0..x7, blr's the native stub, and (if the LVO returns a value) stores the
 * native 32-bit return into st->d[0]. The 68k registers are 32-bit; we load them
 * with a 32-bit ldr (zero-extends into the 64-bit arg register), which matches the
 * ULONG/APTR-in-the-32-bit-sandbox semantics. */
#include "j3_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"      /* adopted MPL emitter — encoders only, called not copied */

#include <string.h>
#include <stdio.h>

/* [J2]-evidence item 2, carried over: the single external symbol A64.h's
 * ASSERT_REG macro references. Never called for valid register numbers (we only
 * pass registers < 32). A no-op satisfies the link. */
__attribute__((weak)) void kprintf(const char *format, ...) { (void)format; }

/* AArch64 register numbers we use for the thunk frame/scratch. The arg registers
 * x0..x7 are the AAPCS64 call args; x19/x20 are callee-saved (survive the blr). */
#define X_SP   31
#define X_FP   29   /* x29 */
#define X_LR   30   /* x30 */
#define X_STATE 19  /* x19: holds the M68KState* across the call (callee-saved) */
#define X_STUB  20  /* x20: holds the native stub address    (callee-saved) */

/* A tiny pool of live regions so several thunks can coexist within the spike. */
#define J3_MAX_THUNKS 8
static jit_region g_pool[J3_MAX_THUNKS];
static int        g_pool_n = 0;

/* Emit the marshal thunk for `desc` into `out`; return the number of 32-bit words.
 * All instructions come from the adopted Emu68 encoders (emu68/A64.h). */
static unsigned emit_thunk(const j3_lvo_desc *desc, uint32_t *out)
{
    unsigned n = 0;
    uintptr_t stub = (uintptr_t)desc->stub;

    /* --- Prologue: standard AAPCS64 frame; preserve x19/x20 (callee-saved). ---
     * stp64 x29,x30,[sp,#-16]!   ;  stp64 x19,x20,[sp,#-16]! */
    out[n++] = stp64_preindex(X_SP, X_FP, X_LR, -16);
    out[n++] = stp64_preindex(X_SP, X_STATE, X_STUB, -16);

    /* x19 := x0 (the M68KState*). Do this BEFORE loading args, since the arg loads
     * overwrite w0. mov64_reg keeps the full 64-bit host pointer. */
    out[n++] = mov64_reg(X_STATE, 0 /*x0*/);

    /* --- Marshal: for each native arg i, load its source 68k register (32-bit)
     * from the M68KState into x_i (the 32-bit ldr zero-extends into the 64-bit arg
     * register). desc->src[i] names the 68k register per the AROS_LHA/AROS_UFHA map. */
    for (int i = 0; i < desc->nargs; i++) {
        j3_m68k_reg r = desc->src[i];
        uint16_t off = J3_REG_IS_ADDR(r) ? M68K_OFF_A(J3_REG_INDEX(r))
                                         : M68K_OFF_D(J3_REG_INDEX(r));
        /* ldr w_i, [x19, #off]  — Emu68's 32-bit ldr_offset (LDR Wt,[Xn,#imm]). */
        out[n++] = ldr_offset(X_STATE, (uint8_t)i /*x0..x7 -> w0..w7*/, off);
    }

    /* --- Build the 64-bit native stub address into x20, then call it. ---
     * mov64 x20,#b0 ; movk64 x20,#b1,lsl16 ; movk64 x20,#b2,lsl32 ; movk64 x20,#b3,lsl48 */
    out[n++] = mov64_immed_u16 (X_STUB, (uint16_t)(stub        & 0xffff), 0);
    out[n++] = movk64_immed_u16(X_STUB, (uint16_t)((stub >> 16) & 0xffff), 1);
    out[n++] = movk64_immed_u16(X_STUB, (uint16_t)((stub >> 32) & 0xffff), 2);
    out[n++] = movk64_immed_u16(X_STUB, (uint16_t)((stub >> 48) & 0xffff), 3);
    out[n++] = blr(X_STUB);                          /* blr x20 — the native call */

    /* --- Return: store the native 32-bit return (w0) into 68k d0 (note C). --- */
    if (desc->returns)
        out[n++] = str_offset(X_STATE, 0 /*w0*/, M68K_OFF_D(0)); /* str w0,[x19,#0] */

    /* --- Epilogue: restore callee-saved regs and the frame, return. --- */
    out[n++] = ldp64_postindex(X_SP, X_STATE, X_STUB, 16);
    out[n++] = ldp64_postindex(X_SP, X_FP, X_LR, 16);
    out[n++] = ret();
    return n;
}

j3_thunk_fn j3_build_marshal_thunk(const j3_lvo_desc *desc, char *errbuf, unsigned errlen)
{
    if (!desc || desc->nargs < 0 || desc->nargs > 8) {
        if (errbuf) snprintf(errbuf, errlen, "bad descriptor (nargs out of [0,8])");
        return NULL;
    }
    if (g_pool_n >= J3_MAX_THUNKS) {
        if (errbuf) snprintf(errbuf, errlen, "thunk pool exhausted");
        return NULL;
    }

    uint32_t staging[64];
    unsigned nwords = emit_thunk(desc, staging);

    jit_region *r = &g_pool[g_pool_n];
    if (jit_region_alloc(r, nwords * sizeof(uint32_t)) != 0) {
        if (errbuf) snprintf(errbuf, errlen, "jit_region_alloc(MAP_JIT) failed");
        return NULL;
    }
    g_pool_n++;

    /* Push the staged AArch64 words into the MAP_JIT region through the [J1] W^X
     * dance: open the per-thread write window, copy, close, i-cache flush. */
    jit_write_begin(r);
    memcpy(r->base, staging, nwords * sizeof(uint32_t));
    jit_write_end(r);
    jit_finalize(r, r->base, nwords * sizeof(uint32_t));

    return (j3_thunk_fn)(void *)r->base;
}

void j3_free_all_thunks(void)
{
    for (int i = 0; i < g_pool_n; i++)
        jit_region_free(&g_pool[i]);
    g_pool_n = 0;
}
