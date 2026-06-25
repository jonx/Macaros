/* j5b_build.c — [J5b] run path: translate a self-contained 68k LOOP via the ADOPTED
 * Emu68 EMITTER, with a HAND-ROLLED loop decode + REAL condition codes + a single-region
 * INTERNAL BACKWARD BRANCH (GLUE, OURS, AROS-licensed).
 *
 * This extends the [J2]/[J4]/[J5a] translate->emit->run pipeline to CONTROL FLOW. It
 * hand-decodes a small loop (read from the RELOCATED sandbox), turns each opcode into
 * AArch64 by CALLING the adopted Emu68 encoders (the verbatim, MPL-quarantined
 * emu68/A64.h), writes the words into the [J1] MAP_JIT region via the jit_region API,
 * and runs the block under W^X.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS
 * (AROS-licensed). It #includes the quarantined Emu68 emitter and CALLS its inline
 * encoders; per MPL-2.0 / Mozilla FAQ Q11-Q13 that does NOT relicense this file. No
 * Emu68 source is copied here. Only encoder functions are invoked; NONE of Emu68's
 * runtime (RA_*, M68k_EA.c, cache.c, M68k_Translator, MainLoop) is linked. Per the
 * [J5a] adoption finding (j5a_jit68k.h), Emu68's EA decode + register allocator do NOT
 * lift incrementally; the decode/branch/CCR logic here is OURS, built around the
 * emitter that DID lift cleanly. NO NEW Emu68 file is vendored for [J5b] — only the
 * existing A64.h encoders are used, so the Exhibit-B check is unchanged.
 *
 * ===================== REAL CONDITION CODES + THE BRANCH ========================
 *   subq.l #1,d1   ->   subs w_d1, w_d1, #1            (FLAG-SETTING subtract, A64.h)
 *   bne.s  L       ->   b.ne (A64_CC_NE, Z==0) back to the loop-top word index
 * The b.ne consumes the NZCV the subs just produced (the AArch64 flags ARE the live
 * 68k branch condition). The 68k CCR (state->ccr) is ALSO recomputed to the full
 * N/Z/V/C/X with NON-flag-setting ops (cset/orr/str), emitted BETWEEN the subs and the
 * b.ne so the branch still sees the subs flags. 68k subtract C = borrow = AArch64
 * carry-CLEAR (A64_CC_CC), the opposite of AArch64's C bit — derived explicitly.
 * ===============================================================================
 *
 * ====================== SINGLE-REGION BACKWARD BRANCH ===========================
 * The whole loop is emitted ONCE. We record, for each 68k opcode at byte offset `ip`,
 * the OUTPUT WORD INDEX where its emitted AArch64 begins (op_word_idx[]). A 68k bne.s
 * targets a 68k byte offset; we map that to the recorded output-word index and emit a
 * b.ne with the signed word offset (target_word - bne_word) — NEGATIVE for the loop's
 * backward branch. No cross-region chaining ([J5c]).
 * ===============================================================================
 *
 * ============================= SCOPE / DEFERRALS ===============================
 * Opcodes hand-decoded here (the [J5b] loop):
 *   moveq #imm8,Dn  (imm8 < 0x80 — the [J2] zero/sign-extend shortcut, enforced)
 *   add.l  Dm,Dn    (register-direct .L)
 *   subq.l #imm,Dn  (imm in 1..8) — sets REAL flags (the branch source)
 *   bne.s  disp8    (the backward branch closing the loop)
 *   rts
 * DEFERRED to [J5c]: cross-region chaining + instruction cache, full Bcc/DBcc coverage,
 * forward branches across blocks, real jsr-through-vector decode, library calls from
 * the program, and OUR register allocator (still memory-backed here).
 * ===============================================================================
 */
#include "j5b_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"      /* adopted MPL emitter — encoders only, called not copied */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

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

/* Emu68's m68k map: D0..D7 -> w19..w26. We reuse the emitter's REG_D* constants. The
 * block is entered as a C function pointer  void block(state*);  so x0 is the state
 * pointer. We use w9..w11 as caller-clobberable scratch (disjoint from the 68k map and
 * from x0) for the CCR recomputation. */
#define SCR_A   9u    /* w9: CCR accumulator                                      */
#define SCR_B  10u    /* w10: a single condition bit (from cset)                  */
#define SCR_C  11u    /* w11: spare / result copy                                 */

static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3,
                                  REG_D4, REG_D5, REG_D6, REG_D7 };

/* Emit the 68k CCR recomputation for the just-executed `subs w_dn, w_dn, #imm` into
 * state->ccr, using ONLY non-flag-setting ops so the live NZCV (which the following
 * b.ne reads) is preserved.
 *
 * 68k subtract flags (result already in w_dn): bit4=X bit3=N bit2=Z bit1=V bit0=C.
 *   N: cset from MI (N==1)        -> bit3
 *   Z: cset from EQ (Z==1)        -> bit2
 *   V: cset from VS (V==1)        -> bit1
 *   C: cset from CC (carry clear == a BORROW occurred == 68k C) -> bit0
 *   X := C                        -> bit4
 * We assemble the byte in w9 with shifted ORRs (orr_immed: bitmask-immediate of a
 * single set bit), then str it to state->ccr. cset/orr/str do not touch NZCV. */
static unsigned emit_ccr_from_subs(uint32_t *out, unsigned n)
{
    /* w9 := 0  (start the CCR accumulator clean). */
    out[n++] = mov_immed_u16(SCR_A, 0, 0);

    /* N (bit3): w10 = cset MI; w9 |= w10 << 3. */
    out[n++] = cset(SCR_B, A64_CC_MI);
    out[n++] = orr_reg(SCR_A, SCR_A, SCR_B, LSL, 3);
    /* Z (bit2): w10 = cset EQ; w9 |= w10 << 2. */
    out[n++] = cset(SCR_B, A64_CC_EQ);
    out[n++] = orr_reg(SCR_A, SCR_A, SCR_B, LSL, 2);
    /* V (bit1): w10 = cset VS; w9 |= w10 << 1. */
    out[n++] = cset(SCR_B, A64_CC_VS);
    out[n++] = orr_reg(SCR_A, SCR_A, SCR_B, LSL, 1);
    /* C (bit0): 68k C = borrow = AArch64 carry-CLEAR. w10 = cset CC; w9 |= w10. */
    out[n++] = cset(SCR_B, A64_CC_CC);
    out[n++] = orr_reg(SCR_A, SCR_A, SCR_B, LSL, 0);
    /* X (bit4) := C : reuse w10 (still the C bit); w9 |= w10 << 4. */
    out[n++] = orr_reg(SCR_A, SCR_A, SCR_B, LSL, 4);

    /* state->ccr = w9. */
    out[n++] = str_offset(0 /*x0 state*/, SCR_A, J5B_OFF_CCR);
    return n;
}

/* Decode the loop from `code` (the relocated sandbox bytes, `code_len`) and emit the
 * equivalent AArch64 into `out`. Returns the number of 32-bit words on success, or 0 on
 * a decode/emit error (errbuf set). `neg_break_branch` alters the emit (see header). */
static unsigned emit_block(const uint8_t *code, uint32_t code_len, int neg_break_branch,
                           uint32_t *out, char *errbuf, unsigned errlen)
{
#define EFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 0; } while (0)

    unsigned n = 0;

    /* AAPCS64 PRESERVE (the [J4] fix): D0..D7=x19..x26 overlap AArch64 callee-saved
     * x19..x28 — save/restore so the block is a valid C function pointer. */
    out[n++] = stp64_preindex(31, 29, 30, -16);
    out[n++] = stp64_preindex(31, 19, 20, -16);
    out[n++] = stp64_preindex(31, 21, 22, -16);
    out[n++] = stp64_preindex(31, 23, 24, -16);
    out[n++] = stp64_preindex(31, 25, 26, -16);
    out[n++] = stp64_preindex(31, 27, 28, -16);

    /* Prologue: load D0..D7 from the state struct (x0). */
    for (int i = 0; i < 8; i++) out[n++] = ldr_offset(0, reg_d[i], J5B_OFF_D(i));

    /* For each 68k opcode byte offset, record the OUTPUT word index where its emitted
     * AArch64 begins. Indexed by (ip/2) since every in-subset opcode is one word.
     * code_len/2 entries max; +1 guard. */
    int op_word_idx[256];
    for (unsigned k = 0; k < 256; k++) op_word_idx[k] = -1;

    /* Pending backward/forward branch fixups: (output-word index of the b.ne, 68k
     * target byte offset). Patched once all op_word_idx[] are known. */
    struct { int bne_word; uint32_t tgt_ip; int wrong_cond; } fixup[32];
    int n_fix = 0;

    uint32_t ip = 0;
    int saw_rts = 0;
    while (ip + 2 <= code_len) {
        if ((ip >> 1) >= 256u) EFAIL("block too long for [J5b] op map");
        op_word_idx[ip >> 1] = (int)n;          /* this opcode emits starting at word n */

        uint16_t op = fetch_be16(code + ip);
        uint32_t op_ip = ip;
        ip += 2;

        if ((op & 0xF100u) == 0x7000u) {            /* moveq #imm8,Dn */
            unsigned dn  = (op >> 9) & 7u;
            unsigned imm = op & 0xFFu;
            if (imm >= 0x80u)
                EFAIL("moveq immediate >= 0x80 needs sign-extend (deferred)");
            out[n++] = mov_immed_u16(reg_d[dn], (uint16_t)imm, 0);
            continue;
        }

        if ((op & 0xF1F8u) == 0xD080u) {            /* add.l Dm,Dn (register-direct) */
            unsigned dn = (op >> 9) & 7u;
            unsigned dm = op & 7u;
            out[n++] = add_reg(reg_d[dn], reg_d[dn], reg_d[dm], LSL, 0);
            continue;
        }

        if ((op & 0xF1F8u) == 0x5180u) {            /* subq.l #imm,Dn -> REAL flags */
            unsigned q   = (op >> 9) & 7u;
            unsigned imm = (q == 0) ? 8u : q;       /* q==0 => 8 */
            unsigned dn  = op & 7u;
            /* subs w_dn, w_dn, #imm — flag-setting; this is the branch's NZCV source. */
            out[n++] = subs_immed(reg_d[dn], reg_d[dn], (uint16_t)imm);
            /* Recompute the full 68k CCR into state->ccr (non-flag-setting ops only,
             * so the following b.ne still sees the subs NZCV). */
            n = emit_ccr_from_subs(out, n);
            continue;
        }

        if ((op & 0xFF00u) == 0x6600u) {            /* bne.s disp8 — the backward branch */
            int32_t disp = (int8_t)(op & 0xFFu);
            if (disp == 0)
                EFAIL("bne.W/.L (disp8==0) not in [J5b] subset");
            int64_t tgt = (int64_t)op_ip + 2 + disp;
            if (tgt < 0 || (uint64_t)tgt >= code_len || (tgt & 1))
                EFAIL("bne target out of block / misaligned");
            fixup[n_fix].bne_word   = (int)n;
            fixup[n_fix].tgt_ip     = (uint32_t)tgt;
            fixup[n_fix].wrong_cond = neg_break_branch;   /* control: force always-taken */
            n_fix++;
            out[n++] = 0;                            /* placeholder, patched below */
            continue;
        }

        if (op == 0x4E75u) { saw_rts = 1; break; }   /* rts — end of block */

        EFAIL("unsupported opcode ([J5b] subset: moveq/add.l/subq.l/bne.s/rts)");
    }

    if (!saw_rts)
        EFAIL("loop block did not terminate in rts");

    /* Epilogue: flush D0..D7 back to the state struct. */
    for (int i = 0; i < 8; i++) out[n++] = str_offset(0, reg_d[i], J5B_OFF_D(i));

    /* Restore callee-saved registers (reverse of the prologue) and ret. */
    out[n++] = ldp64_postindex(31, 27, 28, 16);
    out[n++] = ldp64_postindex(31, 25, 26, 16);
    out[n++] = ldp64_postindex(31, 23, 24, 16);
    out[n++] = ldp64_postindex(31, 21, 22, 16);
    out[n++] = ldp64_postindex(31, 19, 20, 16);
    out[n++] = ldp64_postindex(31, 29, 30, 16);
    out[n++] = ret();

    /* Patch the conditional branches now that every op_word_idx[] is known. */
    for (int i = 0; i < n_fix; i++) {
        int tgt_word = op_word_idx[fixup[i].tgt_ip >> 1];
        if (tgt_word < 0)
            EFAIL("bne target did not land on a decoded opcode");
        int32_t rel = tgt_word - fixup[i].bne_word;   /* signed word offset (negative) */
        if (fixup[i].wrong_cond)
            /* NEGATIVE CONTROL: unconditional backward branch -> infinite loop (the
             * test's watchdog must catch it). */
            out[fixup[i].bne_word] = b((uint32_t)rel);
        else
            /* b.ne back to the loop top: reads the subs NZCV (Z==0 -> taken). */
            out[fixup[i].bne_word] = b_cc(A64_CC_NE, rel);
    }

    return n;
#undef EFAIL
}

/* The compiled block: AAPCS64 void(*)(state*). */
typedef void (*j5b_block_fn)(struct j5b_m68k_state *st);

int j5b_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                  struct j5b_m68k_state *st, int neg_break_branch,
                  char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)

    if (code_len == 0) RFAIL("empty entry block");

    uint32_t staging[1024];
    unsigned nwords = emit_block(code, code_len, neg_break_branch,
                                 staging, errbuf, errlen);
    if (nwords == 0) return 1;             /* errbuf set by emit_block */

    if (jit_region_alloc(&g_region, nwords * sizeof(uint32_t)) != 0)
        RFAIL("jit_region_alloc(MAP_JIT) failed");
    g_region_live = 1;

    jit_write_begin(&g_region);
    memcpy(g_region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&g_region);
    jit_finalize(&g_region, g_region.base, nwords * sizeof(uint32_t));

    st->pc = entry_pc;

    j5b_block_fn block = (j5b_block_fn)(void *)g_region.base;
    block(st);                             /* <-- executes freshly-emitted AArch64 under W^X */

    return 0;
#undef RFAIL
}

void j5b_run_free(void)
{
    if (g_region_live) {
        jit_region_free(&g_region);
        g_region_live = 0;
    }
}
