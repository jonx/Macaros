/* j4_build.c — [J4] run path: translate the relocated entry hunk via the ADOPTED
 * Emu68 emitter and execute it (GLUE, OURS, AROS-licensed).
 *
 * This reuses the [J2] translate->emit->run pipeline: it hand-decodes the
 * REGISTER-ONLY entry code hunk (read from the RELOCATED sandbox, not a hardcoded
 * constant) and turns each opcode into AArch64 by CALLING the adopted Emu68
 * encoders (the verbatim, MPL-quarantined emu68/A64.h), writes the words into the
 * [J1] MAP_JIT region via the jit_region API, and runs the block under W^X. The
 * 68k d0 the block leaves is the program's exit code.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS
 * (AROS-licensed). It #includes the quarantined Emu68 emitter and CALLS its inline
 * encoders; per MPL-2.0 / Mozilla FAQ Q11-Q13 that does NOT relicense this file. No
 * Emu68 source is copied here. Only encoder functions are invoked; none of Emu68's
 * runtime (RA_*, MMU, cache, MainLoop) is linked.
 *
 * ============================= SCOPE / DEFERRALS ===============================
 * This is the [J2]/[J3] decoder shortcut, carried forward intentionally and stated
 * in the spec's [J4]/[J5] split:
 *   - We HAND-DECODE only the register-only opcodes the [J2] path already handles:
 *     moveq #N,d0  and  rts. The FULL Emu68 decoder + register-allocator lift
 *     (M68k_LINE*.c, M68k_EA/CC/SR.c, RegisterAllocator64.c — memory loads/stores,
 *     branches, real jsr-through-vector) is the [J5] mountain, explicitly deferred.
 *   - moveq is emitted via mov_immed_u16 (ZERO-extend), correct only because the
 *     test's immediate is < 0x80 (real moveq SIGN-extends the 8-bit immediate). This
 *     is the documented [J2] shortcut; generalising it is [J5] decoder work. We
 *     REJECT a moveq immediate >= 0x80 here so the shortcut can never silently lie.
 * ===============================================================================
 */
#include "j4_hunk.h"
#include "jit_region.h"
#include "emu68/A64.h"      /* adopted MPL emitter — encoders only, called not copied */

#include <string.h>
#include <stdio.h>

/* The single external symbol A64.h's ASSERT_REG macro references (never called for
 * valid register numbers — we only pass registers < 32). No-op satisfies the link. */
__attribute__((weak)) void kprintf(const char *format, ...) { (void)format; }

static jit_region g_region;
static int        g_region_live = 0;

/* Big-endian 16-bit fetch from the 68k stream (the sandbox is big-endian). */
static uint16_t fetch_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Decode the register-only entry block from `code` (the relocated sandbox bytes of
 * the entry code hunk, `code_len` bytes) and emit the equivalent AArch64 into `out`.
 * Returns the number of 32-bit words on success, or 0 on a decode/emit error
 * (errbuf set). Supported opcodes (the [J2] register-only subset):
 *   moveq #imm8,Dn   (0111 ddd 0 iiiiiiii)   imm8 < 0x80 (zero/sign agree)
 *   add.l  Dm,Dn     (1101 ddd 010 000 mmm)  register-direct .L add
 *   rts              (0x4E75)                 ends the block
 *
 * Prologue loads D0..D7 from the state struct (so a caller-seeded file is honoured
 * and untouched regs are preserved); epilogue flushes them back and `ret`s (the 68k
 * RTS landing at the dispatcher funnel). */
static unsigned emit_entry(const uint8_t *code, uint32_t code_len, uint32_t *out,
                           char *errbuf, unsigned errlen)
{
#define EFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 0; } while (0)

    unsigned n = 0;
    static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3,
                                      REG_D4, REG_D5, REG_D6, REG_D7 };

    /* AAPCS64 PRESERVE: the Emu68 m68k->AArch64 map puts D0..D7 in x19..x26, which
     * are CALLEE-SAVED in AAPCS64 — the C caller (j4_run_entry) may hold live values
     * there across `block(&st)`. The translated body clobbers them, so the block must
     * save/restore x19..x28 (covers D0..D7 = x19..x26, plus x27/x28 for any future
     * A5..A7 use) and x29/x30. This is the host-context save Emu68's MainLoop does
     * before entering a translated unit; we do it inline so the block is a valid
     * AAPCS64 function pointer. stp/ldp are the adopted Emu68 encoders. */
    out[n++] = stp64_preindex(31 /*sp*/, 29 /*x29*/, 30 /*x30*/, -16);
    out[n++] = stp64_preindex(31 /*sp*/, 19, 20, -16);
    out[n++] = stp64_preindex(31 /*sp*/, 21, 22, -16);
    out[n++] = stp64_preindex(31 /*sp*/, 23, 24, -16);
    out[n++] = stp64_preindex(31 /*sp*/, 25, 26, -16);
    out[n++] = stp64_preindex(31 /*sp*/, 27, 28, -16);

    /* Prologue: load D0..D7 from the state struct (x0). */
    for (int i = 0; i < 8; i++)
        out[n++] = ldr_offset(0 /*x0*/, reg_d[i], J4_OFF_D(i));

    /* Walk the big-endian opcode stream of the entry hunk. */
    uint32_t ip = 0;
    int saw_rts = 0;
    while (ip + 2 <= code_len) {
        uint16_t op = fetch_be16(code + ip);
        ip += 2;

        if ((op & 0xF100u) == 0x7000u) {
            /* moveq #imm8,Dn */
            unsigned dn  = (op >> 9) & 7u;
            unsigned imm = op & 0xFFu;
            if (imm >= 0x80u)
                EFAIL("moveq immediate >= 0x80 needs sign-extend (deferred to [J5])");
            out[n++] = mov_immed_u16(reg_d[dn], (uint16_t)imm, 0);   /* mov w_dn,#imm */
            continue;
        }

        if ((op & 0xF000u) == 0xD000u) {
            /* add.l Dm,Dn (register-direct, opmode 010b, ea-mode 000b) */
            unsigned dn     = (op >> 9) & 7u;
            unsigned opmode = (op >> 6) & 7u;
            unsigned eamode = (op >> 3) & 7u;
            unsigned eareg  = op & 7u;
            if (opmode != 2 || eamode != 0)
                EFAIL("only register-direct add.l Dm,Dn supported (rest is [J5])");
            out[n++] = add_reg(reg_d[dn], reg_d[dn], reg_d[eareg], LSL, 0); /* add w_dn,w_dn,w_dm */
            continue;
        }

        if (op == 0x4E75u) {                 /* rts — end of block */
            saw_rts = 1;
            break;
        }

        EFAIL("unsupported opcode in entry (register-only subset only; rest is [J5])");
    }

    if (!saw_rts)
        EFAIL("entry block did not terminate in rts");

    /* Epilogue: flush D0..D7 back to the state struct... */
    for (int i = 0; i < 8; i++)
        out[n++] = str_offset(0 /*x0*/, reg_d[i], J4_OFF_D(i));

    /* ...restore the callee-saved registers (reverse order of the prologue stp), then
     * ret (the 68k RTS landing at the dispatcher funnel). */
    out[n++] = ldp64_postindex(31 /*sp*/, 27, 28, 16);
    out[n++] = ldp64_postindex(31 /*sp*/, 25, 26, 16);
    out[n++] = ldp64_postindex(31 /*sp*/, 23, 24, 16);
    out[n++] = ldp64_postindex(31 /*sp*/, 21, 22, 16);
    out[n++] = ldp64_postindex(31 /*sp*/, 19, 20, 16);
    out[n++] = ldp64_postindex(31 /*sp*/, 29 /*x29*/, 30 /*x30*/, 16);
    out[n++] = ret();

    return n;
#undef EFAIL
}

/* The compiled entry block: AAPCS64 void(*)(struct j4_m68k_state*). */
typedef void (*j4_block_fn)(struct j4_m68k_state *st);

int j4_run_entry(j4_sandbox *sb, const j4_seglist *seglist, uint32_t *exit_d0,
                 char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)

    if (!seglist->entry) RFAIL("no entry PC");

    /* Locate the entry code hunk's relocated bytes in the sandbox. The entry is the
     * first CODE hunk's runtime base; find its size from the seglist. */
    uint32_t code_len = 0;
    for (int i = 0; i < seglist->numhunks; i++) {
        if (seglist->hunk_base[i] == seglist->entry) { code_len = seglist->hunk_size[i]; break; }
    }
    if (code_len == 0) RFAIL("entry hunk has zero length");

    const uint8_t *code = j4_sandbox_host(sb, seglist->entry);

    /* Translate the entry block via the adopted Emu68 emitter. */
    uint32_t staging[256];
    unsigned nwords = emit_entry(code, code_len, staging, errbuf, errlen);
    if (nwords == 0) return 1;            /* errbuf already set by emit_entry */

    if (jit_region_alloc(&g_region, nwords * sizeof(uint32_t)) != 0)
        RFAIL("jit_region_alloc(MAP_JIT) failed");
    g_region_live = 1;

    /* [J1] W^X dance: open the per-thread write window, copy, close, i-cache flush. */
    jit_write_begin(&g_region);
    memcpy(g_region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&g_region);
    jit_finalize(&g_region, g_region.base, nwords * sizeof(uint32_t));

    /* Run it. Seed a zeroed 68k state; the block reads/writes D0..D7. */
    struct j4_m68k_state st;
    memset(&st, 0, sizeof(st));
    st.pc = seglist->entry;

    j4_block_fn block = (j4_block_fn)(void *)g_region.base;
    block(&st);                            /* <-- executes freshly-emitted AArch64 under W^X */

    if (exit_d0) *exit_d0 = st.d[0];
    return 0;
#undef RFAIL
}

void j4_run_free(void)
{
    if (g_region_live) {
        jit_region_free(&g_region);
        g_region_live = 0;
    }
}
