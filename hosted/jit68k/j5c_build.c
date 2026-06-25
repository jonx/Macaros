/* j5c_build.c — [J5c] run path: translate a richer block THROUGH THE REAL EMU68 DECODERS
 * (emu68/M68k_LINE{8,9,B,C,D}.c + M68k_MOVE.c, driven via the line-dispatch), with the
 * three hosted hooks (j5c_shims.c HOOK 2 + PC/SR shims; j5c_ra.c HOOK 3 memory-backed RA;
 * HOOK 1 sandbox/EA surface present), run under W^X (GLUE, OURS, AROS-licensed).
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS. It
 * #includes the quarantined Emu68 headers + CALLS the REAL decoders' exported line-entry
 * functions (EMIT_line8/9/B/C/D, EMIT_moveq) and the A64.h encoders. Per MPL FAQ Q11-Q13
 * calling/linking does not relicense this file; no Emu68 source is copied here. The REAL
 * Emu68 decoder objects (built from the vendored emu68 .c via the darwinize generator)
 * are linked in.
 *
 * ============================ HOW THE REAL DECODER IS DRIVEN ========================
 * Emu68's per-line decoders have the signature
 *     uint32_t *EMIT_lineX(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
 * They read the opcode + extension words from *m68k_ptr via cache_read_16 (HOOK 2 reads
 * big-endian straight from the host pointer), dispatch through their own InsnTable, call
 * EMIT_LoadFromEffectiveAddress / the A64.h encoders / RA_* (HOOK 3) / the A64.h CCR
 * helpers, advance *m68k_ptr, and append AArch64 words at ptr. We point m68k_ptr DIRECTLY
 * at the sandbox host bytes for the 68k stream, then loop: group = (opcode >> 12), call
 * line_fn[group]. This is the same dispatch Emu68's EmitINSN does (M68k_Translator.c) —
 * re-hosted minimally so the decoders run unmodified.
 *
 * Around the decoder body we add OUR block frame:
 *   - AAPCS64 callee-saved preserve (the [J4] fix): the Emu68 map puts D0..D7 in w19..w26
 *     and A-regs in w13..w17/w27..w29; w19..w28 are callee-saved, so save/restore them.
 *   - prologue: load D0..D7 and A0..A7 from struct j5c_m68k_state (x0) into the mapped
 *     ARM registers, and ldr the CCR is done lazily by RA_GetCC.
 *   - epilogue: RA_FlushCC (store the CCR back), then store D0..D7 / A0..A7 to the state.
 * x0 holds the state pointer for the whole block (RA scratch never allocates x0..x3).
 * ===============================================================================
 */
#include "j5c_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"          /* encoders + REG_* map (called, not copied)            */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* The real Emu68 line-decoder entry points (defined in the linked decoder objects). */
extern uint32_t *EMIT_line8(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line9(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineB(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineC(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineD(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_moveq(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);

/* OUR RA reset (j5c_ra.c) + the PC-rel accumulator + PC flush (j5c_shims.c). */
extern void      j5c_ra_reset(void);
extern int32_t   _pc_rel;
extern void      RA_FlushCC(uint32_t **ptr);
extern uint32_t *EMIT_FlushPC(uint32_t *ptr);
extern void      j5c_fetch_set_base(const void *host_stream);   /* HOOK 2 base splice */

static jit_region g_region;
static int        g_region_live = 0;

static uint16_t fetch_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* The fixed Emu68 m68k->ARM map (D0..D7, A0..A7) for our prologue/epilogue. */
static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7 };
static const uint8_t reg_a[8] = { REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7 };

/* Drive the REAL decoders over the 68k stream `code` (the sandbox bytes), emitting into
 * `out`. Returns word count, or 0 on error (errbuf set). */
static unsigned emit_block(const uint8_t *code, uint32_t code_len, int neg_corrupt,
                           uint32_t *out, char *errbuf, unsigned errlen)
{
#define EFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 0; } while (0)

    j5c_ra_reset();
    _pc_rel = 0;

    unsigned n = 0;
    uint32_t *ptr = out;

    /* AAPCS64 callee-saved preserve (the [J4] fix): w19..w28 + x29/x30. */
    *ptr++ = stp64_preindex(31, 29, 30, -16);
    *ptr++ = stp64_preindex(31, 19, 20, -16);
    *ptr++ = stp64_preindex(31, 21, 22, -16);
    *ptr++ = stp64_preindex(31, 23, 24, -16);
    *ptr++ = stp64_preindex(31, 25, 26, -16);
    *ptr++ = stp64_preindex(31, 27, 28, -16);

    /* The block is entered as block(struct j5c_m68k_state *st) -> state in x0.  [J5g]: the
     * shared RA (j5c_ra.c) now keeps the state pointer in x1 (so w0 is free for Emu68's
     * hardcoded `cset(0,...)` flag-extraction scratch). Move state x0 -> x1 here so the
     * RA's memory-backed CCR (ldr/str [x1,#CCR]) reaches the right struct. */
    *ptr++ = mov64_reg(1 /*x1*/, 0 /*x0 = state arg*/);

    /* Prologue: load D0..D7 and A0..A7 from the state struct (x1). */
    for (int i = 0; i < 8; i++) *ptr++ = ldr_offset(1, reg_d[i], J5C_OFF_D(i));
    for (int i = 0; i < 8; i++) *ptr++ = ldr_offset(1, reg_a[i], J5C_OFF_A(i));

    /* ---- DRIVE THE REAL EMU68 DECODERS over the 68k stream ---- */
    /* m68k_ptr points DIRECTLY at the sandbox host bytes; HOOK 2 reads them big-endian. */
    uint16_t *m68k_ptr = (uint16_t *)(uintptr_t)code;
    uint16_t *m68k_end = (uint16_t *)(uintptr_t)(code + (code_len & ~1u));

    /* Optional negative control: corrupt one opcode in a scratch copy so the decoder
     * produces a different (still-valid) instruction => JIT must diverge from interp. */
    uint8_t corrupt_buf[256];
    if (neg_corrupt) {
        if (code_len > sizeof(corrupt_buf)) EFAIL("block too long for neg-control copy");
        memcpy(corrupt_buf, code, code_len);
        /* flip the FIRST opcode's low data-register field bits (e.g. add.l d2,d0 ->
         * add.l d2,d1): changes the destination register, so results diverge. */
        corrupt_buf[1] ^= 0x02;   /* perturb low byte of first opcode */
        m68k_ptr = (uint16_t *)(uintptr_t)corrupt_buf;
        m68k_end = (uint16_t *)(uintptr_t)(corrupt_buf + (code_len & ~1u));
    }

    /* HOOK 2: register the high 32 bits of the stream host base so cache_read_16 can
     * reconstruct the full 64-bit pointer from the truncated uint32_t it receives. */
    j5c_fetch_set_base(m68k_ptr);

    int guard = 0;
    while (m68k_ptr < m68k_end) {
        if (++guard > 64) EFAIL("decode guard tripped (runaway)");

        uint16_t opcode = fetch_be16((const uint8_t *)m68k_ptr);

        /* rts (0x4E75) terminates the block (handled by us, not a driven line). */
        if (opcode == 0x4E75u) break;

        uint8_t group = opcode >> 12;
        uint16_t insn_consumed = 0;

        switch (group) {
            case 0x7: ptr = EMIT_moveq(ptr, &m68k_ptr, &insn_consumed); break; /* moveq  */
            case 0x8: ptr = EMIT_line8(ptr, &m68k_ptr, &insn_consumed); break; /* or/div */
            case 0x9: ptr = EMIT_line9(ptr, &m68k_ptr, &insn_consumed); break; /* sub    */
            case 0xB: ptr = EMIT_lineB(ptr, &m68k_ptr, &insn_consumed); break; /* cmp/eor*/
            case 0xC: ptr = EMIT_lineC(ptr, &m68k_ptr, &insn_consumed); break; /* and/mul*/
            case 0xD: ptr = EMIT_lineD(ptr, &m68k_ptr, &insn_consumed); break; /* add    */
            default:  EFAIL("opcode line not in the [J5c] re-hosted set (7/8/9/B/C/D)");
        }
        if (insn_consumed == 0) EFAIL("decoder consumed 0 insns (unimplemented opcode?)");
    }

    /* Epilogue: flush the CCR back to the state (RA_FlushCC stores it if modified), then
     * flush any pending PC delta, then store the architectural registers. */
    RA_FlushCC(&ptr);
    ptr = EMIT_FlushPC(ptr);
    for (int i = 0; i < 8; i++) *ptr++ = str_offset(1, reg_d[i], J5C_OFF_D(i));
    for (int i = 0; i < 8; i++) *ptr++ = str_offset(1, reg_a[i], J5C_OFF_A(i));

    /* Restore callee-saved registers (reverse) + ret. */
    *ptr++ = ldp64_postindex(31, 27, 28, 16);
    *ptr++ = ldp64_postindex(31, 25, 26, 16);
    *ptr++ = ldp64_postindex(31, 23, 24, 16);
    *ptr++ = ldp64_postindex(31, 21, 22, 16);
    *ptr++ = ldp64_postindex(31, 19, 20, 16);
    *ptr++ = ldp64_postindex(31, 29, 30, 16);
    *ptr++ = ret();

    n = (unsigned)(ptr - out);
    return n;
#undef EFAIL
}

typedef void (*j5c_block_fn)(struct j5c_m68k_state *st);

int j5c_run_block(const j5c_sandbox *sb, uint32_t entry_pc, const uint8_t *code,
                  uint32_t code_len, struct j5c_m68k_state *st,
                  int neg_corrupt_decode, char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)
    (void)sb;

    if (code_len == 0) RFAIL("empty entry block");

    uint32_t staging[4096];
    unsigned nwords = emit_block(code, code_len, neg_corrupt_decode, staging, errbuf, errlen);
    if (nwords == 0) return 1;
    if (nwords > sizeof(staging) / sizeof(staging[0])) RFAIL("emit overflow");

    if (jit_region_alloc(&g_region, nwords * sizeof(uint32_t)) != 0)
        RFAIL("jit_region_alloc(MAP_JIT) failed");
    g_region_live = 1;

    jit_write_begin(&g_region);
    memcpy(g_region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&g_region);
    jit_finalize(&g_region, g_region.base, nwords * sizeof(uint32_t));

    st->pc = entry_pc;

    j5c_block_fn block = (j5c_block_fn)(void *)g_region.base;
    block(st);              /* <-- executes the REAL-decoder-emitted AArch64 under W^X */

    return 0;
#undef RFAIL
}

void j5c_run_free(void)
{
    if (g_region_live) { jit_region_free(&g_region); g_region_live = 0; }
}
