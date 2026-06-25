/* j5d_engine.c — [J5d] the little JIT engine: a per-basic-block translator driving
 * Emu68's REAL decoders + OUR re-hosted dispatcher ("MainLoop") owning inter-block
 * control flow + the (An) sandbox-memory EA + the jsr-through-vector [J3] bridge.
 * (GLUE, OURS, AROS-licensed.)
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS.
 * It #includes the quarantined Emu68 headers + CALLS the REAL decoders' line-entry
 * functions (EMIT_move/line5/line8/line9/lineB/lineC/lineD/moveq) and the A64.h
 * encoders. Per MPL FAQ Q11-Q13 calling/linking does not relicense this file; no Emu68
 * source is copied here. The REAL decoder objects (built from the vendored emu68 .c via
 * the darwinize generator) are linked in.
 *
 * ============================== THE DISPATCH MODEL =============================
 * Emu68's own model: each translated block exits via RET to a central C MainLoop that
 * re-reads the m68k PC. We re-host exactly that. j5d_run():
 *   1. translate_block(pc): pre-decode the straight-line opcodes from `pc` up to a
 *      control-flow TERMINATOR (Bcc/BRA/JSR/RTS) or a dispatcher-handled opcode (LEA),
 *      driving the REAL Emu68 decoders for each data/ALU/move/memory opcode, into a
 *      MAP_JIT region. Cache by entry PC (the [J5d] ICache).
 *   2. run the block under W^X (it updates D/A/CCR in struct j5d_m68k_state).
 *   3. decode the terminator in C and compute the next PC:
 *        rts          -> pop nothing (top-level): DONE, exit_d0 = d0.
 *        bra.s        -> pc = target.
 *        bcc.s        -> read the REAL CCR the decoders produced; branch or fall.
 *        jsr d16(A6)  -> map to (libbase,LVO) via the negative-offset rule; call the
 *                        library bridge ([J3] marshaller into the native stub); fall
 *                        through to the return address (jsr is 4 bytes).
 *        lea abs.l,An -> set An = the relocated abs32 (an address compute, not memory).
 *   4. loop. A step cap + a translated-block cap bound runaway.
 *
 * The heavy SEMANTIC work (every register/ALU/flag/memory opcode) is the REAL Emu68
 * decoders'. The dispatcher only owns block boundaries, the branch decision (using the
 * REAL flags), the (An) sandbox addressing setup, the LEA address compute, and the LVO
 * bridge. lea/bcc/bra/jsr are decoded straight from the instruction stream.
 * ===============================================================================
 */
#include "j5d_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"
#include "j3_jit68k.h"          /* j3_vector_recognise: the negative-offset LVO math */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ---- the REAL Emu68 line-decoder entry points (linked decoder objects) ---- */
extern uint32_t *EMIT_moveq(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_move (uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line5(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line8(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line9(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineB(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineC(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineD(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);

/* The (An)-class EA-emit counter (j5d_ea_helpers.c): each sandbox memory access the
 * rewritten EA decoder emits bumps it, so the engine can report real memory traffic. */
extern unsigned long g_j5d_ea_emits;

/* OUR RA reset + PC accumulator + CCR/PC flush + the HOOK 2 fetch base (j5c_*). */
extern void      j5c_ra_reset(void);
extern int32_t   _pc_rel;
extern void      RA_FlushCC(uint32_t **ptr);
extern uint32_t *EMIT_FlushPC(uint32_t *ptr);
extern void      j5c_fetch_set_base(const void *host_stream);
extern void      j5c_ra_get_masks(uint16_t *live, uint16_t *dirty);  /* [J5e] liveness */

/* The Emu68 m68k->ARM map for our prologue/epilogue (REG_* from A64.h). */
static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7 };
static const uint8_t reg_a[8] = { REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7 };

#define J5D_BASEADJ_X  12u   /* x12 = sandbox base-adjust (host_mem - origin)        */

/* ================================ stats / trace ================================ */
static j5d_stats g_stats;
void j5d_get_stats(j5d_stats *out) { *out = g_stats; }

/* ============================ the [J5d] block ICache =========================== */
/* One MAP_JIT region per distinct entry PC. The compiled block is a C function
 * `void block(struct j5d_m68k_state *st, uint64_t base_adjust)`. */
#define J5D_MAX_BLOCKS 64
typedef void (*j5d_block_fn)(struct j5d_m68k_state *st, uint64_t base_adjust);
typedef struct {
    uint32_t   pc;          /* entry PC of this block         */
    uint32_t   end_pc;      /* PC of the terminator opcode    */
    jit_region region;      /* the MAP_JIT code               */
    int        live;
} j5d_cached_block;
static j5d_cached_block g_cache[J5D_MAX_BLOCKS];
static int              g_cache_n = 0;

void j5d_run_free(void)
{
    for (int i = 0; i < g_cache_n; i++)
        if (g_cache[i].live) { jit_region_free(&g_cache[i].region); g_cache[i].live = 0; }
    g_cache_n = 0;
}

static j5d_cached_block *cache_find(uint32_t pc)
{
    for (int i = 0; i < g_cache_n; i++)
        if (g_cache[i].live && g_cache[i].pc == pc) return &g_cache[i];
    return NULL;
}

/* ============================ big-endian stream reads ========================== */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  be16s(const uint8_t *p){ return (int16_t)be16(p); }

/* Is `op` a terminator the DISPATCHER handles (not driven through a real decoder)? */
static int is_terminator(uint16_t op)
{
    if (op == 0x4E75u) return 1;                 /* rts                            */
    if (op == 0x4EAEu) return 1;                 /* jsr d16(A6)                    */
    if ((op & 0xFF00u) == 0x6000u) return 1;     /* bra.s  (0x60xx, disp != 00/ff) */
    if ((op & 0xFF00u) == 0x6600u) return 1;     /* bne.s                          */
    if ((op & 0xFF00u) == 0x6700u) return 1;     /* beq.s                          */
    if ((op & 0xF1FFu) == 0x41F9u) return 1;     /* lea abs.l,An (dispatcher-decoded)*/
    return 0;
}

/* ====================== translate ONE straight-line basic block ================
 * From entry PC, drive the REAL decoders over the opcodes until (and excluding) the
 * first terminator. Emits the AArch64 into `out`; returns word count, sets *end_pc to
 * the terminator's PC. 0 + errbuf on a decode/emit error. */
static unsigned translate_block(j5d_sandbox *sb, uint32_t pc, uint32_t *out,
                                uint32_t *end_pc, char *errbuf, unsigned errlen)
{
#define TFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s @pc=%08x", (msg), pc); return 0; } while (0)
    j5c_ra_reset();
    _pc_rel = 0;

    /* ========================== [J5e] THE BLOCK-SCOPED REGISTER ALLOCATOR ==========
     * Emu68's REAL decoders already keep the 68k file in fixed host regs across the whole
     * block (D0..D7=w19..w26, An=w13..w17,w27..w29) — no per-op spill inside the decoders.
     * What [J5d] did naively was bracket every block with a FIXED frame: load all 16 Dn/An
     * in the prologue + store all 16 back in the epilogue, regardless of what the block
     * touches (32 state-struct ldr/str/block). [J5e] makes that frame minimal:
     *   - emit the decoder BODY into a temp buffer FIRST, so the RA's live-in/dirty masks
     *     are known (the masks are only complete once every opcode is decoded);
     *   - then compose: prologue loads ONLY live-in regs (read before written), body,
     *     epilogue stores back ONLY dirty regs (written by the block).
     * The body is position-independent (no intra-block branches in a [J5d] straight-line
     * block; branches are inter-block via the dispatcher), so emitting it to a temp buffer
     * and memcpy'ing it after the prologue is sound. The fixed map means a register that
     * is read but never written keeps its prologue-loaded value through to the epilogue,
     * and the epilogue need not store it (not dirty). */
    uint32_t body[8000];
    uint32_t *bp = body;

    /* Point the HOOK 2 fetch base at the sandbox host bytes for this block's PC. */
    const uint8_t *blk_host = sb->host_mem + (pc - sb->origin);
    j5c_fetch_set_base(blk_host);

    uint16_t *m68k_ptr = (uint16_t *)(uintptr_t)blk_host;
    uint32_t  cur_pc   = pc;
    int guard = 0;

    for (;;) {
        if (++guard > 256) TFAIL("block decode guard tripped");
        if (cur_pc < sb->origin || (uint64_t)cur_pc + 2 > (uint64_t)sb->origin + sb->size)
            TFAIL("pc out of sandbox during translate");
        if ((size_t)(bp - body) > sizeof(body)/sizeof(body[0]) - 256) TFAIL("block body overflow");

        const uint8_t *ophost = sb->host_mem + (cur_pc - sb->origin);
        uint16_t op = be16(ophost);

        if (is_terminator(op)) { *end_pc = cur_pc; break; }

        uint8_t group = op >> 12;
        uint16_t insn_consumed = 0;
        uint16_t *before = m68k_ptr;

        switch (group) {
            case 0x2: bp = EMIT_move (bp, &m68k_ptr, &insn_consumed); break; /* move.l/movea.l */
            case 0x7: bp = EMIT_moveq(bp, &m68k_ptr, &insn_consumed); break; /* moveq          */
            case 0x5: bp = EMIT_line5(bp, &m68k_ptr, &insn_consumed); break; /* addq/subq      */
            case 0x8: bp = EMIT_line8(bp, &m68k_ptr, &insn_consumed); break; /* or/div         */
            case 0x9: bp = EMIT_line9(bp, &m68k_ptr, &insn_consumed); break; /* sub            */
            case 0xB: bp = EMIT_lineB(bp, &m68k_ptr, &insn_consumed); break; /* cmp/eor        */
            case 0xC: bp = EMIT_lineC(bp, &m68k_ptr, &insn_consumed); break; /* and/mul        */
            case 0xD: bp = EMIT_lineD(bp, &m68k_ptr, &insn_consumed); break; /* add            */
            default:  TFAIL("opcode line not in the [J5d] driven set (2/5/7/8/9/B/C/D + terminators)");
        }
        if (insn_consumed == 0) TFAIL("decoder consumed 0 insns (unimplemented opcode?)");

        /* The real decoder advanced m68k_ptr by (insn_consumed + ext words). Mirror the
         * PC by the same byte distance so cur_pc tracks the stream. */
        uint32_t advanced = (uint32_t)((m68k_ptr - before) * 2u);
        if (advanced == 0) TFAIL("decoder did not advance the stream");
        cur_pc += advanced;
        g_stats.insns_decoded += insn_consumed;
    }

    /* The CCR + PC delta flush is part of the body's exit work (touches scratch regs only,
     * not the architectural file). Append it to the body so the masks are final. */
    RA_FlushCC(&bp);
    bp = EMIT_FlushPC(bp);
    unsigned body_words = (unsigned)(bp - body);

    /* Now the live-in/dirty masks are complete. Compose the minimal frame. */
    uint16_t live = 0, dirty = 0;
    j5c_ra_get_masks(&live, &dirty);

    uint32_t *ptr = out;

    /* AAPCS64 callee-saved preserve (the [J4]/[J5c] fix): w19..w28 + x29/x30. We keep the
     * FULL preserve set (cheap, 6 stp/6 ldp, off the per-op hot path) so the frame is
     * always well-formed regardless of which Dn/An the block uses; the [J5e] win is the
     * eliminated STATE-STRUCT traffic (the prologue/epilogue ldr/str below), which is what
     * the per-op memory-traffic measurement targets. */
    *ptr++ = stp64_preindex(31, 29, 30, -16);
    *ptr++ = stp64_preindex(31, 19, 20, -16);
    *ptr++ = stp64_preindex(31, 21, 22, -16);
    *ptr++ = stp64_preindex(31, 23, 24, -16);
    *ptr++ = stp64_preindex(31, 25, 26, -16);
    *ptr++ = stp64_preindex(31, 27, 28, -16);

    /* x12 := x1 (the sandbox base-adjust), used by j5d_ea_mem for (An) accesses. */
    *ptr++ = mov64_reg(J5D_BASEADJ_X, 1 /*x1*/);

    /* [J5e] PROLOGUE: load ONLY the live-in 68k regs (read before written) from the
     * state (x0). A register the block writes before reading (e.g. moveq #imm,Dn) is NOT
     * loaded — its prologue ldr was pure waste in the naive frame. */
    unsigned reg_loads = 0;
    for (int i = 0; i < 8; i++) if (live & (1u << i))      { *ptr++ = ldr_offset(0, reg_d[i], J5D_OFF_D(i)); reg_loads++; }
    for (int i = 0; i < 8; i++) if (live & (1u << (i+8)))  { *ptr++ = ldr_offset(0, reg_a[i], J5D_OFF_A(i)); reg_loads++; }

    /* The decoder body (every register/ALU/flag/move/memory opcode, REAL Emu68 decoders). */
    memcpy(ptr, body, body_words * sizeof(uint32_t));
    ptr += body_words;

    /* [J5e] EPILOGUE: store back ONLY the dirty 68k regs (written by the block). A register
     * the block only read keeps the value the caller's state already holds, so no store. */
    unsigned reg_stores = 0;
    for (int i = 0; i < 8; i++) if (dirty & (1u << i))     { *ptr++ = str_offset(0, reg_d[i], J5D_OFF_D(i)); reg_stores++; }
    for (int i = 0; i < 8; i++) if (dirty & (1u << (i+8))) { *ptr++ = str_offset(0, reg_a[i], J5D_OFF_A(i)); reg_stores++; }

    *ptr++ = ldp64_postindex(31, 27, 28, 16);
    *ptr++ = ldp64_postindex(31, 25, 26, 16);
    *ptr++ = ldp64_postindex(31, 23, 24, 16);
    *ptr++ = ldp64_postindex(31, 21, 22, 16);
    *ptr++ = ldp64_postindex(31, 19, 20, 16);
    *ptr++ = ldp64_postindex(31, 29, 30, 16);
    *ptr++ = ret();

    /* [J5e] metrics: per-block state-struct traffic, before vs after. */
    g_stats.state_ldrstr_naive += 32;                       /* old: 16 loads + 16 stores  */
    g_stats.state_ldrstr_ra    += reg_loads + reg_stores;   /* new: live-in + dirty        */
    g_stats.reg_loads_emitted  += reg_loads;
    g_stats.reg_stores_emitted += reg_stores;

    return (unsigned)(ptr - out);
#undef TFAIL
}

/* Get (translating + caching if needed) the compiled block at `pc`. */
static j5d_cached_block *get_block(j5d_sandbox *sb, uint32_t pc, char *errbuf, unsigned errlen)
{
    j5d_cached_block *b = cache_find(pc);
    if (b) return b;
    if (g_cache_n >= J5D_MAX_BLOCKS) { if (errbuf) snprintf(errbuf, errlen, "block cache full"); return NULL; }

    uint32_t staging[8192];
    uint32_t end_pc = pc;
    unsigned long ea_before = g_j5d_ea_emits;
    unsigned nwords = translate_block(sb, pc, staging, &end_pc, errbuf, errlen);
    if (nwords == 0) return NULL;
    g_stats.mem_accesses += (uint32_t)(g_j5d_ea_emits - ea_before);
    if (nwords > sizeof(staging)/sizeof(staging[0])) { if (errbuf) snprintf(errbuf, errlen, "emit overflow"); return NULL; }

    b = &g_cache[g_cache_n];
    if (jit_region_alloc(&b->region, nwords * sizeof(uint32_t)) != 0) {
        if (errbuf) snprintf(errbuf, errlen, "jit_region_alloc(MAP_JIT) failed"); return NULL;
    }
    jit_write_begin(&b->region);
    memcpy(b->region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&b->region);
    jit_finalize(&b->region, b->region.base, nwords * sizeof(uint32_t));

    b->pc = pc; b->end_pc = end_pc; b->live = 1;
    g_cache_n++;
    g_stats.blocks_translated++;
    g_stats.arm_words_emitted += nwords;
    return b;
}

/* ============================== the dispatcher ================================= */
int j5d_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
            struct j5d_m68k_state *st, uint32_t *exit_d0,
            j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)
    memset(&g_stats, 0, sizeof(g_stats));
    st->a[6] = a6_libbase;                          /* A6 = library base (AmigaOS)  */
    st->pc   = entry_pc;

    uint64_t base_adjust = (uint64_t)(uintptr_t)sb->host_mem - (uint64_t)sb->origin;
    uint32_t pc = entry_pc;
    uint32_t steps = 0;

    for (;;) {
        if (++steps > 2000000u) RFAIL("dispatcher step cap (runaway)");

        /* (1)+(2): translate (if needed) + run the straight-line block at pc. */
        j5d_cached_block *b = get_block(sb, pc, errbuf, errlen);
        if (!b) return 1;                            /* errbuf set                  */

        st->pc = pc;
        ((j5d_block_fn)(void *)b->region.base)(st, base_adjust);
        g_stats.blocks_executed++;

        /* (3): decode the terminator in C and compute the next PC. */
        uint32_t tpc = b->end_pc;
        if (tpc + 2 > sb->origin + sb->size) RFAIL("terminator pc out of sandbox");
        const uint8_t *thost = sb->host_mem + (tpc - sb->origin);
        uint16_t top = be16(thost);

        if (top == 0x4E75u) {                        /* rts (top-level) -> DONE     */
            *exit_d0 = st->d[0];
            return 0;
        }
        else if (top == 0x4EAEu) {                   /* jsr d16(A6) -> [J3] bridge  */
            int16_t d16 = be16s(thost + 2);
            uint32_t target = st->a[6] + (uint32_t)(int32_t)d16;
            int n = j3_vector_recognise(st->a[6], target);  /* negative-offset rule */
            if (n < 0) RFAIL("jsr(A6): target not a valid library vector");
            if (!lvo)  RFAIL("jsr(A6): library call but no bridge registered");
            char e2[160] = {0};
            /* The bridge marshals 68k regs (in *st) into the native stub via the REAL
             * [J3] marshaller and writes any return into st->d[0]. */
            if (lvo(n, (struct j5d_m68k_state *)st, user, e2, sizeof e2)) {
                if (errbuf) snprintf(errbuf, errlen, "lib bridge: %s", e2);
                return 1;
            }
            g_stats.lib_calls++;
            pc = tpc + 4;                            /* jsr d16(A6) is 4 bytes      */
        }
        else if ((top & 0xFF00u) == 0x6000u ||       /* bra.s                       */
                 (top & 0xFF00u) == 0x6600u ||       /* bne.s                       */
                 (top & 0xFF00u) == 0x6700u) {       /* beq.s                       */
            int8_t disp8 = (int8_t)(top & 0xFFu);
            if (disp8 == 0)  RFAIL("bcc.W/.L not in the [J5d] subset (8-bit only)");
            if (disp8 == -1) RFAIL("bcc.L not in the [J5d] subset");
            uint32_t taken = (uint32_t)((int64_t)tpc + 2 + disp8);
            uint32_t fall  = tpc + 2;
            int take;
            if      ((top & 0xFF00u) == 0x6000u) take = 1;                       /* bra */
            else if ((top & 0xFF00u) == 0x6600u) take = !(st->ccr & J5D_CCR_Z);  /* bne */
            else                                 take =  (st->ccr & J5D_CCR_Z);  /* beq */
            pc = take ? taken : fall;
        }
        else if ((top & 0xF1FFu) == 0x41F9u) {       /* lea abs.l,An (address compute) */
            unsigned an = (top >> 9) & 7u;
            uint32_t abs32 = ((uint32_t)thost[2] << 24) | ((uint32_t)thost[3] << 16) |
                             ((uint32_t)thost[4] << 8)  |  (uint32_t)thost[5];
            st->a[an] = abs32;                       /* the loader already relocated it */
            pc = tpc + 6;                            /* lea abs.l is 6 bytes        */
        }
        else {
            RFAIL("unknown terminator opcode");
        }
    }
#undef RFAIL
}
