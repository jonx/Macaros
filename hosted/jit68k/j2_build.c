/* j2_build.c — [J2] JIT builder: GLUE (OURS, AROS-licensed).
 *
 * This is the small adapter the spec's [J2] calls for: it lets the *adopted
 * Emu68 emitter* (the verbatim, MPL-quarantined hosted/jit68k/emu68/A64.h) write
 * AArch64 instruction words into a plain uint32_t staging buffer, then pushes that
 * buffer into the [J1] MAP_JIT executable-memory region via the jit_region API
 * (jit_write_begin -> copy -> jit_write_end -> jit_finalize). The result is a
 * callable AArch64 function implementing one fixed 68k basic block.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md):
 *   - THIS FILE is OURS (AROS-licensed). It #includes the quarantined Emu68
 *     emitter and CALLS its inline encoders. Per MPL-2.0 / Mozilla FAQ Q11-Q13,
 *     including/compiling-against an MPL header does NOT relicense this file; only
 *     COPYING Emu68 source into it would. No Emu68 source is copied here.
 *   - The Emu68 emitter stays verbatim in emu68/. Only its *encoder functions*
 *     are invoked; none of its runtime (RA_*, MMU, cache, MainLoop) is linked.
 *
 * ============================ [J0]-SEPARABILITY EVIDENCE ====================
 * What the adopted Emu68 emitter needed, to run hosted in our jit_region — the
 * concrete validation of the [J0] bet "the emitter separates cleanly from the
 * bare-metal runtime":
 *
 *   1. Headers: emu68/A64.h includes ONLY <stdint.h> and <RegisterAllocator.h>.
 *      A64.h's pure-encoder section (lines 1..650) needs neither MMU, cache,
 *      MMIO, RasPi, nor PiStorm — exactly as [J0] predicted. We vendored
 *      RegisterAllocator.h (declarations only) solely so A64.h's verbatim
 *      `#include <RegisterAllocator.h>` (line 652) resolves; none of those RA_*
 *      functions are defined or called by this spike.
 *
 *   2. One undefined symbol to satisfy: `void kprintf(const char*, ...)` —
 *      declared in A64.h and referenced ONLY by the ASSERT_REG debug macro
 *      (A64.h:134). We provide a no-op kprintf below so the translation unit
 *      links. ASSERT_OFFSET is already compiled out upstream (#if 0). No other
 *      external symbol is referenced by the encoders we use.
 *
 *   3. Register-map constants reused verbatim from A64.h: REG_D0..REG_D7 (W19..W26)
 *      and the encoders mov_immed_u16, movk_immed_u16, add_reg, ldr_offset,
 *      str_offset, ret(). These are Emu68's fixed m68k->AArch64 mapping; using
 *      them keeps our emitted block consistent with Emu68 conventions.
 *
 *   4. Endianness: A64.h's I32() normalises each instruction word for the host's
 *      byte order; on little-endian arm64 macOS it is effectively identity, so the
 *      words land as correct native AArch64. Verified out-of-band against llvm-mc.
 *
 *   CONCLUSION: the emitter separated CLEANLY. Total adaptation cost = 1 no-op
 *   stub (kprintf) + 1 declarations-only header. The DECODER was NOT lifted: this
 *   spike HAND-DECODES the two opcodes (see below). Adopting Emu68's M68K_LINE*.c
 *   decoders + RegisterAllocator64.c is deferred to [J3] (they are coupled to
 *   Emu68's JIT-cache bookkeeping and RA state, per the spec's Architecture note).
 * ===========================================================================
 */
#include "j2_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"      /* adopted MPL emitter — encoders only, called not copied */

#include <string.h>
#include <stdio.h>

/* [J0]-evidence item 2: the single external symbol A64.h's ASSERT_REG macro
 * references. Never actually called for valid register numbers; a no-op satisfies
 * the link. (We pass only REG_D0..REG_D7 / x0, all < 32, so ASSERT never fires.) */
__attribute__((weak)) void kprintf(const char *format, ...) { (void)format; }

/* The one region this spike uses. Kept module-static so jit68k_free_block can
 * release it; the [J1] R-JIT-THREAD rule (emit+exec on one thread) is satisfied
 * because the test calls build then run on the same (main) thread. */
static jit_region g_region;
static int        g_region_live = 0;

/* Hand-decode the FIXED 68k basic block and emit the equivalent AArch64 with the
 * adopted Emu68 encoders into `out`. Returns the number of 32-bit words written.
 *
 * 68k block (big-endian opcode bytes, for the record / the interpreter's stream):
 *   moveq #10,d0   -> 0x70 0x0A        (0111 000 0 dddddddd : Dn=0, data=0x0A)
 *   moveq #7,d1    -> 0x72 0x07        (0111 001 0 dddddddd : Dn=1, data=0x07)
 *   add.l  d1,d0   -> 0xD0 0x81        (1101 ddd 1 10 mmmmmm: ADD <ea>,Dn dir=
 *                                       Dn+<ea>->Dn? No: 0xD081 = ADD.L D1,D0)
 *   rts            -> 0x4E 0x75
 *
 * Semantics implemented (register-only, no memory, no flags needed by the block
 * except CCR which add.l sets — we mirror add.l's flag effect for parity with the
 * interpreter; moveq also sets N/Z and clears V/C):
 *
 * AArch64 (state ptr in x0; Emu68 map D0->W19=REG_D0, D1->W20=REG_D1):
 *   ; load the architectural file the caller set up (so a nonzero initial state is
 *   ; honoured and ONLY the regs the block touches change):
 *   ldr  w19,[x0,#0]      ; D0
 *   ldr  w20,[x0,#4]      ; D1
 *   ; translated body:
 *   mov  w19,#10          ; moveq #10,d0
 *   mov  w20,#7           ; moveq #7,d1
 *   add  w19,w19,w20      ; add.l d1,d0   (32-bit add -> result in D0)
 *   ; flush live state back (the RTS epilogue, spec's RA_StoreDirtyM68kRegs shape):
 *   str  w19,[x0,#0]      ; D0
 *   str  w20,[x0,#4]      ; D1
 *   ret                   ; rts -> dispatcher funnel
 *
 * Flags: the block's flag-affecting ops are moveq and add.l. moveq sets N/Z from
 * the value, clears V,C; add.l (ADD) sets N/Z/V/C from the 32-bit sum and X:=C.
 * For operands 10 and 7 (sum 17): N=0,Z=0,V=0,C=0, and X:=C:=0. So the block's
 * final CCR has X,N,Z,V,C all 0. We emit that with a single bic_immed of the low
 * 5 bits (= `and w21,w21,#0xffffffe0`), the same bic_immed encoder Emu68 uses in
 * EMIT_ClearFlags. The independent interpreter computes CCR the long way (full
 * moveq + add.l flag rules) and the test asserts they agree — so this static
 * fold is only "correct" if it matches the from-scratch flag computation.
 */
static unsigned emit_block(uint32_t *out)
{
    unsigned n = 0;

    /* Prologue: load the regs the block reads/writes from the state struct, so the
     * JIT honours a caller-provided initial file (parity with the interpreter,
     * which starts from the same struct). D0,D1 are the only data regs touched. */
    out[n++] = ldr_offset(0 /*x0*/, REG_D0, M68K_OFF_D(0));   /* ldr w19,[x0,#0]  */
    out[n++] = ldr_offset(0 /*x0*/, REG_D1, M68K_OFF_D(1));   /* ldr w20,[x0,#4]  */

    /* Body — the hand-decoded translation of the two data ops. */
    out[n++] = mov_immed_u16(REG_D0, 10, 0);                 /* moveq #10,d0     */
    out[n++] = mov_immed_u16(REG_D1, 7, 0);                  /* moveq #7,d1      */
    out[n++] = add_reg(REG_D0, REG_D0, REG_D1, LSL, 0);      /* add.l d1,d0      */

    /* CCR := clear X,N,Z,V,C (all 0 for this block's result; see header note).
     * bic_immed(dst,src,5,0) = `and dst,src,#0xffffffe0` (clears the low 5 bits,
     * preserves bits 5..31). Uses a tmp reg W21=REG_D2, free here (block doesn't
     * touch D2). bic_immed is the adopted Emu68 encoder Emu68 itself uses for
     * EMIT_ClearFlags. */
    out[n++] = ldr_offset(0 /*x0*/, REG_D2, M68K_OFF_CCR);   /* ldr w21,[x0,#64]  */
    out[n++] = bic_immed(REG_D2, REG_D2, 5, 0);             /* and w21,w21,#~0x1f */

    /* PC := entry_pc + 6 (3 in-block instructions of 2 bytes each consumed before
     * the RTS; the interpreter advances pc by 2 per moveq/moveq/add.l and stops at
     * rts, leaving pc = entry + 6). Track it so the full register file (incl. PC)
     * is comparable to the interpreter. Uses tmp W22=REG_D3 (block doesn't touch
     * D3). add_immed is the adopted Emu68 encoder. */
    out[n++] = ldr_offset(0 /*x0*/, REG_D3, M68K_OFF_PC);    /* ldr w22,[x0,#68]  */
    out[n++] = add_immed(REG_D3, REG_D3, 6);                /* add w22,w22,#6    */
    out[n++] = str_offset(0 /*x0*/, REG_D3, M68K_OFF_PC);    /* str w22,[x0,#68]  */

    /* Flush live regs back to the state struct (the RTS epilogue). */
    out[n++] = str_offset(0 /*x0*/, REG_D0, M68K_OFF_D(0));  /* str w19,[x0,#0]  */
    out[n++] = str_offset(0 /*x0*/, REG_D1, M68K_OFF_D(1));  /* str w20,[x0,#4]  */
    out[n++] = str_offset(0 /*x0*/, REG_D2, M68K_OFF_CCR);   /* str w21,[x0,#64] */

    out[n++] = ret();                                        /* rts              */
    return n;
}

jit68k_block_fn jit68k_build_block(char *errbuf, unsigned errlen)
{
    uint32_t staging[64];
    unsigned nwords = emit_block(staging);

    if (jit_region_alloc(&g_region, nwords * sizeof(uint32_t)) != 0) {
        if (errbuf) snprintf(errbuf, errlen, "jit_region_alloc(MAP_JIT) failed");
        return NULL;
    }
    g_region_live = 1;

    /* Push the staged AArch64 words into the MAP_JIT region through the [J1]
     * W^X dance: open the per-thread write window, copy, close, i-cache flush. */
    jit_write_begin(&g_region);
    memcpy(g_region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&g_region);
    jit_finalize(&g_region, g_region.base, nwords * sizeof(uint32_t));

    return (jit68k_block_fn)(void *)g_region.base;
}

void jit68k_free_block(void)
{
    if (g_region_live) {
        jit_region_free(&g_region);
        g_region_live = 0;
    }
}
