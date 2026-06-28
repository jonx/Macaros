/* j5d_engine.c — the little JIT engine: a per-basic-block translator driving Emu68's
 * REAL decoders + OUR re-hosted dispatcher ("MainLoop") owning inter-block control flow
 * + the (An) sandbox-memory EA + the jsr-through-vector [J3] bridge.  [J5f] generalises
 * the dispatcher into a PC-DRIVEN loop with a REAL 68k RETURN STACK (nested bsr/jsr/rts +
 * computed jmp(An)/jsr(An) + the full Bcc displacement widths) over a PC-keyed BLOCK CACHE.
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
 *   1. get_block(pc): look the block up in the PC-keyed BLOCK CACHE; on a miss,
 *      translate_block(pc) pre-decodes the straight-line opcodes from `pc` up to a
 *      control-flow TERMINATOR, driving the REAL Emu68 decoders for each data/ALU/move/
 *      memory opcode, into a MAP_JIT region, and caches it by entry PC. A loop body or a
 *      repeatedly-called subroutine therefore translates ONCE — a cache HIT thereafter.
 *   2. run the block under W^X (it updates D/A/CCR in struct j5d_m68k_state).
 *   3. decode the terminator in C and take the NEXT PC the block resolves to:
 *        rts          -> POP the 68k return address off the real return stack (a7/SP);
 *                        when SP is back at the initial SP this is the TOP-LEVEL return
 *                        (program exit, exit_d0 = d0); otherwise resume at the popped PC.
 *        bsr.B/.W/.L  -> PUSH the 68k return address big-endian to (a7-=4); pc = target.
 *        jsr abs.l    -> like bsr to an absolute target.
 *        jsr (An)     -> computed: target from the register; PUSH return; pc = An.
 *        jmp abs.l    -> pc = absolute target (no push).
 *        jmp (An)     -> computed: pc = An (no push).
 *        bra.B/.W/.L  -> pc = target.
 *        bcc.B/.W/.L  -> read the REAL CCR the decoders produced; branch or fall.
 *        jsr d16(A6)  -> a LIBRARY VECTOR (negative-offset rule): call the [J3] bridge
 *                        (marshals 68k regs into the native stub); resume after the jsr.
 *                        This is a host-C call that returns immediately — NO stack push.
 *        lea abs.l,An -> set An = the relocated abs32 (an address compute, not memory).
 *   4. loop. A step cap bounds runaway; a corrupt return address that escapes the
 *      sandbox is caught (clean error, no host crash).
 *
 * THE REAL 68k RETURN STACK. a7 (== A0..A7's index 7, st->a[7]) is a SANDBOX address.
 * The dispatcher seeds it to the top of the sandbox at entry, records that as the initial
 * SP, and on a bsr/jsr-to-code PUSHES the 4-byte return address BIG-ENDIAN into the
 * sandbox at (a7-4) then sets a7-=4 (68k predecrement push semantics). rts reads the
 * longword big-endian at (a7), sets a7+=4, and jumps there. The pushed bytes live in the
 * sandbox the test inspects, so the return-stack contents are byte-exact-verifiable. The
 * push/pop happen in the dispatcher (C), not inside translated blocks, because they occur
 * at the terminator boundary the dispatcher already owns — exactly where Emu68's MainLoop
 * re-reads the PC. (A future cross-region-chaining engine would emit the push/pop inline.)
 *
 * The heavy SEMANTIC work (every register/ALU/flag/memory opcode) is the REAL Emu68
 * decoders'. The dispatcher only owns block boundaries, the branch decision (using the
 * REAL flags), the return stack, the computed jump targets, the (An) sandbox addressing
 * setup, the LEA address compute, and the LVO bridge. All control-flow terminators are
 * decoded straight from the instruction stream, not hand-constructed.
 * ===============================================================================
 */
#include "j5d_jit68k.h"
#include "j5s_fpu_exc.h"        /* [J5s] FP exception model: FPSR EXC/AEXC, FPCR enable/mode, vectors, BSUN */
#include "jit_region.h"
#include "emu68/A64.h"
#include "j3_jit68k.h"          /* j3_vector_recognise: the negative-offset LVO math */
#include "j5n_diag.h"           /* [J5n] the diagnostics funnel (NULL-gated side-channel) */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fenv.h>               /* [J5s] feclearexcept/fetestexcept/fesetround around FP blocks */
/* [J5s] the dispatcher reads the host FP exception flags + sets the rounding mode around each
 * native FP block run; tell the compiler not to reorder across the fenv calls. */
#pragma STDC FENV_ACCESS ON

/* [J5n] The DIAGNOSTICS funnel hook. EVERY fault path in this dispatcher routes through
 * here when a diag config is registered (j5d_set_diag); NULL (the whole existing corpus)
 * is the zero-overhead fast path. The macro snapshots the per-run instruction coordinate
 * into the config, then calls j5d_fault to write the bundle. It does NOT change control
 * flow — the existing raise_exception/RFAIL behavior is untouched; the funnel is additive.
 * `kind` is a j5n_fault_kind; `detail` a human one-liner; `st`/`sb` the faulting state. */
#define J5N_FUNNEL(kind, detail, st, sb) do {                                  \
        j5n_diag *_d = j5d_get_diag();                                         \
        if (_d) { _d->insn_number = g_insn_number;                            \
                  j5d_fault((kind), (detail), (st), (sb), NULL); }            \
    } while (0)

/* [J5n] When diag is active AND the exception that was just raised dispatched to an
 * UNINSTALLED vector (handler reads as 0 / out of sandbox = no real handler), the fault is
 * unrecoverable — exactly the silent crash the bundle exists to catch. Funnel it with the
 * proper kind AT ITS ORIGIN (before the [J5i] cascade to PC 0 masks it as a later bus error)
 * and stop the run. This is diag-only and does NOT change the [J5i] model: with no diag
 * registered (the whole corpus + the [J5i] test), this is a no-op and the existing
 * dispatch-to-handler behavior is byte-for-byte unchanged. `_h` is the handler PC. */
#define J5N_UNHANDLED(kind, detail, st, sb, _h) do {                           \
        j5n_diag *_d = j5d_get_diag();                                         \
        if (_d && ((_h) == 0u || (_h) < (sb)->origin ||                       \
                   (uint64_t)(_h) + 2 > (uint64_t)(sb)->origin + (sb)->size)) {\
            _d->insn_number = g_insn_number;                                   \
            j5d_fault((kind), (detail), (st), (sb), NULL);                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

/* [J5n] the deterministic GLOBAL 68k instruction number — bumped by each block's decoded
 * instruction count as the block executes. It is the crash coordinate #N. Kept OUTSIDE
 * struct j5d_m68k_state (the frozen seam); a private engine global. Reset per run. */
static uint64_t g_insn_number = 0;

/* [J5n] WEAK fallbacks for the diagnostics hooks the engine calls. The diagnostics subsystem
 * (j5n_diag.c) provides the STRONG definitions; when a [J5*] test links the engine WITHOUT
 * j5n_diag.c (the whole existing corpus + j1..j5m), these weak no-ops satisfy the linker and
 * j5d_get_diag() returns NULL — so EVERY diagnostics hook is dead and the engine behaves
 * byte-for-byte as before. This keeps the diagnostics genuinely OPTIONAL and the existing
 * targets unchanged (no new objects to link). */
__attribute__((weak)) j5n_diag *j5d_get_diag(void) { return NULL; }
__attribute__((weak)) const char *j5d_fault(j5n_fault_kind kind, const char *detail,
        const struct j5d_m68k_state *st, j5d_sandbox *sb, const j5n_hostregs *host)
{ (void)kind; (void)detail; (void)st; (void)sb; (void)host; return NULL; }
__attribute__((weak)) void j5n_signal_set_context(const struct j5d_m68k_state *st, j5d_sandbox *sb)
{ (void)st; (void)sb; }
__attribute__((weak)) void j5n_diag_record_block(j5n_diag *d, j5d_sandbox *sb, uint32_t pc, uint32_t end_pc)
{ (void)d; (void)sb; (void)pc; (void)end_pc; }

/* [J5n] the DIFFERENTIAL block-boundary callback (set by the lockstep diff driver). It is
 * called at each next-block-entry boundary with the flushed JIT state; returns nonzero to
 * STOP the run (divergence found + bundled, or a replay-to-N landing). NULL by default. */
typedef int (*j5d_boundary_cb)(void *user, j5d_sandbox *sb,
                               struct j5d_m68k_state *st, uint32_t next_pc);
static j5d_boundary_cb g_block_boundary_cb = NULL;
static void           *g_block_boundary_user = NULL;
uint64_t j5d_diag_insn_number(void) { return g_insn_number; }

/* ---- the REAL Emu68 line-decoder entry points (linked decoder objects) ---- */
extern uint32_t *EMIT_moveq(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_move (uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line0(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);  /* [J5g] */
extern uint32_t *EMIT_line4(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);  /* [J5g] */
extern uint32_t *EMIT_line5(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line8(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_line9(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineB(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineC(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineD(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);
extern uint32_t *EMIT_lineE(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);  /* [J5g] */
extern uint32_t *EMIT_lineF(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed);  /* [J5o] FPU */
/* [J5o] WEAK fallback for the line-F (FPU) decoder so the engine LINKS for the integer-only
 * spikes (J5d..J5n) that do NOT vendor M68k_LINEF.c. When the real verbatim M68k_LINEF.c IS
 * linked (the J5o target), its STRONG EMIT_lineF overrides this. Non-FPU programs never decode a
 * line-F opcode (the engine's pre-decode validation rejects any out-of-subset opcode before
 * translating), so this fallback is never reached there — it exists purely to satisfy the linker,
 * keeping the FPU strictly additive/opcode-gated (no Makefile change to the integer targets). */
__attribute__((weak))
uint32_t *EMIT_lineF(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
{
    (void)ptr; (void)m68k_ptr; (void)insn_consumed;
    /* Should be unreachable in an integer-only build. If a line-F opcode somehow reaches here,
     * leave insn_consumed at 0 so the engine's "decoder consumed 0 insns" guard fails loudly. */
    return ptr;
}

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
extern void      j5c_ra_get_fp_masks(uint16_t *live, uint16_t *dirty); /* [J5o] FP liveness */

/* The Emu68 m68k->ARM map for our prologue/epilogue (REG_* from A64.h). */
static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7 };
static const uint8_t reg_a[8] = { REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7 };

/* [J5o] FP0..FP7 map to AArch64 d8..d15 in the JIT'd block (Emu68's RA_MapFPURegister:
 * fp_reg -> d8+fp_reg). The engine seeds these from st->fp[] (FP regs the block reads) in the
 * prologue and stores back the ones it writes in the epilogue — the [J5e]-style FP liveness
 * frame, so a block touching no FPU emits ZERO FP load/store (the integer corpus is unchanged).
 * The fp[] / fpcr / fpsr byte offsets are taken with offsetof() so the struct's padding before
 * fp[] never needs hand-encoding; static-asserted against the J5O_OFF_* literals j5c_ra.c uses. */
#define J5O_FP0  8u                 /* d8 = FP0 ... d15 = FP7 */
#include <stddef.h>
_Static_assert(offsetof(struct j5d_m68k_state, fpcr) == 144u, "[J5o] fpcr offset drift");
_Static_assert(offsetof(struct j5d_m68k_state, fpsr) == 148u, "[J5o] fpsr offset drift");

#define J5D_BASEADJ_X  12u   /* x12 = sandbox base-adjust (host_mem - origin)        */

/* [J5k] chaining scratch host regs for the inline Bcc condition eval + the patch-slot
 * fallback. These are AArch64 caller-saved temporaries NOT in the 68k file map (D0..D7=
 * w19..w26, A0..A4=w13..w17, A5..A7=w27..w29, CCR scratch=w4..w11), so using them in the
 * tail cannot clobber any live 68k register. w0..w3 are free at the tail (the body's last
 * act, RA_FlushCC, leaves no live scratch; w0 is Emu68's 1-shot flag scratch, dead here). */
#define J5K_T0  2u
#define J5K_T1  3u
#define J5K_T2  6u
#define J5K_T3  7u
#define J5K_T4  8u
#define J5K_T5  9u
#define J5K_T6  10u  /* extra scratch: holds the CCR source for emit_cond_bool (must NOT
                      * alias the V/C/Z/N/T/ONE regs that emit_cond_bool overwrites) */

/* ================================ stats / trace ================================ */
static j5d_stats g_stats;

/* [J5k] total block-execution counter, bumped by an emitted ldr/add/str at EVERY block's
 * CHAIN ENTRY (reached by both the cold fall-through and a chained `b` from a predecessor).
 * So it counts total block runs; chain_branches_taken = this - dispatcher_roundtrips. Kept
 * OUTSIDE struct j5d_m68k_state (the frozen seam) — a private engine global addressed by an
 * immediate baked into the block. */
static volatile uint64_t g_block_exec_count;

/* [J5k] which cached block a chain actually ENDED at. When the C dispatcher calls block A
 * and A's tail is linked to B (and B's to C, ...), the JIT'd code runs the whole chain past
 * the dispatcher and only re-enters C at the first UNLINKED tail's epilogue. That terminal
 * block's epilogue writes its own cache index here (an immediate baked at translate time), so
 * the dispatcher decodes the TERMINAL block's terminator — not block A's. Without this the
 * dispatcher would mis-handle the wrong terminator (the head's, not the tail's). */
static volatile uint32_t g_chain_terminal_idx;

void j5d_get_stats(j5d_stats *out) { *out = g_stats; }

/* [J5k] fold the JIT-side exec counter into the public stats at run exit. blocks_executed is
 * the TOTAL block runs (the JIT-emitted chain-entry bump counts cold AND chained entries);
 * chain_branches_taken is the runs that arrived via a direct block->block branch (i.e. the
 * runs that did NOT cost a C round-trip). chain_spills_elided is the register-file memory ops
 * those chained hops removed: a chained hop A->B branches from A's tail straight into B's
 * chain entry, SKIPPING A's epilogue 16-register store AND B's outer-prologue 16-register
 * reload — 32 register memory ops per chained hop that never hit struct j5d_m68k_state. */
static void finalize_stats(void)
{
    g_stats.blocks_executed = (uint32_t)g_block_exec_count;
    g_stats.chain_branches_taken =
        (g_stats.blocks_executed > g_stats.dispatcher_roundtrips)
        ? (g_stats.blocks_executed - g_stats.dispatcher_roundtrips) : 0;
    g_stats.chain_spills_elided = g_stats.chain_branches_taken * 32u;
}

/* Emit `*(uint64_t*)addr += 1` using two scratch host regs (caller guarantees they are dead
 * here): materialize the 64-bit `addr` into `wa`'s X reg via movz/movk, ldr/add/str. */
static uint32_t *emit_counter_bump(uint32_t *p, uint64_t addr, uint8_t ra, uint8_t rv)
{
    *p++ = mov64_immed_u16(ra, (uint16_t)(addr        & 0xffff), 0);
    *p++ = movk64_immed_u16(ra, (uint16_t)((addr>>16) & 0xffff), 1);
    *p++ = movk64_immed_u16(ra, (uint16_t)((addr>>32) & 0xffff), 2);
    *p++ = movk64_immed_u16(ra, (uint16_t)((addr>>48) & 0xffff), 3);
    *p++ = ldr64_offset(ra, rv, 0);     /* rv = *addr        */
    *p++ = add64_immed(rv, rv, 1);      /* rv += 1           */
    *p++ = str64_offset(ra, rv, 0);     /* *addr = rv        */
    return p;
}

/* ============================ the [J5d] block ICache =========================== */
/* One MAP_JIT region per distinct entry PC. The compiled block is a C function
 * `void block(struct j5d_m68k_state *st, uint64_t base_adjust)` entered at its OUTER
 * entry (offset 0). [J5k] adds a second entry point — the CHAIN ENTRY — that a chaining
 * predecessor branches to directly (see translate_block for the layout). */
#define J5D_MAX_BLOCKS 256
#define J5K_MAX_LINKS  2     /* a block has at most two chainable tail targets (Bcc: 2)   */
typedef void (*j5d_block_fn)(struct j5d_m68k_state *st, uint64_t base_adjust);

/* [J5k] one chainable tail-branch slot inside a translated block. At translate time the
 * slot holds a fall-back sequence that returns to the C dispatcher (st->pc = target_pc);
 * once the target block is translated the slot's `b` word is BACKPATCHED to jump straight
 * into the target's chain entry. `taken` distinguishes the two Bcc outcomes for stats. */
typedef struct {
    uint32_t  target_pc;    /* the 68k PC this tail transfers to (statically known)        */
    uint32_t  slot_word;    /* word index into region.base of the BACKPATCHABLE `b` slot   */
    uint8_t   linked;       /* 1 once backpatched to the target's chain entry             */
} j5k_link;

typedef struct {
    uint32_t   pc;          /* entry PC of this block         */
    uint32_t   end_pc;      /* PC of the terminator opcode    */
    uint32_t   body_insns;  /* [J5n] decoded BODY instructions (excl. the terminator) — the
                             * per-block contribution to the deterministic global insn count */
    jit_region region;      /* the MAP_JIT code               */
    uint32_t   chain_off;   /* word offset of the CHAIN ENTRY (regs already live)         */
    uint16_t   dirty_mask;  /* [J5k] which 68k regs this block writes (for spill accounting)*/
    uint8_t    fpu_block;   /* [J5s] 1 if this block contains FP arithmetic (drives the    */
                            /*       per-block fenv exception model around the native run)  */
    uint8_t    fp_dirty;    /* [J5s] FP0..FP7 written by this block (for single-prec re-round)*/
    uint8_t    nlinks;      /* number of chainable tail slots (0,1,2)                      */
    j5k_link   link[J5K_MAX_LINKS];
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
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* Is `op` a terminator the DISPATCHER handles (not driven through a real decoder)?
 * [J5f] broadens this from the [J5d] flat-PC subset to the full control-flow set:
 * rts, bsr/bra/Bcc (all .B/.W/.L widths), jmp/jsr absolute + computed (An), the
 * library jsr d16(A6) vector, and lea abs.l,An.  [J5i] adds the EXCEPTION-causing
 * instructions + rte as dispatcher-decoded terminators (the 68k exception model is OURS,
 * in C — Emu68's bare-metal EMIT_Exception is a no-op stub in the hosted runtime). */
static int is_terminator(uint16_t op)
{
    if (op == 0x4E75u) return 1;                 /* rts                            */
    if (op == 0x4E73u) return 1;                 /* rte  ([J5i] return from exception) */
    if (op == 0x4E71u) return 1;                 /* nop (dispatcher steps over it) */
    if (op == 0x4EAEu) return 1;                 /* jsr d16(A6)  (library vector)  */
    if (op == 0x4EB9u) return 1;                 /* jsr abs.l                      */
    if (op == 0x4EF9u) return 1;                 /* jmp abs.l                      */
    if ((op & 0xFFF8u) == 0x4E90u) return 1;     /* jsr (An)   (computed)          */
    if ((op & 0xFFF8u) == 0x4ED0u) return 1;     /* jmp (An)   (computed)          */
    if ((op & 0xFFF0u) == 0x4E40u) return 1;     /* TRAP #n  ([J5i] vector 32+n)   */
    if (op == 0x4AFCu) return 1;                 /* ILLEGAL  ([J5i] vector 4)      */
    if ((op & 0xFFC0u) == 0x80C0u) return 1;     /* divu.w  ([J5i] vector 5 on /0) */
    if ((op & 0xFFC0u) == 0x81C0u) return 1;     /* divs.w  ([J5i] vector 5 on /0) */
    if ((op & 0xF000u) == 0x6000u) return 1;     /* Bcc/BRA/BSR, any .B/.W/.L width */
    if ((op & 0xF0F8u) == 0x50C8u) return 1;     /* [J5u] DBcc Dn,disp16 (decrement-and-branch loop) */
    if ((op & 0xF1FFu) == 0x41F9u) return 1;     /* lea abs.l,An (dispatcher-decoded)*/
    if ((op & 0xF1FFu) == 0x41FAu) return 1;     /* lea (d16,pc),An (dispatcher-decoded)*/
    /* [J5q] FP conditional control-flow — decoded at the DISPATCHER (like integer Bcc),
     * NOT through EMIT_lineF (Emu68's FBcc/FScc emit the bare-metal REG_PC funnel + the
     * 0xfffffffe sentinel; FDBcc/FTRAPcc have NO decoder body at all). The dispatcher reads
     * the FPSR cc the FP body produced and evaluates the FP predicate in C. Note the order:
     * FDBcc (mode 001) + FTRAPcc (mode 111) carve out of the FScc EA range. */
    if ((op & 0xFFF8u) == 0xF248u) return 1;     /* FDBcc (cpDBcc)                  */
    if ((op & 0xFFF8u) == 0xF278u) return 1;     /* FTRAPcc (cpTRAPcc -> vector 7)  */
    if ((op & 0xFFC0u) == 0xF240u) return 1;     /* FScc (set byte, Dn or memory)   */
    if ((op & 0xFF80u) == 0xF280u) return 1;     /* FBcc (.W/.L) incl FNOP (0xF280) */
    return 0;
}

/* [J5r] FMOVEM (FP register-list move) + FP system-register moves (FMOVE/FMOVEM to/from
 * FPCR/FPSR/FPIAR) are distinguished only by opcode2 (the command word), so they cannot be
 * recognised from `op` alone. Like FBcc/FScc, they are decoded at the DISPATCHER level in C
 * (the .x 96-bit conversion + the sandbox memory + the reglist are OURS; Emu68's verbatim
 * FMOVEM/FMOVE-special bodies are bare-metal — they `blr` the abort-stub Load96bit/Store96bit
 * with a 32-bit-truncated helper address and manipulate the real FPCR via `msr fpcr`). When the
 * body loop sees a line-F op whose opcode2 marks one of these, it ends the block so the
 * dispatcher handles it. Returns 1 for: FMOVEM (opcode2 & 0xC700 == 0xC000), and FMOVE/FMOVEM
 * to/from a control register (opcode2 bits 15-13 == 100 or 101 — the to-/from-special forms,
 * which also cover the multi-control-reg FMOVEM.l list). */
static int is_fp_mem_terminator(uint16_t op, uint16_t opcode2)
{
    if ((op & 0xFFC0u) != 0xF200u) return 0;          /* must be the FP EA-form line-F op */
    if ((opcode2 & 0xC700u) == 0xC000u) return 1;     /* FMOVEM (.x register list)       */
    if ((opcode2 & 0xE000u) == 0x8000u) return 1;     /* FMOVE/FMOVEM to a control reg   */
    if ((opcode2 & 0xE000u) == 0xA000u) return 1;     /* FMOVE/FMOVEM from a control reg */
    return 0;
}

/* [J5t] An FP arithmetic op (FADD/FSUB/FMUL/FDIV/FCMP/FMOVE/... <ea>,FPn) whose SOURCE is an
 * INLINE IMMEDIATE (ea = mode 111:100 = 0x3c, opcode2 R/M bit = 1).  vbcc -fpu=68881 emits these
 * heavily (fmove.d #$imm,fp ; fadd.d #$imm,fp ; fcmp.d #$imm,fp ; fmul.d #$imm,fp ; ...) but the
 * hand-written FP corpus ([J5o]-[J5s]) never did — it loaded constants from a DATA section via a
 * (d16,An)/(An) EA.  Emu68's verbatim EMIT_lineF lowers the .d/.s immediate to `fldd/flds(*reg,
 * REG_PC, off)` — a PC-RELATIVE load that reads the immediate out of the live instruction stream
 * via REG_PC.  On the bare metal REG_PC is a host pointer into mapped code; in our re-hosted JIT
 * REG_PC (x18) is a CACHED 68k PC VALUE in sandbox space, so that PC-relative fldd dereferences a
 * bogus host address -> SIGSEGV.  (The --fpu-sandbox darwinize pass deliberately leaves the
 * REG_PC/const immediate paths ALONE — it only rewrites the EA-base sites.)  So we DECODE this one
 * op at the dispatcher boundary and execute it in C (j5d_fp_imm_arith), exactly as the oracle does
 * for an immediate source — the immediate bytes are read from the SANDBOX at the correct 68k PC.
 * The .l/.w/.b integer immediates are NOT affected (Emu68 lowers those to movw/mov_immed, no PC
 * load), so only the FP-format (.s/.d) immediate is the gap; we route ALL immediate-source FP
 * arith to C uniformly (the C path handles every format identically to the oracle). */
static int is_fp_imm_arith(uint16_t op, uint16_t opcode2)
{
    if ((op & 0xFFC0u) != 0xF200u) return 0;          /* FP EA-form line-F op                  */
    if ((op & 0x3Fu) != 0x3Cu) return 0;              /* EA = mode 111:100 = #immediate        */
    if (((opcode2 >> 15) & 1u) != 0u) return 0;       /* opcode2 bit15==0 : the EA-arith family */
    if (((opcode2 >> 14) & 1u) != 1u) return 0;       /* R/M==1 : source is the EA (the imm)    */
    /* exclude the FMOVE-to-mem direction (a #imm dest is illegal anyway) — defensive. */
    if ((opcode2 & 0xE000u) == 0x6000u) return 0;
    return 1;
}

/* ============================== [J5i] THE SR / EXCEPTION MODEL =====================
 * Dispatcher-level (C), the spec's "68k exceptions handled in C" (no host VBAR; Emu68's
 * bare-metal EMIT_Exception/VBR path is a no-op stub in our re-hosted runtime). The
 * dispatcher already owns the inter-block PC funnel + the return stack + the LVO bridge;
 * the exception dispatch is one more thing decided at the same terminator boundary. */

/* Pack the architectural 68k SR from the internal state. The CCR byte stored in the state
 * uses Emu68's INTERNAL bit layout (V=0,C=1,Z=2,N=3,X=4 — see j5d_jit68k.h); the real 68k
 * CCR low byte is C=0,V=1,Z=2,N=3,X=4. We re-order C and V so the SR PUSHED IN THE FRAME is
 * a genuine 68k SR (the program's rte / a real handler would read standard bits). */
uint16_t j5d_pack_sr(const struct j5d_m68k_state *st)
{
    uint32_t i = st->ccr;
    uint16_t ccr68 = 0;
    if (i & J5D_CCR_C) ccr68 |= 0x01u;   /* C: internal bit1 -> 68k bit0 */
    if (i & J5D_CCR_V) ccr68 |= 0x02u;   /* V: internal bit0 -> 68k bit1 */
    if (i & J5D_CCR_Z) ccr68 |= 0x04u;   /* Z */
    if (i & J5D_CCR_N) ccr68 |= 0x08u;   /* N */
    if (i & J5D_CCR_X) ccr68 |= 0x10u;   /* X */
    return (uint16_t)((st->sr_high << 8) | ccr68);
}

/* The reverse: write the CCR low byte (Emu68-internal layout) + sr_high back from a 68k SR
 * (used by rte to restore the condition codes + the system byte the frame saved). */
static void j5d_unpack_sr(struct j5d_m68k_state *st, uint16_t sr)
{
    uint32_t i = 0;
    if (sr & 0x01u) i |= J5D_CCR_C;
    if (sr & 0x02u) i |= J5D_CCR_V;
    if (sr & 0x04u) i |= J5D_CCR_Z;
    if (sr & 0x08u) i |= J5D_CCR_N;
    if (sr & 0x10u) i |= J5D_CCR_X;
    st->ccr     = i;
    st->sr_high = (uint16_t)((sr >> 8) & 0xFFu);
}

/* The per-run exception log (the test's bookkeeping; NULL when a program raises none). */
static j5i_exc_log *g_exc_log = NULL;
void j5d_set_exc_log(j5i_exc_log *log) { g_exc_log = log; }

/* Read the handler address for vector `vnum` from the sandbox vector table (big-endian
 * longword at J5I_VBR + vnum*4). Returns 1 (error) if the table slot is out of sandbox. */
static int read_vector(j5d_sandbox *sb, unsigned vnum, uint32_t *handler)
{
    uint32_t va = J5I_VBR + vnum * 4u;
    if (va < sb->origin || (uint64_t)va + 4 > (uint64_t)sb->origin + sb->size) return 1;
    *handler = be32(sb->host_mem + (va - sb->origin));
    return 0;
}

/* Raise a 68k exception: build the standard short frame on the supervisor stack (a7),
 * set S, and RETURN the handler PC the dispatcher must jump to. `return_pc` is the PC saved
 * in the frame (for a TRAP/illegal: the instruction AFTER; for div0/bus: the faulting one,
 * per the PRM group conventions we model). On any error (bad vector slot / a7 escapes the
 * sandbox) returns 1 with errbuf set. */
static int raise_exception(j5d_sandbox *sb, struct j5d_m68k_state *st, unsigned vnum,
                           uint32_t return_pc, uint32_t *handler_out,
                           char *errbuf, unsigned errlen)
{
    uint32_t handler;
    if (read_vector(sb, vnum, &handler)) {
        if (errbuf) snprintf(errbuf, errlen, "exception: vector %u table slot out of sandbox", vnum);
        return 1;
    }
    uint16_t sr = j5d_pack_sr(st);            /* the SR to push (BEFORE setting S)         */

    /* Push the 6-byte frame: predecrement a7 by 6, write SR (16-bit BE) @ a7, PC (32-bit
     * BE) @ a7+2. (M68000 short frame: SR then PC, both big-endian.) */
    uint32_t a7 = st->a[7] - 6u;
    if (a7 < sb->origin || (uint64_t)a7 + 6 > (uint64_t)sb->origin + sb->size) {
        if (errbuf) snprintf(errbuf, errlen, "exception: frame push a7=%08x out of sandbox", a7);
        return 1;
    }
    uint8_t *p = sb->host_mem + (a7 - sb->origin);
    p[0] = (uint8_t)(sr >> 8);  p[1] = (uint8_t)sr;                          /* SR  @ a7    */
    p[2] = (uint8_t)(return_pc >> 24); p[3] = (uint8_t)(return_pc >> 16);
    p[4] = (uint8_t)(return_pc >> 8);  p[5] = (uint8_t)return_pc;            /* PC  @ a7+2  */
    st->a[7] = a7;

    st->sr_high |= (J5D_SR_S >> 8);           /* enter supervisor state (set S)            */
    st->exc_count++;

    if (g_exc_log && g_exc_log->n < J5I_MAX_EXC) {
        j5i_exc_record *r = &g_exc_log->rec[g_exc_log->n++];
        r->vector = (uint8_t)vnum; r->frame_sr = sr; r->frame_pc = return_pc;
        r->a7_at_entry = a7; r->handler_pc = handler;
    }
    g_stats.exceptions_dispatched++;
    *handler_out = handler;
    return 0;
}

/* [J5f] the AArch64 condition each 68k Bcc tests, evaluated against the 68k CCR the REAL
 * decoders produced (J5D_CCR_* bit layout). Returns 1 = branch taken, 0 = fall through.
 * `cc4` is the 4-bit condition field (op>>8 & 0xF); 0/1 (BRA/BSR) are handled by the
 * caller. Grounded on the M68000 PRM condition-code table. */
static int bcc_taken(unsigned cc4, uint32_t ccr)
{
    int N = (ccr & J5D_CCR_N) != 0, Z = (ccr & J5D_CCR_Z) != 0;
    int V = (ccr & J5D_CCR_V) != 0, C = (ccr & J5D_CCR_C) != 0;
    switch (cc4) {
        case 0x2: return !C && !Z;          /* HI  */
        case 0x3: return  C ||  Z;          /* LS  */
        case 0x4: return !C;                /* CC/HS */
        case 0x5: return  C;                /* CS/LO */
        case 0x6: return !Z;                /* NE  */
        case 0x7: return  Z;                /* EQ  */
        case 0x8: return !V;                /* VC  */
        case 0x9: return  V;                /* VS  */
        case 0xA: return !N;                /* PL  */
        case 0xB: return  N;                /* MI  */
        case 0xC: return  N == V;           /* GE  */
        case 0xD: return  N != V;           /* LT  */
        case 0xE: return !Z && (N == V);    /* GT  */
        case 0xF: return  Z || (N != V);    /* LE  */
        default:  return 0;
    }
}

/* ===================== [J5r] FMOVEM + FP SYSTEM-REGISTER MOVES (OURS, dispatcher) =========
 * The .x 96-bit conversion (j5r_double_to_x / j5r_x_to_double, j5d_jit68k.h) + the sandbox
 * memory + the reglist semantics, in C. The FP register file is canonical in st->fp[] at the
 * block boundary (the epilogue flushed dirty d8..d15); FPCR/FPSR/FPIAR are memory-backed. */

/* Read/write a 12-byte extended (.x) slot at sandbox address `addr` (bounds-checked). */
static int x_mem_check(j5d_sandbox *sb, uint32_t addr, char *e, unsigned el)
{
    if (addr < sb->origin || (uint64_t)addr + 12 > (uint64_t)sb->origin + sb->size) {
        if (e) snprintf(e, el, "FMOVEM: .x slot %08x out of sandbox", addr);
        return 1;
    }
    return 0;
}

/* Resolve the FMOVEM / FP-sys-reg destination EA to a BASE 68k address + the post-instruction
 * PC. Handles the control + predec/postinc modes a real FP prologue/epilogue uses; the An update
 * for -(An)/(An)+ is applied by the CALLER (it needs the total byte count). `total` = bytes the
 * whole transfer occupies (12*count for .x, 4*count for control regs). Returns the BASE address
 * the first element uses (for -(An): An-total; for (An)+ and the rest: the current An / EA). */
static int fmovem_resolve_ea(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t tpc,
                             uint16_t op, uint32_t total, unsigned *ext_bytes,
                             uint32_t *base_out, int *predec, int *postinc,
                             char *e, unsigned el)
{
    unsigned mode = (op >> 3) & 7u, reg = op & 7u;
    *predec = 0; *postinc = 0; *ext_bytes = 0;
    const uint8_t *thost = sb->host_mem + (tpc - sb->origin);
    switch (mode) {
        case 2: *base_out = st->a[reg]; return 0;                          /* (An)        */
        case 3: *base_out = st->a[reg]; *postinc = 1; return 0;            /* (An)+       */
        case 4: *base_out = st->a[reg] - total; *predec = 1; return 0;     /* -(An)       */
        case 5: { int16_t d16 = be16s(thost + 4); *ext_bytes = 2;          /* (d16,An)    */
                  *base_out = st->a[reg] + (uint32_t)(int32_t)d16; return 0; }
        case 7:
            if (reg == 0) { int16_t d16 = be16s(thost + 4); *ext_bytes = 2;/* abs.w       */
                            *base_out = (uint32_t)(int32_t)d16; return 0; }
            if (reg == 1) { *base_out = be32(thost + 4); *ext_bytes = 4; return 0; } /* abs.l */
            if (reg == 2) { int16_t d16 = be16s(thost + 4); *ext_bytes = 2;/* (d16,PC)    */
                            *base_out = (tpc + 4) + (uint32_t)(int32_t)d16; return 0; }
            break;
    }
    if (e) snprintf(e, el, "FMOVEM/FP-sys EA mode %u not in the [J5r] subset", mode);
    return 1;
}

/* FMOVEM .x — move an FP register list to/from memory in 96-bit extended format. */
static int j5d_fmovem_x(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t tpc,
                        uint16_t op, uint16_t opcode2, uint32_t *after, char *e, unsigned el)
{
    unsigned dir     = (opcode2 >> 13) & 1u;   /* 0 = mem->FP, 1 = FP->mem                  */
    unsigned dynamic = (opcode2 >> 11) & 1u;   /* 0 = static list, 1 = register-specified   */
    unsigned mode    = (op >> 3) & 7u, reg = op & 7u;
    uint8_t  mask;
    if (dynamic) mask = (uint8_t)(st->d[(opcode2 >> 4) & 7u] & 0xFFu);  /* dynamic: Dn low byte */
    else         mask = (uint8_t)(opcode2 & 0xFFu);

    /* Map the 8 mask bits to FP register numbers in ascending memory order. Predecrement (-(An))
     * uses mask bit i = FPi; every other mode uses mask bit i = FP(7-i). In BOTH cases the
     * lowest-numbered selected FP register lands at the LOWEST address (the [J5l]-style order),
     * so a -(An) save round-trips a (An)+ restore. */
    int is_predec = (mode == 4);
    unsigned fpregs[8]; int count = 0;
    for (int fp = 0; fp < 8; fp++) {
        int bit = is_predec ? fp : (7 - fp);          /* the mask bit testing this FP reg */
        if (mask & (1u << bit)) fpregs[count++] = (unsigned)fp;
    }
    uint32_t total = (uint32_t)(12 * count);

    int predec, postinc; unsigned ext_bytes; uint32_t base;
    if (fmovem_resolve_ea(sb, st, tpc, op, total, &ext_bytes, &base, &predec, &postinc, e, el))
        return 1;
    (void)mode; (void)reg;

    for (int s = 0; s < count; s++) {
        uint32_t addr = base + (uint32_t)(12 * s);
        if (x_mem_check(sb, addr, e, el)) return 1;
        uint8_t *p = sb->host_mem + (addr - sb->origin);
        if (dir) {                                    /* FP -> mem (.x store)             */
            uint8_t xb[12]; j5r_double_to_x(st->fp[fpregs[s]], xb);
            memcpy(p, xb, 12);
        } else {                                      /* mem -> FP (.x load)             */
            st->fp[fpregs[s]] = j5r_x_to_double(p);
        }
        g_stats.mem_accesses++;
    }
    if (predec)  st->a[reg] -= total;                 /* -(An): commit the predecrement   */
    if (postinc) st->a[reg] += total;                 /* (An)+: commit the postincrement  */

    *after = tpc + 4 + ext_bytes;                     /* opcode + command word + EA ext   */
    return 0;
}

/* FMOVE / FMOVEM to/from FPCR/FPSR/FPIAR (the control-register move forms). The control regs are
 * each a 32-bit longword in memory; multiple regs in one instruction are stored FPCR, FPSR, FPIAR
 * (low->high address). The Dn/An-direct single-register forms are also handled. */
static int j5d_fmove_sysreg(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t tpc,
                            uint16_t op, uint16_t opcode2, uint32_t *after, char *e, unsigned el)
{
    unsigned to_special = ((opcode2 & 0xE000u) == 0x8000u);  /* 100 = Dn/mem -> ctrl reg  */
    unsigned mode = (op >> 3) & 7u, reg = op & 7u;
    /* control-register select bits: FPCR=bit12, FPSR=bit11, FPIAR=bit10 (stored low->high). */
    int want_fpcr = (opcode2 & 0x1000u) != 0;
    int want_fpsr = (opcode2 & 0x0800u) != 0;
    int want_fpiar= (opcode2 & 0x0400u) != 0;
    int count = want_fpcr + want_fpsr + want_fpiar;

    /* Dn-direct (mode 0) / An-direct (mode 1) single-register forms. */
    if (mode == 0 || mode == 1) {
        uint32_t *slot = (mode == 0) ? &st->d[reg] : &st->a[reg];
        uint32_t *creg = want_fpcr ? &st->fpcr : want_fpsr ? &st->fpsr : &st->fpiar;
        if (count != 1) { if (e) snprintf(e, el, "FMOVE Dn/An,<ctrl>: expected one ctrl reg"); return 1; }
        if (to_special) *creg = *slot; else *slot = *creg;
        *after = tpc + 4;
        return 0;
    }

    /* memory forms: resolve the EA base + ext words; control regs are 4 bytes each, big-endian. */
    uint32_t total = (uint32_t)(4 * count);
    int predec, postinc; unsigned ext_bytes; uint32_t base;
    if (fmovem_resolve_ea(sb, st, tpc, op, total, &ext_bytes, &base, &predec, &postinc, e, el))
        return 1;

    uint32_t off = 0;
    /* the canonical order in memory is FPCR, then FPSR, then FPIAR (ascending address). */
    uint32_t *order[3]; int sel[3], n = 0;
    if (want_fpcr)  { order[n] = &st->fpcr;  sel[n] = 1; n++; }
    if (want_fpsr)  { order[n] = &st->fpsr;  sel[n] = 1; n++; }
    if (want_fpiar) { order[n] = &st->fpiar; sel[n] = 1; n++; }
    for (int i = 0; i < n; i++) {
        uint32_t addr = base + off;
        if (addr < sb->origin || (uint64_t)addr + 4 > (uint64_t)sb->origin + sb->size) {
            if (e) snprintf(e, el, "FMOVE <ctrl> mem %08x out of sandbox", addr); return 1;
        }
        uint8_t *p = sb->host_mem + (addr - sb->origin);
        if (to_special) {                            /* mem -> ctrl reg (load)           */
            *order[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        } else {                                     /* ctrl reg -> mem (store)          */
            uint32_t v = *order[i];
            p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
            p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        }
        (void)sel;
        g_stats.mem_accesses++;
        off += 4;
    }
    if (predec)  st->a[reg] -= total;
    if (postinc) st->a[reg] += total;
    *after = tpc + 4 + ext_bytes;
    return 0;
}

/* ===================== [J5s] THE FP EXCEPTION MODEL (engine / dispatcher side) =========
 * The JIT runs the FP arithmetic NATIVELY (Emu68's EMIT_lineF emits AArch64 fadd/fdiv/... inside
 * the block) — that path produces the FP register + memory RESULTS, verified bit-exact since
 * [J5o]. The FPSR EXCEPTION (EXC) + ACCRUED (AEXC) bytes, though, are PER-INSTRUCTION state: the
 * EXC byte reflects ONLY THE LAST FP instruction (re-set each op), while AEXC is sticky. The
 * block-level host fenv accumulates ALL ops in the block, so it cannot give the last-op EXC byte.
 * So the FPSR EXC/AEXC are derived by a per-op C RE-WALK (j5s_fp_exc_walk): for each FP arithmetic
 * op in [start,end) the walk recomputes it in C double from an EVOLVING LOCAL FP file (seeded by
 * the pre-block snapshot — so a dependent op sees the prior op's result), under PER-OP host fenv
 * (clear/test), mapping to the EXC byte (last op wins) + AEXC (OR-accumulated) + the SNAN-vs-OPERR
 * split (sNaN source operand) — EXACTLY the oracle's per-op model, so the asserted FPSR is bit-
 * exact. The cc byte (bits 27..24) stays from the native EMIT_GetFPUFlags path; EXC (15..8) + AEXC
 * (7..0) are disjoint and merged in here. The rounding MODE is applied to the walk (fesetround)
 * just as to the native block, and PRECISION single re-rounds through float (j5s_round_prec). */

/* Recompute one FP arithmetic op in C double from the evolving local FP file `fp` (8 doubles),
 * under per-op host fenv, and return its EXC bits (incl. the SNAN/OPERR split). Advances the
 * local FP file for a dst-writing op. `oper` is the FPU opmode; the op is the SAME table the
 * oracle's switch uses. Non-FP-arith opers (FMOVE-to-mem, sys-reg, etc.) are filtered by the
 * caller. Returns the EXC bits for THIS op. */
static uint32_t j5s_one_op_exc(double fp[8], unsigned oper, unsigned fpn, double src, uint32_t fpcr)
{
    double dst = fp[fpn & 7];
    int had_snan = j5s_is_snan(src) ||
                   (oper != 0x00u && oper != 0x3au && j5s_is_snan(dst));
    feclearexcept(FE_ALL_EXCEPT);
    double res = dst; int writes = 1;
    switch (oper) {
        case 0x00: res = src; writes = 1; break;                 /* FMOVE (no signal: see below) */
        case 0x18: res = fabs(src); break;  case 0x1a: res = -src; break;
        case 0x04: res = sqrt(src); break;
        case 0x20: res = dst / src; break;  case 0x22: res = dst + src; break;
        case 0x23: res = dst * src; break;  case 0x28: res = dst - src; break;
        case 0x38: writes = 0; break;                            /* FCMP                         */
        case 0x3a: writes = 0; break;                            /* FTST                         */
        case 0x0e: res = sin(src); break;   case 0x1d: res = cos(src); break;
        case 0x0f: res = tan(src); break;   case 0x0c: res = asin(src); break;
        case 0x1c: res = acos(src); break;  case 0x0a: res = atan(src); break;
        case 0x02: res = sinh(src); break;  case 0x19: res = cosh(src); break;
        case 0x09: res = tanh(src); break;  case 0x0d: res = atanh(src); break;
        case 0x10: res = exp(src); break;   case 0x08: res = expm1(src); break;
        case 0x11: res = exp2(src); break;  case 0x12: res = pow(10.0, src); break;
        case 0x14: res = log(src); break;   case 0x06: res = log1p(src); break;
        case 0x15: res = log10(src); break; case 0x16: res = log2(src); break;
        case 0x01: res = rint(src); break;  case 0x03: res = trunc(src); break;
        case 0x21: { double q = trunc(dst / src); res = dst - src * q; break; } /* FMOD          */
        case 0x25: res = remainder(dst, src); break;             /* FREM                         */
        case 0x26: res = ldexp(dst, (int)src); break;            /* FSCALE                       */
        case 0x1e: { uint64_t b; memcpy(&b,&src,8); res = (double)((int)((b>>52)&0x7ff)-1023); break; } /* FGETEXP */
        case 0x1f: { uint64_t b; memcpy(&b,&src,8); b=(b&~(0x7ffull<<52))|(0x3ffull<<52); memcpy(&res,&b,8); break; } /* FGETMAN */
        default: writes = 0; break;                              /* moves/format-only: no signal */
    }
    int fe = fetestexcept(FE_ALL_EXCEPT);
    /* FMOVE (and the non-arith moves) do NOT signal on AArch64 (a plain move), so they never
     * set the EXC byte — match the host: clear fe for a pure move. */
    if (oper == 0x00u) fe = 0;
    if (writes) { res = j5s_round_prec(res, fpcr); fp[fpn & 7] = res; }
    return j5s_fenv_to_exc(fe, had_snan);
}

/* Per-op FP exception RE-WALK over [start_pc,end_pc). Seeds a local FP file from `fp_pre`, decodes
 * each FP arithmetic op (resolving its source operand from registers / sandbox / immediate, exactly
 * as the oracle does), and computes the per-op EXC. Returns the LAST op's EXC byte in *exc_out
 * (the byte the FPSR shows) and the OR of all ops' AEXC contributions in *aexc_out. The rounding
 * direction must already be set (fesetround) by the caller. */
static void j5s_fp_exc_walk(j5d_sandbox *sb, struct j5d_m68k_state *st, const double fp_pre[8],
                            uint32_t start_pc, uint32_t end_pc, uint32_t *exc_out, uint32_t *aexc_out)
{
    double fp[8]; memcpy(fp, fp_pre, sizeof fp);
    uint32_t last_exc = 0, aexc = 0;
    uint32_t pc = start_pc; int guard = 0;
    while (pc < end_pc) {
        if (++guard > 512) break;
        if ((uint64_t)pc + 4 > (uint64_t)sb->origin + sb->size) break;
        const uint8_t *p = sb->host_mem + (pc - sb->origin);
        uint16_t op = be16(p);
        if ((op & 0xff80u) != 0xf200u || (op & 0x0e00u) != 0x0200u) { pc += 2; continue; }
        uint16_t opcode2 = be16(p + 2);
        unsigned ea  = op & 0x3fu;
        unsigned rm  = (opcode2 >> 14) & 1u;
        unsigned srcspec = (opcode2 >> 10) & 7u;
        unsigned fpn = (opcode2 >> 7) & 7u;
        unsigned oper = opcode2 & 0x7fu;
        unsigned is_to_mem = ((opcode2 & 0xe000u) == 0x6000u);
        uint32_t ext = pc + 4;
        if (is_to_mem) { pc = ext; continue; }   /* FMOVE FPn->mem: a CONSUMER, no EXC of its own */

        double src = 0.0;
        if (rm == 0) { src = fp[srcspec & 7]; }   /* register-direct FP source */
        else {
            unsigned mode = (ea >> 3) & 7u, reg = ea & 7u;
            int fsz = (srcspec==0)?4 : (srcspec==1)?4 : (srcspec==4)?2 : (srcspec==5)?8 : (srcspec==6)?1 : 0;
            uint32_t addr = 0; int rd = 0, imm = 0;
            if (mode == 0) {                       /* Dn-direct */
                uint32_t dv = st->d[reg];
                if (srcspec == 0)      src = (double)(int32_t)dv;
                else if (srcspec == 4) src = (double)(int32_t)(int16_t)(dv & 0xffff);
                else if (srcspec == 6) src = (double)(int32_t)(int8_t)(dv & 0xff);
                else if (srcspec == 1) { float f; memcpy(&f,&dv,4); src=(double)f; }
            } else if (mode == 2 || mode == 3) { addr = st->a[reg]; rd = 1; }
            else if (mode == 5) { addr = st->a[reg] + (uint32_t)(int32_t)(int16_t)be16(sb->host_mem+(ext-sb->origin)); ext += 2; rd = 1; }
            else if (mode == 4) { addr = st->a[reg] - (uint32_t)fsz; rd = 1; }   /* -(An) base */
            else if (mode == 7 && reg == 4) imm = 1;
            if (imm) {
                if (srcspec == 0) { src = (double)(int32_t)be32(sb->host_mem+(ext-sb->origin)); ext += 4; }
                else if (srcspec == 1) { uint32_t b=be32(sb->host_mem+(ext-sb->origin)); float f; memcpy(&f,&b,4); src=(double)f; ext += 4; }
                else if (srcspec == 4) { src = (double)(int32_t)(int16_t)be16(sb->host_mem+(ext-sb->origin)); ext += 2; }
                else if (srcspec == 5) { uint64_t b=0; for(int i=0;i<8;i++) b=(b<<8)|sb->host_mem[(ext+i)-sb->origin]; memcpy(&src,&b,8); ext += 8; }
                else if (srcspec == 6) { src = (double)(int32_t)(int8_t)(be16(sb->host_mem+(ext-sb->origin))&0xff); ext += 2; }
            } else if (rd && (uint64_t)addr + (uint64_t)(fsz?fsz:4) <= (uint64_t)sb->origin + sb->size) {
                if (srcspec == 0)      src = (double)(int32_t)be32(sb->host_mem+(addr-sb->origin));
                else if (srcspec == 1) { uint32_t b=be32(sb->host_mem+(addr-sb->origin)); float f; memcpy(&f,&b,4); src=(double)f; }
                else if (srcspec == 4) src = (double)(int32_t)(int16_t)be16(sb->host_mem+(addr-sb->origin));
                else if (srcspec == 5) { uint64_t b=0; for(int i=0;i<8;i++) b=(b<<8)|sb->host_mem[(addr+i)-sb->origin]; memcpy(&src,&b,8); }
                else if (srcspec == 6) src = (double)(int32_t)(int8_t)(be16(sb->host_mem+(addr-sb->origin))&0xff);
            }
        }
        uint32_t exc = j5s_one_op_exc(fp, oper, fpn, src, st->fpcr);
        last_exc = exc;
        aexc |= j5s_exc_to_aexc(exc);
        pc = ext;
    }
    *exc_out = last_exc;
    *aexc_out = aexc;
}

/* Merge the per-op FP exception state into st->fpsr (EXC byte = the last op's; AEXC OR-accumulated;
 * the cc byte from the native path is preserved) and TAKE an FP trap if an enabled exception fired.
 * `exc`/`aexc` come from j5s_fp_exc_walk; `return_pc` is the PC after the FP block (saved in a trap
 * frame). On a trap: *trap_pc=handler, return 1; on a raise failure: *trap_pc=0xFFFFFFFF, return 1.
 * 0 = no trap. */
static int j5s_fp_exc_apply(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t exc, uint32_t aexc,
                            uint32_t return_pc, uint32_t *trap_pc, char *errbuf, unsigned errlen)
{
    st->fpsr = (st->fpsr & ~J5S_FPSR_EXC_MASK) | exc;
    st->fpsr |= aexc;
    uint32_t enabled = exc & (st->fpcr & J5S_FPCR_ENABLE_MASK);
    unsigned vec;
    if (!j5s_exc_vector(enabled, &vec)) return 0;
    uint32_t handler;
    if (raise_exception(sb, st, vec, return_pc, &handler, errbuf, errlen)) { *trap_pc = 0xFFFFFFFFu; return 1; }
    *trap_pc = handler;
    return 1;
}

/* ===================== [J5t] IMMEDIATE-SOURCE FP ARITHMETIC (dispatcher / C) ==============
 * Execute ONE FP arithmetic op whose source is an inline immediate (fadd.d #imm,fp / fcmp.d
 * #imm,fp / fmove.d #imm,fp / ...), decoded at the dispatcher boundary because Emu68's native
 * lowering uses a PC-relative fldd that faults in our re-host (see is_fp_imm_arith).  The semantics
 * MIRROR the oracle's immediate-source path (j5d_interp.c) EXACTLY so the JIT and oracle stay
 * byte-exact: same arithmetic (j5s_one_op_exc's table, used here), same FPSR cc-update gate
 * (fp_imm_update_needed == the oracle's fpu_fpsr_update_needed), same cc packing (fp_pack_cc ==
 * the oracle's fpu_fcmp_fpsr), same EXC/AEXC + the same FP trap.  *after = the PC past this
 * instruction (opcode + opcode2 + the immediate words). */

/* Whether a later FP instruction CONSUMES the FPSR cc within 16 ops (so it must be written now).
 * A FAITHFUL re-derivation of the oracle's fpu_fpsr_update_needed (j5d_interp.c) — same scan, same
 * consumer set — so the JIT writes the cc byte at EXACTLY the same instructions the oracle does. */
static int fp_imm_update_needed(j5d_sandbox *sb, uint32_t after_pc)
{
    uint32_t pc = after_pc; int cnt = 0;
    for (;;) {
        if (pc < sb->origin || (uint64_t)pc + 2 > (uint64_t)sb->origin + sb->size) return 1;
        uint16_t op = be16(sb->host_mem + (pc - sb->origin));
        if ((op & 0xfe00u) == 0xf200u) break;                     /* reached an FPU (line-F) op  */
        if (cnt++ > 15) return 1;
        if ((op & 0xf000u) == 0x6000u) return 1;                  /* Bcc/BRA/BSR                 */
        if ((op & 0xff80u) == 0x4e80u) return 1;                  /* JSR/JMP                     */
        if (op == 0x4e75u || op == 0x4e73u || op == 0x4e77u) return 1; /* RTS/RTE/RTR            */
        if ((op & 0xfff0u) == 0x4e40u) return 1;                  /* TRAP #n                     */
        if ((op & 0xf0f8u) == 0x50c8u) return 1;                  /* DBcc                        */
        pc += 2;
    }
    uint16_t opcode  = be16(sb->host_mem + (pc - sb->origin));
    uint16_t opcode2 = ((uint64_t)pc + 4 <= (uint64_t)sb->origin + sb->size)
                       ? be16(sb->host_mem + (pc + 2 - sb->origin)) : 0;
    if ((opcode & 0xff80u) == 0xf280u) return 1;                            /* FBcc          */
    if ((opcode & 0xfff8u) == 0xf248u && (opcode2 & 0xffc0u) == 0) return 1;/* FDBcc         */
    if ((opcode & 0xffc0u) == 0xf200u && (opcode2 & 0xc700u) == 0xc000u) return 1; /* FMOVEM */
    if ((opcode & 0xffc0u) == 0xf200u && (opcode2 & 0xc3ffu) == 0x8000u) return 1; /* FMOVEMsp*/
    if ((opcode & 0xffc0u) == 0xf240u && (opcode2 & 0xffc0u) == 0) return 1;/* FScc          */
    if ((opcode & 0xfff8u) == 0xf278u && (opcode2 & 0xffc0u) == 0) return 1;/* FTRAPcc       */
    if ((opcode & 0xffc0u) == 0xf200u && (opcode2 & 0xe000u) == 0x6000u) return 1; /* FMOVE->MEM */
    if ((opcode & 0xffc0u) == 0xf340u) return 1;                            /* FRESTORE      */
    return 0;
}

/* Pack the FPSR cc nibble (N/Z/I/NAN, bits 27..24) for `a <op> b` — the SAME mapping the oracle's
 * fpu_fcmp_fpsr uses (EMIT_GetFPUFlags: I always 0 since the AArch64 C bit is bic'd out). */
static uint32_t fp_pack_cc(uint32_t fpsr, double a, double b)
{
    fpsr &= ~(J5Q_FPSR_N | J5Q_FPSR_Z | 0x02000000u /*I*/ | J5Q_FPSR_NAN);
    if (a != a || b != b)      fpsr |= J5Q_FPSR_NAN;   /* unordered (NaN)                       */
    else if (a < b)            fpsr |= J5Q_FPSR_N;     /* less                                  */
    else if (a == b)           fpsr |= J5Q_FPSR_Z;     /* equal                                 */
    /* greater: all clear */
    return fpsr;
}

static int j5d_fp_imm_arith(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t tpc,
                            uint16_t op, uint16_t opcode2, uint32_t *after,
                            char *errbuf, unsigned errlen)
{
#define FFAIL(msg) do { snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)
    unsigned srcspec = (opcode2 >> 10) & 7u;          /* the immediate's format (.l/.s/.w/.d/.b) */
    unsigned fpn     = (opcode2 >> 7) & 7u;           /* dst FP register                          */
    unsigned oper    = opcode2 & 0x7fu;               /* opmode (FADD/FSUB/.../FCMP/FMOVE)        */
    uint32_t ext     = tpc + 4;                       /* first word AFTER opcode + opcode2        */

    /* Read the inline immediate -> double `src`, advancing `ext` past the immediate words.  The
     * widths match the oracle: .l=4 .s=4 .w=2 .d=8 .b=2 (byte immediate occupies a full word). */
    double src = 0.0;
    if (srcspec == 0) { /* .l */
        if ((uint64_t)ext + 4 > (uint64_t)sb->origin + sb->size) FFAIL("FP imm .l out of sandbox");
        src = (double)(int32_t)be32(sb->host_mem + (ext - sb->origin)); ext += 4;
    } else if (srcspec == 1) { /* .s single */
        if ((uint64_t)ext + 4 > (uint64_t)sb->origin + sb->size) FFAIL("FP imm .s out of sandbox");
        uint32_t b = be32(sb->host_mem + (ext - sb->origin)); float f; memcpy(&f, &b, 4); src = (double)f; ext += 4;
    } else if (srcspec == 4) { /* .w */
        if ((uint64_t)ext + 2 > (uint64_t)sb->origin + sb->size) FFAIL("FP imm .w out of sandbox");
        src = (double)(int32_t)(int16_t)be16(sb->host_mem + (ext - sb->origin)); ext += 2;
    } else if (srcspec == 5) { /* .d double */
        if ((uint64_t)ext + 8 > (uint64_t)sb->origin + sb->size) FFAIL("FP imm .d out of sandbox");
        uint64_t b = 0; for (int i = 0; i < 8; i++) b = (b << 8) | sb->host_mem[(ext + i) - sb->origin];
        memcpy(&src, &b, 8); ext += 8;
    } else if (srcspec == 6) { /* .b (occupies one extension word) */
        if ((uint64_t)ext + 2 > (uint64_t)sb->origin + sb->size) FFAIL("FP imm .b out of sandbox");
        src = (double)(int32_t)(int8_t)(be16(sb->host_mem + (ext - sb->origin)) & 0xff); ext += 2;
    } else {
        FFAIL("FP imm .x/.p source format deferred (precision model)");   /* srcspec 2(.x)/3(.p) */
    }
    *after = ext;

    /* Apply the op via the SAME table the per-op exception walk uses (j5s_one_op_exc), under the
     * FPCR rounding direction + per-op fenv, and get the EXC bits.  j5s_one_op_exc advances the
     * passed-in FP file for a dst-writing op and rounds to the FPCR precision — identical to the
     * oracle's writes_dst path.  FCMP/FTST do not write FPn (handled inside j5s_one_op_exc). */
    double dst = st->fp[fpn & 7];                     /* the cc compare reference for FCMP        */
    int prev_round = fegetround();
    fesetround(j5s_host_round(st->fpcr));
    uint32_t exc = j5s_one_op_exc(st->fp, oper, fpn, src, st->fpcr);
    fesetround(prev_round);

    /* FPSR EXC byte (this op) + AEXC (sticky). */
    st->fpsr = (st->fpsr & ~J5S_FPSR_EXC_MASK) | exc;
    st->fpsr |= j5s_exc_to_aexc(exc);

    /* FPSR condition codes — set IFF a subsequent FP consumer needs them (the oracle's gate).
     * FCMP (oper 0x38) packs fcmp(dst,src); FTST (0x3a) tests src; everything else tests the
     * result now in st->fp[fpn]. */
    if (fp_imm_update_needed(sb, *after)) {
        if (oper == 0x38u)      st->fpsr = fp_pack_cc(st->fpsr, dst, src);
        else if (oper == 0x3au) st->fpsr = fp_pack_cc(st->fpsr, src, 0.0);
        else                    st->fpsr = fp_pack_cc(st->fpsr, st->fp[fpn & 7], 0.0);
    }

    /* TAKE an FP exception trap if an EXC bit fired AND its FPCR enable bit is set ([J5s] model). */
    uint32_t enabled = exc & (st->fpcr & J5S_FPCR_ENABLE_MASK);
    unsigned vec;
    if (j5s_exc_vector(enabled, &vec)) {
        uint32_t handler;
        if (raise_exception(sb, st, vec, *after, &handler, errbuf, errlen)) FFAIL("FP imm trap: no handler");
        *after = handler;                              /* take the FP exception vector            */
    }
    return 0;
#undef FFAIL
}

/* [J5s] BSUN (engine side, mirrors the oracle's j5s_bsun). A SIGNALLING FP predicate (selector
 * bit4 set) on an UNORDERED operand (FPSR NAN bit set) sets BSUN, accrues AIOP, and traps to
 * vector 48 if BSUN is enabled in FPCR. Returns 1 if a trap was TAKEN (*pc=handler, or
 * 0xFFFFFFFF on a raise failure); 0 if the op proceeds normally. */
static int j5s_engine_bsun(j5d_sandbox *sb, struct j5d_m68k_state *st, unsigned pred,
                           uint32_t return_pc, char *errbuf, unsigned errlen, uint32_t *pc)
{
    int signalling = (pred & 0x10u) != 0;     /* bit 4 = the IEEE-aware (signalling) predicate set */
    int unordered  = (st->fpsr & J5Q_FPSR_NAN) != 0;
    if (!(signalling && unordered)) return 0;
    st->fpsr |= J5S_FPSR_BSUN;
    st->fpsr |= J5S_FPSR_AIOP;
    if (st->fpcr & J5S_FPSR_BSUN) {               /* the FPCR BSUN enable bit (same position)  */
        uint32_t h;
        if (raise_exception(sb, st, J5S_VEC_FP_BSUN, return_pc, &h, errbuf, errlen)) { *pc = 0xFFFFFFFFu; return 1; }
        *pc = h;
        return 1;
    }
    return 0;
}

/* ============================ [J5k] CROSS-REGION CHAINING ==========================
 * A block whose terminator is a STATIC-TARGET control transfer can branch DIRECTLY to the
 * target block (past the C dispatcher), keeping the 68k register file live in host regs.
 * The chainable terminators are exactly those whose next PC is computable at translate time
 * AND that do not touch the (dynamic) return stack / library bridge / exception model:
 *   - BRA  (.B/.W/.L)           : one target  (always taken)
 *   - jmp abs.l (0x4EF9)        : one target
 *   - Bcc  (.B/.W/.L)           : two targets (taken PC, fall-through PC)
 * NOT chainable (stay C-dispatcher round-trips, by design): rts (return stack pop), bsr/jsr
 * abs.l + jsr/jmp (An) (push and/or computed target), jsr d16(A6) (library bridge), TRAP/
 * ILLEGAL/divu0/rte (exception model), nop/lea (handled in C — rare, no win). The spill
 * policy (docs [J5k]): regs stay live ONLY across a chained hop; EVERY dispatcher exit
 * (incl. an unresolved chain link) stores the dirty file to struct j5d_m68k_state, so the
 * memory-consistent-state boundaries (rts/library/exception/(An)) are unaffected. */

/* Decode a chainable terminator at tpc into up to two (target_pc) entries + the condition
 * field. Returns the number of targets (0 = not chainable), with targets[0] = the TAKEN /
 * unconditional target and (for Bcc) targets[1] = the FALL-THROUGH target. `*cc4` is the
 * 68k condition (>=2) for a Bcc, or 0/1 sentinel for BRA (unconditional). */
static int chain_targets(const uint8_t *thost, uint32_t tpc, uint32_t targets[2], unsigned *cc4)
{
    uint16_t top = be16(thost);
    if (top == 0x4EF9u) { targets[0] = be32(thost + 2); *cc4 = 0; return 1; }  /* jmp abs.l */
    if ((top & 0xF000u) == 0x6000u) {                                           /* Bcc/BRA   */
        unsigned c = (top >> 8) & 0xFu;
        if (c == 0x1) return 0;                       /* BSR pushes -> dispatcher-owned       */
        int8_t disp8 = (int8_t)(top & 0xFFu);
        uint32_t base = tpc + 2, target, after;
        if (disp8 == 0x00)            { int16_t d = be16s(thost + 2); target = (uint32_t)((int64_t)base + d); after = tpc + 4; }
        else if ((uint8_t)disp8==0xFF){ int32_t d = (int32_t)be32(thost + 2); target = (uint32_t)((int64_t)base + d); after = tpc + 6; }
        else                          { target = (uint32_t)((int64_t)base + disp8); after = tpc + 2; }
        if (c == 0x0) { targets[0] = target; *cc4 = 0; return 1; }  /* BRA: 1 static target  */
        targets[0] = target; targets[1] = after; *cc4 = c;          /* Bcc: taken + fall      */
        return 2;
    }
    return 0;
}

/* [J5k] emit `w_dst = (cc4 condition taken ? nonzero : 0)` from the 68k CCR word in w_cc
 * (Emu68-internal layout V=bit0,C=bit1,Z=bit2,N=bit3). VALIDATED against bcc_taken for all
 * 14 conditions x 32 CCR values (the chaining condition eval must agree with the C decision
 * the dispatcher fallback uses). Uses chaining scratch regs only (J5K_T*), never a 68k-file
 * reg. Result is strictly 0/1. cc4 must be >= 2 (a real condition; BRA never calls this). */
static uint32_t *emit_cond_bool(uint32_t *p, unsigned cc4, uint8_t w_cc, uint8_t w_dst)
{
    const uint8_t V = J5K_T0, C = J5K_T1, Z = J5K_T2, N = J5K_T3, T = J5K_T4, ONE = J5K_T5;
    *p++ = movw_immed_u16(ONE, 1);
    *p++ = ubfx(V, w_cc, 0, 1);
    *p++ = ubfx(C, w_cc, 1, 1);
    *p++ = ubfx(Z, w_cc, 2, 1);
    *p++ = ubfx(N, w_cc, 3, 1);
    switch (cc4) {
        case 0x2: *p++ = orr_reg(T, C, Z, LSL, 0); *p++ = eor_reg(w_dst, T, ONE, LSL, 0); break; /* HI: !C&&!Z */
        case 0x3: *p++ = orr_reg(w_dst, C, Z, LSL, 0); break;                                    /* LS: C||Z   */
        case 0x4: *p++ = eor_reg(w_dst, C, ONE, LSL, 0); break;                                  /* CC: !C     */
        case 0x5: *p++ = mov_reg(w_dst, C); break;                                               /* CS: C      */
        case 0x6: *p++ = eor_reg(w_dst, Z, ONE, LSL, 0); break;                                  /* NE: !Z     */
        case 0x7: *p++ = mov_reg(w_dst, Z); break;                                               /* EQ: Z      */
        case 0x8: *p++ = eor_reg(w_dst, V, ONE, LSL, 0); break;                                  /* VC: !V     */
        case 0x9: *p++ = mov_reg(w_dst, V); break;                                               /* VS: V      */
        case 0xA: *p++ = eor_reg(w_dst, N, ONE, LSL, 0); break;                                  /* PL: !N     */
        case 0xB: *p++ = mov_reg(w_dst, N); break;                                               /* MI: N      */
        case 0xC: *p++ = eor_reg(T, N, V, LSL, 0); *p++ = eor_reg(w_dst, T, ONE, LSL, 0); break; /* GE: N==V   */
        case 0xD: *p++ = eor_reg(w_dst, N, V, LSL, 0); break;                                    /* LT: N!=V   */
        case 0xE: *p++ = eor_reg(T, N, V, LSL, 0); *p++ = orr_reg(T, T, Z, LSL, 0);
                  *p++ = eor_reg(w_dst, T, ONE, LSL, 0); break;                                  /* GT         */
        case 0xF: *p++ = eor_reg(T, N, V, LSL, 0); *p++ = orr_reg(w_dst, T, Z, LSL, 0); break;   /* LE         */
        default:  *p++ = movw_immed_u16(w_dst, 0); break;
    }
    return p;
}

/* ====================== translate ONE straight-line basic block ================
 * From entry PC, drive the REAL decoders over the opcodes until (and excluding) the
 * first terminator. Emits the AArch64 into `out`; returns word count, sets *end_pc to
 * the terminator's PC. 0 + errbuf on a decode/emit error.
 *
 * [J5k] LAYOUT (two entry points + a chainable tail):
 *   [0]            OUTER ENTRY (the C function pointer): save callee regs (incl. A0..A4 =
 *                  w13..w17, which AAPCS treats as caller-saved temporaries), set x12/x1,
 *                  then LOAD ALL 16 Dn/An from the state. Loading the full file (not just
 *                  [J5e] live-in) establishes the chaining invariant "all 16 regs live at a
 *                  chain entry"; cold entries are rare next to chained hops.
 *   [chain_off]    CHAIN ENTRY: a chaining predecessor `b`-branches here. The 16 regs are
 *                  already live (left by the predecessor), x1=state, x12=base_adjust; CCR is
 *                  in memory (the predecessor flushed it), reloaded by the body's RA_GetCC.
 *   [body]         the REAL Emu68 decoders + RA_FlushCC + EMIT_FlushPC.
 *   [tail]         for a chainable terminator (BRA/jmp abs.l/Bcc): the inline condition eval
 *                  (Bcc) + the BACKPATCHABLE branch slot(s), each initially a `b` to EPI.
 *   [EPI]          EPILOGUE: store the DIRTY 68k regs to the state, restore callee, ret to
 *                  the C dispatcher (the universal fallback: unresolved link, rts, library,
 *                  exception, computed jump — every memory-consistent boundary).
 * The chain metadata (chain_off, the link slots' word offsets + target PCs, the dirty mask)
 * is returned via *cb so the dispatcher can lazily link + account the spills. */
static unsigned translate_block(j5d_sandbox *sb, uint32_t pc, uint32_t *out,
                                uint32_t *end_pc, j5d_cached_block *cb, unsigned self_idx,
                                char *errbuf, unsigned errlen)
{
#define TFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s @pc=%08x", (msg), pc); return 0; } while (0)
    j5c_ra_reset();
    _pc_rel = 0;
    cb->chain_off = 0; cb->nlinks = 0; cb->dirty_mask = 0; cb->body_insns = 0;

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
        /* [J5r] FMOVEM / FP system-register moves are opcode2-distinguished; end the block so
         * the dispatcher decodes them in C (the .x conversion + sandbox memory + reglist).
         * [J5t] An immediate-source FP arithmetic op (fadd.d #imm,fp etc.) likewise ends the
         * block — Emu68 would lower it to a PC-relative fldd that faults in our re-host; the
         * dispatcher executes it in C (j5d_fp_imm_arith) reading the immediate from the sandbox. */
        if ((op & 0xFFC0u) == 0xF200u &&
            (uint64_t)cur_pc + 4 <= (uint64_t)sb->origin + sb->size &&
            (is_fp_mem_terminator(op, be16(ophost + 2)) ||
             is_fp_imm_arith(op, be16(ophost + 2)))) { *end_pc = cur_pc; break; }

        uint8_t group = op >> 12;
        uint16_t insn_consumed = 0;
        uint16_t *before = m68k_ptr;

        switch (group) {
            case 0x0: bp = EMIT_line0(bp, &m68k_ptr, &insn_consumed); break; /* [J5g] ori/andi/eori/subi/addi/cmpi/btst/bset/bclr/bchg */
            case 0x1: bp = EMIT_move (bp, &m68k_ptr, &insn_consumed); break; /* [J5g] move.b */
            case 0x2: bp = EMIT_move (bp, &m68k_ptr, &insn_consumed); break; /* move.l/movea.l */
            case 0x3: bp = EMIT_move (bp, &m68k_ptr, &insn_consumed); break; /* [J5g] move.w/movea.w */
            case 0x4: bp = EMIT_line4(bp, &m68k_ptr, &insn_consumed); break; /* [J5g] clr/neg/not/tst/ext/swap/lea/pea */
            case 0x7: bp = EMIT_moveq(bp, &m68k_ptr, &insn_consumed); break; /* moveq          */
            case 0x5: bp = EMIT_line5(bp, &m68k_ptr, &insn_consumed); break; /* addq/subq      */
            case 0x8: bp = EMIT_line8(bp, &m68k_ptr, &insn_consumed); break; /* or/div         */
            case 0x9: bp = EMIT_line9(bp, &m68k_ptr, &insn_consumed); break; /* sub            */
            case 0xB: bp = EMIT_lineB(bp, &m68k_ptr, &insn_consumed); break; /* cmp/eor        */
            case 0xC: bp = EMIT_lineC(bp, &m68k_ptr, &insn_consumed); break; /* and/mul        */
            case 0xD: bp = EMIT_lineD(bp, &m68k_ptr, &insn_consumed); break; /* add            */
            case 0xE: bp = EMIT_lineE(bp, &m68k_ptr, &insn_consumed); break; /* [J5g] asl/asr/lsl/lsr/rol/ror */
            case 0xF: bp = EMIT_lineF(bp, &m68k_ptr, &insn_consumed); break; /* [J5o] FPU (FMOVE/FADD/...)   */
            default:  TFAIL("opcode line not in the [J5g]/[J5o] driven set (0/2/4/5/7/8/9/B/C/D/E/F + terminators)");
        }
        if (insn_consumed == 0) TFAIL("decoder consumed 0 insns (unimplemented opcode?)");

        /* The real decoder advanced m68k_ptr by (insn_consumed + ext words). Mirror the
         * PC by the same byte distance so cur_pc tracks the stream. */
        uint32_t advanced = (uint32_t)((m68k_ptr - before) * 2u);
        if (advanced == 0) TFAIL("decoder did not advance the stream");
        cur_pc += advanced;
        g_stats.insns_decoded += insn_consumed;
        cb->body_insns += insn_consumed;   /* [J5n] per-block body instruction count */
    }

    /* [J5p] STRIP Emu68's post-emit OPTIMIZER MARKERS from the body. Emu68's FP decoder
     * (M68k_LINEF.c) emits trailing sentinel words INSN_TO_LE(0xfffffff0) (and, in paths we do
     * NOT drive, 0xfffffffe / 0xffffffff) into the AArch64 stream after a transcendental's blr
     * site — markers its translator's OPTIMIZER pass (M68K_Translator.c, NOT lifted) consumes
     * and removes before the unit is finalized. They are NOT executable AArch64; left in the
     * body they would be reached as garbage and branch to an invalid PC. We do not run Emu68's
     * optimizer, so we strip them here. The body is straight-line + position-independent (no
     * intra-block branches in a [J5d] block), so compacting these words out is safe. (0xfffffff0
     * is the only marker the [J5p] DRIVEN set — transcendentals + FSINCOS + FREM — emits; the
     * others belong to FBcc/FSAVE/FRESTORE paths that are not decoded here.) */
    {
        uint32_t *rd = body, *wr = body;
        while (rd < bp) {
            uint32_t w = *rd++;
            if (w == 0xfffffff0u || w == 0xfffffffeu || w == 0xffffffffu) continue; /* marker */
            *wr++ = w;
        }
        bp = wr;
    }

    /* The CCR + PC delta flush is part of the body's exit work (touches scratch regs only,
     * not the architectural file). Append it to the body so the masks are final. [J5o]: the
     * FPCR/FPSR are flushed the same way (the FPU decoder lazily loads them via RA_ModifyFPSR
     * and we store the modified scratch back to st->fpsr/fpcr here, exactly as RA_FlushCC does
     * for the CCR). For an integer block these are no-ops (never loaded), so the existing frame
     * is byte-identical. */
    RA_FlushCC(&bp);
    RA_FlushFPCR(&bp);
    RA_FlushFPSR(&bp);
    bp = EMIT_FlushPC(bp);
    unsigned body_words = (unsigned)(bp - body);

    /* Now the live-in/dirty masks are complete. Compose the frame. */
    uint16_t live = 0, dirty = 0;
    j5c_ra_get_masks(&live, &dirty);
    cb->dirty_mask = dirty;
    /* [J5o] FP register liveness — the FP regs the block reads (load in prologue) / writes
     * (store in epilogue). For a non-FPU block these are 0, so no FP load/store is emitted and
     * the integer frame is byte-for-byte unchanged. */
    uint16_t fp_live = 0, fp_dirty = 0;
    j5c_ra_get_fp_masks(&fp_live, &fp_dirty);
    int touches_fpu = (fp_live | fp_dirty) != 0;
    cb->fpu_block = (uint8_t)(touches_fpu != 0);   /* [J5s] drive the per-block fenv exc model */
    cb->fp_dirty  = (uint8_t)(fp_dirty & 0xff);    /* [J5s] FP regs to re-round for single-prec */

    /* Decode the terminator: is it a CHAINABLE static-target transfer? [J5o]: a block that
     * touches the FPU is NOT chained — it stays a C-dispatcher round-trip (like rts / the
     * library bridge / exceptions already are). Cross-block FP-register caching would need the
     * d8..d15 file kept live across the hop AND a whole-FP-file flush at the chain-terminal
     * epilogue (mirroring the integer file's "store all 16" policy); rather than store all 8 FP
     * regs at EVERY epilogue (which would touch d8..d15 for integer blocks too and regress the
     * corpus), we keep FP blocks un-chained. FP block->block chaining is a later FP increment. */
    const uint8_t *thost = sb->host_mem + (*end_pc - sb->origin);
    uint32_t ctgt[2]; unsigned ccc4 = 0;
    int ntargets = (!touches_fpu && (uint64_t)*end_pc + 2 <= (uint64_t)sb->origin + sb->size)
                   ? chain_targets(thost, *end_pc, ctgt, &ccc4) : 0;

    uint32_t *ptr = out;

    /* ---- OUTER ENTRY: AAPCS64 callee-saved preserve. [J5k]: the preserve set now INCLUDES
     * w13..w17 (A0..A4) — they are AAPCS caller-saved temporaries, so without saving them the
     * chained-register invariant ("all 16 live across a hop") would be unsafe the moment any
     * non-leaf host code ran between blocks; saving them makes the whole 68k file genuinely
     * callee-preserved across this block's activation. w19..w28 + x29/x30 as before. */
    *ptr++ = stp64_preindex(31, 29, 30, -16);
    *ptr++ = stp64_preindex(31, 19, 20, -16);
    *ptr++ = stp64_preindex(31, 21, 22, -16);
    *ptr++ = stp64_preindex(31, 23, 24, -16);
    *ptr++ = stp64_preindex(31, 25, 26, -16);
    *ptr++ = stp64_preindex(31, 27, 28, -16);
    *ptr++ = stp64_preindex(31, 13, 14, -16);   /* [J5k] A0,A1 */
    *ptr++ = stp64_preindex(31, 15, 16, -16);   /* [J5k] A2,A3 */
    *ptr++ = stp64_preindex(31, 17, 18, -16);   /* [J5k] A4, x18(REG_PC scratch) — keep pair */

    /* [J5o] FP CALLEE-SAVE: d8..d15 are AAPCS64 callee-saved, and this block uses them as the
     * FP0..FP7 file (loading/computing into them clobbers the caller's values). Save exactly the
     * d-regs the block touches (load-in OR written — loading clobbers too) onto the stack here,
     * restore them in the epilogue, so the C dispatcher's d8..d15 are preserved across the block
     * activation. For an integer block (fp_clobber==0) NOTHING is emitted — the frame is unchanged.
     * FP blocks are un-chained (see ntargets above), so this entry + the epilogue restore always
     * pair up (no chain jumps over them). Pushed LIFO; each push is a 16-byte-aligned slot. */
    uint16_t fp_clobber = (uint16_t)((fp_live | fp_dirty) & 0xff);
    for (int i = 0; i < 8; i++)
        if (fp_clobber & (1u << i))
            *ptr++ = fstd_preindex((uint8_t)(J5O_FP0 + i), 31 /*sp*/, -16);

    /* The block is entered as block(state in x0, base_adjust in x1).  [J5g]: the STATE
     * pointer is kept in x1 for the whole block (NOT x0). Save base_adjust (x1) into x12
     * FIRST, then move the state (x0) into x1. */
    *ptr++ = mov64_reg(J5D_BASEADJ_X, 1 /*x1 = base_adjust*/);   /* x12 := base_adjust */
    *ptr++ = mov64_reg(1 /*x1*/, 0 /*x0 = state*/);              /* x1  := state ptr   */

    /* [J5k] OUTER PROLOGUE: load ALL 16 Dn/An from the state (x1) — the cold-entry full load
     * that makes the chain entry's "all 16 live" precondition hold. (A chained predecessor
     * skips this entirely, branching to chain_off below with the file already live.) */
    for (int i = 0; i < 8; i++) *ptr++ = ldr_offset(1, reg_d[i], J5D_OFF_D(i));
    for (int i = 0; i < 8; i++) *ptr++ = ldr_offset(1, reg_a[i], J5D_OFF_A(i));

    /* [J5o] FP PROLOGUE: load the FP regs the block reads (live-in) from st->fp[] into d8..d15.
     * fp[i] is at offsetof(struct j5d_m68k_state, fp[i]) — fits the fldd signed-9-bit byte
     * offset (fp[] spans bytes 80..136). Only live-in FP regs are loaded (the [J5e] minimal
     * frame); for an integer block fp_live==0 so nothing is emitted. */
    for (int i = 0; i < 8; i++)
        if (fp_live & (1u << i))
            *ptr++ = fldd((uint8_t)(J5O_FP0 + i), 1, (int16_t)offsetof(struct j5d_m68k_state, fp[i]));

    /* ---- CHAIN ENTRY: regs already live (cold path loaded them just above; chained path was
     * left them live by the predecessor). x2/x3 are dead here (the body has not run; the EA
     * helpers' x2/x3 scratch use is later), so the execution-counter bump can use them. */
    cb->chain_off = (uint32_t)(ptr - out);
    ptr = emit_counter_bump(ptr, (uint64_t)(uintptr_t)&g_block_exec_count, J5K_T0, J5K_T1);

    /* The decoder body (every register/ALU/flag/move/memory opcode, REAL Emu68 decoders). */
    memcpy(ptr, body, body_words * sizeof(uint32_t));
    ptr += body_words;

    /* ---- [J5k] CHAINABLE TAIL: the inline condition (Bcc) + backpatchable branch slot(s),
     * each emitted initially as `b EPI` (a fall to the epilogue = the C-dispatcher return).
     * The dispatcher backpatches a slot to `b <target chain entry>` once the target exists. */
    if (ntargets == 1) {
        /* BRA / jmp abs.l: one unconditional slot. */
        cb->link[0].target_pc = ctgt[0];
        cb->link[0].slot_word = (uint32_t)(ptr - out);
        cb->link[0].linked    = 0;
        cb->nlinks = 1;
        *ptr++ = b(0); /* placeholder: patched in finalize to `b EPI` (relative) below */
    } else if (ntargets == 2) {
        /* Bcc: the CCR is in MEMORY at the tail (the body's RA_FlushCC stored it). Load it
         * into a chaining scratch reg, evaluate the 68k condition inline into J5K_T2 (this
         * is byte-for-byte the bcc_taken decision the C fallback uses — VALIDATED), then:
         *   cbz w_dst, S_fall     ; not taken -> fall slot
         *   S_taken: b EPI        ; (patched -> b taken.chain_entry)
         *   S_fall:  b EPI        ; (patched -> b fall.chain_entry)
         * On the TAKEN path the cbz falls through to S_taken; on the NOT-taken path it jumps
         * to S_fall (the next-but-one word). */
        *ptr++ = ldr_offset(1, J5K_T6, J5D_OFF_CCR);          /* w10 = CCR (distinct from the
                                                               * V/C/Z/N/T/ONE scratch below) */
        ptr = emit_cond_bool(ptr, ccc4, J5K_T6, J5K_T2);      /* J5K_T2 = taken?1:0          */
        *ptr++ = cbz(J5K_T2, 2);                              /* not taken -> S_fall (+2 wd) */
        cb->link[0].target_pc = ctgt[0];                      /* TAKEN target                */
        cb->link[0].slot_word = (uint32_t)(ptr - out);
        cb->link[0].linked    = 0;
        *ptr++ = b(0);                                        /* S_taken (patched to b EPI)  */
        cb->link[1].target_pc = ctgt[1];                      /* FALL-THROUGH target         */
        cb->link[1].slot_word = (uint32_t)(ptr - out);
        cb->link[1].linked    = 0;
        *ptr++ = b(0);                                        /* S_fall  (patched to b EPI)  */
        cb->nlinks = 2;
    }

    /* ---- EPILOGUE (EPI): store back the 68k file to the state, then restore callee, ret.
     * SPILL POLICY (the correctness-critical point of [J5k]): store ALL 16 Dn/An, not just
     * this block's dirty set. Reason — a chain A->...->Z reaches EXACTLY ONE epilogue (Z's);
     * the intermediate blocks' epilogues are jumped over. The 68k file mutated by ANY block
     * in the chain is live in the host regs, but Z's `ldp` is about to restore those host
     * regs to the values the chain's HEAD (A) saved on entry from C. So Z must flush the WHOLE
     * file to st->* BEFORE the ldp, or a register written upstream (and not by Z) would be
     * lost. Storing all 16 makes st->* memory-consistent at every dispatcher exit regardless
     * of chain history — which is exactly what the rts / library bridge / exception / (An)
     * boundaries require. (The CCR is already memory-consistent: each block's body flushes it
     * via RA_FlushCC before the tail, so the last block in the chain left it current.) */
    uint32_t epi_word = (uint32_t)(ptr - out);
    /* [J5k] record THIS block as the chain-terminal block (its index, baked as an immediate),
     * so the dispatcher decodes this block's terminator after a chained run, not the head's.
     * x2/x3 are dead here (body + tail finished). */
    *ptr++ = mov64_immed_u16(J5K_T0, (uint16_t)(self_idx & 0xffff), 0);
    {
        uint64_t a = (uint64_t)(uintptr_t)&g_chain_terminal_idx;
        *ptr++ = mov64_immed_u16(J5K_T1, (uint16_t)(a        & 0xffff), 0);
        *ptr++ = movk64_immed_u16(J5K_T1, (uint16_t)((a>>16) & 0xffff), 1);
        *ptr++ = movk64_immed_u16(J5K_T1, (uint16_t)((a>>32) & 0xffff), 2);
        *ptr++ = movk64_immed_u16(J5K_T1, (uint16_t)((a>>48) & 0xffff), 3);
        *ptr++ = str_offset(J5K_T1, J5K_T0, 0);   /* g_chain_terminal_idx = self_idx (32-bit) */
    }
    unsigned reg_stores = 0;
    for (int i = 0; i < 8; i++) { *ptr++ = str_offset(1, reg_d[i], J5D_OFF_D(i)); reg_stores++; }
    for (int i = 0; i < 8; i++) { *ptr++ = str_offset(1, reg_a[i], J5D_OFF_A(i)); reg_stores++; }
    (void)dirty;

    /* [J5o] FP EPILOGUE: store the FP regs the block WROTE back to st->fp[] (the [J5e] minimal
     * frame — only dirty regs), then RESTORE the caller's d8..d15 from the stack in REVERSE
     * (LIFO) order. The store-to-state happens FIRST (d-regs still hold the computed FP values),
     * the stack restore SECOND (overwriting them with the caller's saved values). For an integer
     * block both loops emit nothing — the epilogue is byte-identical. */
    for (int i = 0; i < 8; i++)
        if (fp_dirty & (1u << i))
            *ptr++ = fstd((uint8_t)(J5O_FP0 + i), 1, (int16_t)offsetof(struct j5d_m68k_state, fp[i]));
    for (int i = 7; i >= 0; i--)
        if (fp_clobber & (1u << i))
            *ptr++ = fldd_postindex((uint8_t)(J5O_FP0 + i), 31 /*sp*/, 16);

    *ptr++ = ldp64_postindex(31, 17, 18, 16);   /* [J5k] restore A4,x18 */
    *ptr++ = ldp64_postindex(31, 15, 16, 16);   /* [J5k] A2,A3 */
    *ptr++ = ldp64_postindex(31, 13, 14, 16);   /* [J5k] A0,A1 */
    *ptr++ = ldp64_postindex(31, 27, 28, 16);
    *ptr++ = ldp64_postindex(31, 25, 26, 16);
    *ptr++ = ldp64_postindex(31, 23, 24, 16);
    *ptr++ = ldp64_postindex(31, 21, 22, 16);
    *ptr++ = ldp64_postindex(31, 19, 20, 16);
    *ptr++ = ldp64_postindex(31, 29, 30, 16);
    *ptr++ = ret();

    /* Resolve every chain slot's INITIAL word to `b EPI` (a relative branch to the epilogue).
     * A B's imm26 is the signed word distance (target - slot). EPI is always AFTER the slot. */
    for (unsigned k = 0; k < cb->nlinks; k++) {
        int32_t d = (int32_t)epi_word - (int32_t)cb->link[k].slot_word;
        out[cb->link[k].slot_word] = b((uint32_t)d & 0x3ffffffu);
    }

    /* [J5e] metrics: per-block state-struct traffic (the dirty-store half is what survives;
     * the full-file load at the cold outer entry is rare relative to chained hops). */
    g_stats.state_ldrstr_naive += 32;
    g_stats.state_ldrstr_ra    += 16 + reg_stores;          /* full load + dirty stores    */
    g_stats.reg_loads_emitted  += 16;
    g_stats.reg_stores_emitted += reg_stores;

    return (unsigned)(ptr - out);
#undef TFAIL
}

/* Get (translating + caching if needed) the compiled block at `pc`. */
static j5d_cached_block *get_block(j5d_sandbox *sb, uint32_t pc, char *errbuf, unsigned errlen)
{
    j5d_cached_block *b = cache_find(pc);
    if (b) { g_stats.block_cache_hits++; return b; }     /* [J5f] block-cache HIT: no re-translate */
    g_stats.block_cache_misses++;                        /* MISS: translate once, cache by PC      */
    if (g_cache_n >= J5D_MAX_BLOCKS) { if (errbuf) snprintf(errbuf, errlen, "block cache full"); return NULL; }

    uint32_t staging[8192];
    uint32_t end_pc = pc;
    unsigned long ea_before = g_j5d_ea_emits;
    j5d_cached_block tmp; memset(&tmp, 0, sizeof tmp);
    /* translate into a temp cb (chain metadata is region-relative word offsets, so it stays
     * valid after the staging->MAP_JIT memcpy below). self_idx = g_cache_n (this block's slot
     * index, baked into its epilogue so the dispatcher can identify a chain's terminal block). */
    unsigned nwords = translate_block(sb, pc, staging, &end_pc, &tmp, (unsigned)g_cache_n, errbuf, errlen);
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
    b->body_insns = tmp.body_insns;    /* [J5n] per-block instruction count for #N         */
    b->chain_off  = tmp.chain_off;     /* [J5k] carry the chain metadata into the cache slot */
    b->dirty_mask = tmp.dirty_mask;
    b->fpu_block  = tmp.fpu_block;     /* [J5s] FP-exception per-block fenv gate              */
    b->fp_dirty   = tmp.fp_dirty;      /* [J5s] dirty FP regs for single-precision re-round   */
    b->nlinks     = tmp.nlinks;
    for (unsigned k = 0; k < tmp.nlinks; k++) b->link[k] = tmp.link[k];
    g_cache_n++;
    g_stats.blocks_translated++;
    g_stats.arm_words_emitted += nwords;
    return b;
}

/* ============================ [J5k] LAZY LINKING / BACKPATCH ======================
 * Patch src->link[k]'s `b` slot to jump straight into tgt's CHAIN ENTRY (a cross-region
 * direct branch past the C dispatcher). A64 `b` reaches +-128 MiB (signed 26-bit word
 * offset); if the two MAP_JIT regions are farther apart the slot stays its fall-to-EPI
 * default and the edge keeps routing through C — correctness is never reachability-gated.
 * The patch is one word inside a pthread_jit write window + an i-cache invalidate of that
 * word (R-JIT-WRITE/R-JIT-ICACHE). Returns 1 if it linked, 0 if left unlinked. */
static int try_link(j5d_cached_block *src, unsigned k, j5d_cached_block *tgt)
{
    if (src->link[k].linked) return 1;
    uint32_t *slot = (uint32_t *)src->region.base + src->link[k].slot_word;
    uint32_t *dst  = (uint32_t *)tgt->region.base + tgt->chain_off;
    int64_t  word_off = (int64_t)(dst - slot);                 /* signed word distance        */
    if (word_off >  ((int64_t)1 << 25) - 1 ||                  /* out of B's +-128 MiB range  */
        word_off < -((int64_t)1 << 25)) return 0;
    jit_write_begin(&src->region);
    *slot = b((uint32_t)((int32_t)word_off) & 0x3ffffffu);
    jit_write_end(&src->region);
    jit_finalize(&src->region, slot, sizeof(uint32_t));
    src->link[k].linked = 1;
    g_stats.chain_links_patched++;
    return 1;
}

/* After a block returned to the dispatcher with a chainable terminator, the dispatcher has
 * computed the next PC. If that PC is one of the block's chain targets and its block exists,
 * link the edge so the NEXT pass branches directly (and accounts the spills the chained hop
 * will elide: the source's dirty regs that no longer round-trip through memory). */
static void link_if_resolved(j5d_sandbox *sb, j5d_cached_block *src, uint32_t next_pc)
{
    (void)sb;
    /* [J5n] when the diagnostics subsystem is active, keep chaining OFF so every block returns
     * to the C dispatcher — that makes the deterministic per-block instruction accounting (#N),
     * the flight recorder, and fault localization exact. Chaining is a hot-path optimization;
     * the diagnostics path is explicitly off the hot path (a mode is enabled / a fault occurs),
     * so this costs nothing in normal corpus runs (diag==NULL there). The chained corpus tests
     * ([J5k]/[J5j]) do NOT register a diag, so they keep full chaining. */
    if (j5d_get_diag()) return;
    if (!src || src->nlinks == 0) return;
    for (unsigned k = 0; k < src->nlinks; k++) {
        if (src->link[k].linked || src->link[k].target_pc != next_pc) continue;
        j5d_cached_block *tgt = cache_find(next_pc);
        if (!tgt) return;                 /* not translated yet — link on a later pass        */
        /* [J5t] NEVER chain INTO an FP block. FP blocks carry an FP callee-save prologue
         * (fstd d8..d15) + epilogue (fldd d8..d15) wrapping the integer chain entry; a chained
         * branch lands at tgt->chain_off (PAST that prologue) so the FP regs are never pushed,
         * yet the epilogue still pops them — corrupting the host stack -> a wild return. FP blocks
         * already never chain OUT (ntargets==0 for touches_fpu); this is the matching IN-edge
         * guard. The capstone's vbcc FP loops are the first to have an INTEGER block (a loop
         * counter test) fall into an FP block, which the hand-written FP corpus never did. The
         * edge simply stays a C round-trip (correct, just not lazily linked). */
        if (tgt->fpu_block) continue;
        try_link(src, k, tgt);
    }
}

/* ============================ the real 68k return stack =======================
 * a7 (st->a[7]) is a SANDBOX address. push: a7-=4 then write the 4-byte 68k return
 * address BIG-ENDIAN at (a7). pop: read big-endian at (a7) then a7+=4. Both validate
 * the access stays in the sandbox (a corrupt SP / return address is a clean dispatcher
 * error, NOT a host out-of-bounds write). */
static int sp_push(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t value,
                   char *errbuf, unsigned errlen)
{
    uint32_t sp = st->a[7] - 4u;
    if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size) {
        if (errbuf) snprintf(errbuf, errlen, "return-stack push: a7=%08x out of sandbox", sp);
        return 1;
    }
    uint8_t *p = sb->host_mem + (sp - sb->origin);
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);  p[3] = (uint8_t)value;          /* big-endian */
    st->a[7] = sp;
    return 0;
}
static int sp_pop(j5d_sandbox *sb, struct j5d_m68k_state *st, uint32_t *out,
                  char *errbuf, unsigned errlen)
{
    uint32_t sp = st->a[7];
    if (sp < sb->origin || (uint64_t)sp + 4 > (uint64_t)sb->origin + sb->size) {
        if (errbuf) snprintf(errbuf, errlen, "return-stack pop: a7=%08x out of sandbox", sp);
        return 1;
    }
    *out = be32(sb->host_mem + (sp - sb->origin));                  /* big-endian */
    st->a[7] = sp + 4u;
    return 0;
}

/* ============================== the dispatcher =================================
 * [J5f] PC-driven loop with the real return stack. SIGNATURE IS UNCHANGED from [J5d]:
 * the dispatcher seeds a7 to the top of the sandbox (the initial SP) and records it; a
 * top-level RTS (which pops back to that initial SP) is the program exit — so the
 * existing flat-PC corpus (which never pushes) hits its first RTS at the initial SP and
 * exits exactly as before. A subroutine program pushes/pops in between. */
int j5d_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
            struct j5d_m68k_state *st, uint32_t *exit_d0,
            j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen)
{
/* [J5n] route the clean dispatcher error through the diagnostics funnel (writes a bundle)
 * before returning, when a diag config is registered. The existing behavior (errbuf + return
 * 1) is preserved exactly; the funnel is additive. */
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg));               \
                        J5N_FUNNEL(J5N_FAULT_ENGINE, (msg), st, sb); return 1; } while (0)
    memset(&g_stats, 0, sizeof(g_stats));
    g_block_exec_count = 0;                         /* [J5k] reset the JIT-side exec counter */
    g_insn_number = 0;                              /* [J5n] reset the deterministic insn #N */
    st->a[6] = a6_libbase;                          /* A6 = library base (AmigaOS)  */

    /* [J5n] register the live 68k state + sandbox with the host-signal net so a genuine host
     * SIGSEGV/SIGBUS in translated code recovers THIS context (NULL-safe; only meaningful
     * when the diagnostics signal net is installed). */
    j5n_signal_set_context(st, sb);

    /* Seed the return stack: a7 = top of sandbox (16-byte aligned), recorded as the
     * initial SP. A top-level RTS returns to here = program exit. A caller may preset
     * st->a[7] to an explicit SP (nonzero); we honour it but still record it as the
     * top-of-stack baseline. */
    uint32_t initial_sp = st->a[7];
    if (initial_sp == 0)
        initial_sp = (sb->origin + sb->size) & ~0xFu;   /* default: top of sandbox */
    st->a[7] = initial_sp;
    st->pc   = entry_pc;

    uint64_t base_adjust = (uint64_t)(uintptr_t)sb->host_mem - (uint64_t)sb->origin;
    uint32_t pc = entry_pc;
    uint64_t steps = 0;
    uint32_t depth = 0;                              /* current nested-call depth   */

    /* Runaway guard: bound dispatcher steps so a corrupt/looping program can't hang the
     * unattended harness. Default 2,000,000 (≈9M instructions). A legitimate long run — a
     * benchmark, say — raises it via the JIT68K_STEP_CAP env var (decimal or 0x-hex). */
    uint64_t step_cap = 2000000u;
    { const char *e = getenv("JIT68K_STEP_CAP");
      if (e && *e) { unsigned long long v = strtoull(e, (char **)0, 0); if (v) step_cap = (uint64_t)v; } }

    for (;;) {
        if (++steps > step_cap) RFAIL("dispatcher step cap (runaway)");

        /* [J5i] ADDRESS / BUS error: a jump/return to a PC OUTSIDE the sandbox (a corrupt
         * return address, a wild computed jmp/jsr(An), a bad branch target) is the hosted
         * stand-in for the host SIGSEGV the integrated JIT would take (graft/cpu_aarch64.h
         * seam). Rather than a clean dispatcher error, vector it as a 68k exception:
         *   - odd PC  -> ADDRESS error (vector 3): 68k instructions must be word-aligned.
         *   - PC out of sandbox -> BUS error (vector 2): an access to nonexistent memory.
         * The vector table itself lives in the sandbox, so the handler PC is reachable; the
         * saved frame PC is the bad target (group-0 convention: the faulting address). If
         * the program has NOT installed these vectors (handler reads as 0 / out of range),
         * raise_exception fails cleanly — no host crash either way. */
        if ((pc & 1u) && (pc >= sb->origin) && ((uint64_t)pc + 2 <= (uint64_t)sb->origin + sb->size)) {
            uint32_t handler;
            if (raise_exception(sb, st, J5I_VEC_ADDRESS_ERROR, pc, &handler, errbuf, errlen)) {
                /* [J5n] no 68k handler installed -> an unrecoverable address fault: bundle it. */
                char dt[96]; snprintf(dt, sizeof dt, "misaligned PC 0x%08X (68k address error, vector 3)", pc);
                J5N_FUNNEL(J5N_FAULT_ADDRESS, dt, st, sb);
                return 1;
            }
            { char dt[96]; snprintf(dt, sizeof dt, "misaligned PC 0x%08X (68k address error, vector 3, no handler)", pc);
              J5N_UNHANDLED(J5N_FAULT_ADDRESS, dt, st, sb, handler); }
            pc = handler; continue;
        }
        if (pc < sb->origin || (uint64_t)pc + 2 > (uint64_t)sb->origin + sb->size) {
            uint32_t handler;
            if (raise_exception(sb, st, J5I_VEC_BUS_ERROR, pc, &handler, errbuf, errlen)) {
                /* [J5n] no 68k handler -> unrecoverable bus fault (PC outside the sandbox). */
                char dt[128]; snprintf(dt, sizeof dt,
                    "PC 0x%08X outside sandbox 0x%08X..0x%08X (68k bus error, vector 2)",
                    pc, sb->origin, sb->origin + sb->size);
                J5N_FUNNEL(J5N_FAULT_BUS, dt, st, sb);
                return 1;
            }
            { char dt[128]; snprintf(dt, sizeof dt,
                "PC 0x%08X outside sandbox 0x%08X..0x%08X (68k bus error, vector 2, no handler)",
                pc, sb->origin, sb->origin + sb->size);
              J5N_UNHANDLED(J5N_FAULT_BUS, dt, st, sb, handler); }
            pc = handler; continue;
        }

        /* (1)+(2): look up (translate-on-miss + cache) + run the block at pc. */
        j5d_cached_block *b = get_block(sb, pc, errbuf, errlen);
        if (!b) return 1;                            /* errbuf set                  */

        st->pc = pc;
        if (getenv("J5G_TRACE"))      /* opt-in per-block PC trace (debug aid, off by default) */
            fprintf(stderr, "[blk] pc=%08x end=%08x\n", pc, b->end_pc);

        /* [J5n] DIAGNOSTICS: when a diag config is registered, keep the signal net's view of
         * the live state current, record this block's instructions into the flight recorder
         * BEFORE running (so a fault inside has the trail), and advance the deterministic
         * global instruction number. Chaining is gated OFF in this mode (below) so each block
         * returns to C and the per-block accounting is exact. NULL = the zero-overhead path. */
        j5n_diag *diag = j5d_get_diag();
        if (diag) {
            j5n_signal_set_context(st, sb);
            j5n_diag_record_block(diag, sb, pc, b->end_pc);
        }

        /* [J5k] enter the block at its OUTER entry. If its tail was already linked to a cached
         * successor, the JIT'd code branches block->block past this dispatcher (counted by the
         * chain-entry exec bump), and only RE-ENTERS C at the end of the chain — so one C call
         * here may run a whole CHAIN of blocks. */
        g_chain_terminal_idx = (uint32_t)(b - g_cache);   /* default: no chaining ran          */

        /* [J5s] THE FP EXCEPTION MODEL. FP blocks are never chained (translate_block keeps
         * touches_fpu blocks un-chained), so this block runs alone and returns to C here. The
         * native run sets the rounding direction we apply (fesetround), produces the FP register +
         * memory RESULTS, and we then derive the FPSR EXC/AEXC by a PER-OP C RE-WALK over the
         * block's FP ops (j5s_fp_exc_walk) — last-op-wins EXC + sticky AEXC, the SAME per-op model
         * the oracle uses, so the FPSR is bit-exact. A non-FP block (fpu_block==0, the whole integer
         * corpus) skips ALL of this: zero overhead, byte-identical to before. */
        int j5s_fp = b->fpu_block;
        double j5s_fp_pre[8];
        int j5s_prev_round = 0;
        uint32_t j5s_blk_start = b->pc, j5s_blk_end = b->end_pc;
        if (j5s_fp) {
            memcpy(j5s_fp_pre, st->fp, sizeof j5s_fp_pre);   /* operands for the per-op re-walk */
            j5s_prev_round = fegetround();
            fesetround(j5s_host_round(st->fpcr));            /* the native FP honours this      */
        }

        ((j5d_block_fn)(void *)b->region.base)(st, base_adjust);
        g_stats.dispatcher_roundtrips++;             /* [J5k] one C-dispatcher round-trip      */

        if (j5s_fp) {
            /* single-precision rounding: re-round the FP regs this block wrote through float
             * (the native ops computed double; the FPCR PREC byte may select single). */
            if ((st->fpcr & J5S_FPCR_PREC_MASK) == J5S_FPCR_PREC_S) {
                for (int fpi = 0; fpi < 8; fpi++)
                    if (b->fp_dirty & (1u << fpi)) st->fp[fpi] = j5s_round_prec(st->fp[fpi], st->fpcr);
            }
            uint32_t exc = 0, aexc = 0;
            j5s_fp_exc_walk(sb, st, j5s_fp_pre, j5s_blk_start, j5s_blk_end, &exc, &aexc);
            fesetround(j5s_prev_round);
            uint32_t trap_pc = 0;
            if (j5s_fp_exc_apply(sb, st, exc, aexc, j5s_blk_end, &trap_pc, errbuf, errlen)) {
                if (trap_pc == 0xFFFFFFFFu) {            /* enabled trap but no handler installed */
                    char dt[96]; snprintf(dt, sizeof dt, "FP exception trap with no handler @pc=%08x", j5s_blk_end);
                    J5N_FUNNEL(J5N_FAULT_ENGINE, dt, st, sb);
                    return 1;
                }
                g_stats.exceptions_dispatched++;
                pc = trap_pc;                            /* take the FP exception vector          */
                continue;
            }
        }
        /* [J5n] #N counts EVERY 68k instruction (body + the one terminator), matching the
         * instruction-precise oracle, so a fault's #N (set in the funnel) equals the oracle
         * index of the faulting instruction and replay-to-N lands on it. After running this
         * block's body we are positioned AT its terminator; the terminator's own #N is
         * g_insn_number + body_insns (added below once the terminator is dispatched). */
        if (diag) g_insn_number += b->body_insns;    /* [J5n] advance #N past this block's body */

        /* (3): the chain may have run past `b`; the TERMINAL block's epilogue recorded its own
         * index. Decode the TERMINAL block's terminator (not the head's) and take the next PC. */
        b = &g_cache[g_chain_terminal_idx];
        uint32_t tpc = b->end_pc;
        if (tpc + 2 > sb->origin + sb->size) RFAIL("terminator pc out of sandbox");
        const uint8_t *thost = sb->host_mem + (tpc - sb->origin);
        uint16_t top = be16(thost);

        if (top == 0x4E75u) {                        /* rts -> POP the return stack */
            if (st->a[7] >= initial_sp) {            /* back at the initial SP: exit */
                *exit_d0 = st->d[0];
                finalize_stats();                    /* [J5k] derive chaining metrics         */
                return 0;
            }
            uint32_t ret;
            if (sp_pop(sb, st, &ret, errbuf, errlen)) return 1;
            g_stats.returns_popped++;
            if (depth) depth--;
            pc = ret;
        }
        else if (top == 0x4E73u) {                   /* [J5i] rte -> pop SR+PC, resume */
            /* The frame is at a7: SR (16-bit BE) @ a7, PC (32-bit BE) @ a7+2. Pop both,
             * a7 += 6, restore S + the condition codes from the saved SR, resume at PC. */
            uint32_t a7 = st->a[7];
            if (a7 < sb->origin || (uint64_t)a7 + 6 > (uint64_t)sb->origin + sb->size)
                RFAIL("rte: frame a7 out of sandbox");
            const uint8_t *fp = sb->host_mem + (a7 - sb->origin);
            uint16_t sr = (uint16_t)((fp[0] << 8) | fp[1]);
            uint32_t rpc = be32(fp + 2);
            st->a[7] = a7 + 6u;
            j5d_unpack_sr(st, sr);                   /* restore CCR + the system byte (S) */
            g_stats.rte_returns++;
            pc = rpc;
        }
        else if ((top & 0xFFF0u) == 0x4E40u) {       /* [J5i] TRAP #n -> vector 32+n */
            unsigned n = top & 0xFu;
            uint32_t handler;
            /* TRAP is group 2: the saved PC is the instruction AFTER the TRAP (2 bytes). */
            if (raise_exception(sb, st, J5I_VEC_TRAP_BASE + n, tpc + 2, &handler, errbuf, errlen)) {
                char dt[80]; snprintf(dt, sizeof dt, "TRAP #%u with no handler (vector %u)", n, J5I_VEC_TRAP_BASE + n);
                J5N_FUNNEL(J5N_FAULT_ENGINE, dt, st, sb);
                return 1;
            }
            pc = handler;
        }
        else if (top == 0x4AFCu) {                   /* [J5i] ILLEGAL -> vector 4    */
            uint32_t handler;
            /* Illegal is group 1: the saved PC is the faulting instruction itself. */
            if (raise_exception(sb, st, J5I_VEC_ILLEGAL, tpc, &handler, errbuf, errlen)) {
                /* [J5n] illegal instruction with no 68k handler -> unrecoverable: bundle it. */
                char dt[96]; snprintf(dt, sizeof dt, "ILLEGAL instruction 0x4AFC at PC 0x%08X (68k vector 4)", tpc);
                J5N_FUNNEL(J5N_FAULT_ILLEGAL, dt, st, sb);
                return 1;
            }
            { char dt[96]; snprintf(dt, sizeof dt, "ILLEGAL instruction 0x4AFC at PC 0x%08X (68k vector 4, no handler)", tpc);
              J5N_UNHANDLED(J5N_FAULT_ILLEGAL, dt, st, sb, handler); }
            pc = handler;
        }
        else if ((top & 0xFFC0u) == 0x80C0u || (top & 0xFFC0u) == 0x81C0u) {
            /* [J5i] divu.w/divs.w — the dispatcher computes the (16-bit) division IN C so
             * a ZERO divisor can vector to 5 (Emu68's decoder would emit a branch into the
             * un-rehosted bare-metal EMIT_Exception/VBR path — a no-op stub here). Only the
             * register-direct + #imm source forms the test uses are decoded; an unsupported
             * EA mode is a clean error, not a silent miss. dst = Dn = (top>>9)&7. */
            int is_signed = ((top & 0xFFC0u) == 0x81C0u);
            unsigned dn = (top >> 9) & 7u;
            unsigned mode = (top >> 3) & 7u, srcreg = top & 7u;
            uint32_t divisor; uint32_t after;
            if (mode == 0) {                         /* Dm direct */
                divisor = st->d[srcreg] & 0xFFFFu; after = tpc + 2;
            } else if (mode == 7 && srcreg == 4) {   /* #imm.w     */
                divisor = be16(thost + 2);           after = tpc + 4;
            } else {
                RFAIL("divu/divs: unsupported source EA (only Dm and #imm.w decoded in C)");
            }
            if (divisor == 0) {                      /* -> vector 5 (group 2: save PC after) */
                uint32_t handler;
                if (raise_exception(sb, st, J5I_VEC_DIV_BY_ZERO, after, &handler, errbuf, errlen)) {
                    /* [J5n] divide-by-zero with no 68k handler -> unrecoverable: bundle it. */
                    char dt[96]; snprintf(dt, sizeof dt, "divide by zero (%s.w) at PC 0x%08X (68k vector 5)",
                                          is_signed ? "divs" : "divu", tpc);
                    J5N_FUNNEL(J5N_FAULT_DIVZERO, dt, st, sb);
                    return 1;
                }
                { char dt[96]; snprintf(dt, sizeof dt, "divide by zero (%s.w) at PC 0x%08X (68k vector 5, no handler)",
                                        is_signed ? "divs" : "divu", tpc);
                  J5N_UNHANDLED(J5N_FAULT_DIVZERO, dt, st, sb, handler); }
                pc = handler;
            } else {
                uint32_t dividend = st->d[dn];       /* 32-bit dividend */
                uint32_t quot, rem;
                if (is_signed) {
                    int32_t q = (int32_t)dividend / (int32_t)(int16_t)divisor;
                    int32_t r = (int32_t)dividend % (int32_t)(int16_t)divisor;
                    quot = (uint32_t)q; rem = (uint32_t)r;
                } else {
                    quot = dividend / divisor; rem = dividend % divisor;
                }
                /* result = (remainder.w << 16) | (quotient.w); NZ from the quotient word,
                 * V on quotient overflow (>16 bits), C=0, X unaffected (PRM DIVU/DIVS). */
                uint32_t cc = st->ccr & J5D_CCR_X;
                int ovf = is_signed ? ((int32_t)quot < -32768 || (int32_t)quot > 32767)
                                    : (quot > 0xFFFFu);
                if (ovf) {
                    cc |= J5D_CCR_V;                 /* overflow: result undefined, regs kept */
                    st->ccr = cc;
                } else {
                    st->d[dn] = ((rem & 0xFFFFu) << 16) | (quot & 0xFFFFu);
                    uint16_t qw = (uint16_t)quot;
                    if (qw == 0)            cc |= J5D_CCR_Z;
                    if (qw & 0x8000u)       cc |= J5D_CCR_N;
                    st->ccr = cc;
                }
                pc = after;
            }
        }
        else if (top == 0x4E71u) {                   /* nop                          */
            pc = tpc + 2;
        }
        else if (top == 0x4EAEu) {                   /* jsr d16(A6) -> [J3] bridge  */
            int16_t d16 = be16s(thost + 2);
            uint32_t target = st->a[6] + (uint32_t)(int32_t)d16;
            int n = j3_vector_recognise(st->a[6], target);  /* negative-offset rule */
            if (n < 0) RFAIL("jsr(A6): target not a valid library vector");
            if (!lvo)  RFAIL("jsr(A6): library call but no bridge registered");
            char e2[160] = {0};
            /* The bridge marshals 68k regs (in *st) into the native stub via the REAL
             * [J3] marshaller and writes any return into st->d[0]. A host-C call that
             * returns immediately — NO return-stack push (the 68k program never sees a
             * pushed frame for a library vector). */
            if (lvo(n, (struct j5d_m68k_state *)st, user, e2, sizeof e2)) {
                if (errbuf) snprintf(errbuf, errlen, "lib bridge: %s", e2);
                return 1;
            }
            g_stats.lib_calls++;
            pc = tpc + 4;                            /* jsr d16(A6) is 4 bytes      */
        }
        else if (top == 0x4EB9u) {                   /* jsr abs.l -> push + jump    */
            uint32_t target = be32(thost + 2);
            if (sp_push(sb, st, tpc + 6, errbuf, errlen)) return 1;  /* ret = after the jsr */
            g_stats.calls_pushed++;
            if (++depth > g_stats.max_call_depth) g_stats.max_call_depth = depth;
            pc = target;
        }
        else if ((top & 0xFFF8u) == 0x4E90u) {       /* jsr (An) -> computed push+jump */
            unsigned an = top & 7u;
            uint32_t target = st->a[an];
            if (sp_push(sb, st, tpc + 2, errbuf, errlen)) return 1;  /* jsr (An) is 2 bytes */
            g_stats.calls_pushed++;
            g_stats.computed_jumps++;
            if (++depth > g_stats.max_call_depth) g_stats.max_call_depth = depth;
            pc = target;
        }
        else if (top == 0x4EF9u) {                   /* jmp abs.l -> jump (no push) */
            pc = be32(thost + 2);
        }
        else if ((top & 0xFFF8u) == 0x4ED0u) {       /* jmp (An) -> computed (no push) */
            unsigned an = top & 7u;
            g_stats.computed_jumps++;
            pc = st->a[an];
        }
        else if ((top & 0xF000u) == 0x6000u) {       /* Bcc/BRA/BSR, any .B/.W/.L    */
            unsigned cc4 = (top >> 8) & 0xFu;        /* 0=BRA 1=BSR else condition  */
            int8_t  disp8 = (int8_t)(top & 0xFFu);
            uint32_t base = tpc + 2;                 /* disp is relative to (PC+2)  */
            uint32_t target; uint32_t after;         /* `after` = instruction end   */
            if (disp8 == 0x00) {                     /* .W : 16-bit displacement    */
                int16_t d16 = be16s(thost + 2);
                target = (uint32_t)((int64_t)base + d16);
                after  = tpc + 4;
            } else if ((uint8_t)disp8 == 0xFFu) {    /* .L : 32-bit displacement    */
                int32_t d32 = (int32_t)be32(thost + 2);
                target = (uint32_t)((int64_t)base + d32);
                after  = tpc + 6;
            } else {                                 /* .B : 8-bit displacement     */
                target = (uint32_t)((int64_t)base + disp8);
                after  = tpc + 2;
            }
            if (cc4 == 0x1) {                        /* BSR -> push + jump          */
                if (sp_push(sb, st, after, errbuf, errlen)) return 1;
                g_stats.calls_pushed++;
                if (++depth > g_stats.max_call_depth) g_stats.max_call_depth = depth;
                pc = target;
            } else {                                 /* BRA (cc4==0) or Bcc         */
                int take = (cc4 == 0x0) ? 1 : bcc_taken(cc4, st->ccr);
                pc = take ? target : after;
            }
        }
        else if ((top & 0xF0F8u) == 0x50C8u) {       /* [J5u] DBcc Dn,disp16 (decrement-and-branch) */
            /* Loop primitive (the integer twin of the [J5q] FDBcc above): if the 68k
             * condition is TRUE the loop EXITS (fall through); else Dn.W -= 1 and, while the
             * .W counter has not run past -1, branch to (PC-of-disp)+disp16. The condition
             * field reuses bcc_taken (cc>=2); cc=0 is DBT (always true -> exit), cc=1 is
             * DBF/DBRA (always false -> pure counter). Only the low WORD of Dn is the counter;
             * the high word is preserved. vbcc emits DBF for its movem/struct-copy loops, which
             * the hand-built corpus never used — so DBcc was missing from is_terminator until
             * real compiler output surfaced it. */
            unsigned cc4 = (top >> 8) & 0xFu;
            unsigned dn  = top & 7u;
            int16_t  disp16  = be16s(thost + 2);     /* signed 16-bit displacement       */
            uint32_t disp_pc = tpc + 2;              /* disp is relative to the disp word */
            uint32_t after   = tpc + 4;              /* opcode + disp = 4 bytes          */
            int cond_true = (cc4 == 0x0) ? 1 : (cc4 == 0x1) ? 0 : bcc_taken(cc4, st->ccr);
            if (cond_true) {
                pc = after;                          /* condition true -> exit the loop  */
            } else {
                uint16_t cnt = (uint16_t)(st->d[dn] & 0xFFFFu);
                cnt = (uint16_t)(cnt - 1);
                st->d[dn] = (st->d[dn] & 0xFFFF0000u) | cnt;   /* .W counter, hi word kept */
                pc = (cnt == 0xFFFFu)                /* counter expired (-1) -> fall through */
                     ? after
                     : (uint32_t)((int64_t)disp_pc + disp16);
            }
        }
        else if ((top & 0xF1FFu) == 0x41F9u) {       /* lea abs.l,An (address compute) */
            unsigned an = (top >> 9) & 7u;
            st->a[an] = be32(thost + 2);             /* the loader already relocated it */
            pc = tpc + 6;                            /* lea abs.l is 6 bytes        */
        }
        else if ((top & 0xF1FFu) == 0x41FAu) {       /* lea (d16,pc),An (PC-relative)  */
            unsigned an = (top >> 9) & 7u;
            int16_t d16 = be16s(thost + 2);          /* PC = address of the ext word   */
            st->a[an] = (uint32_t)((int64_t)(tpc + 2) + d16);
            pc = tpc + 4;                            /* lea (d16,pc) is 4 bytes        */
        }
        /* =================== [J5q] FP CONDITIONAL CONTROL-FLOW (dispatcher-level C) ===========
         * Read the FPSR cc the preceding FCMP/FTST computed (memory-backed in st->fpsr) and
         * evaluate the 68881 FP predicate (j5q_fp_cond_taken). Same funnel as integer Bcc: the
         * decode + the predicate are OURS in C, NOT the bare-metal REG_PC branch funnel Emu68's
         * FBcc/FScc emit (FDBcc/FTRAPcc have no decoder body at all). Order: FDBcc (mode 001) +
         * FTRAPcc (mode 111) carve out of the FScc EA range, so test them first. */
        else if ((top & 0xFFF8u) == 0xF248u) {       /* FDBcc Dn,disp16                */
            /* opcode2 = command word (predicate6 in low 6 bits); a 16-bit displacement follows.
             * Semantics (FP-condition DBcc): if cond TRUE -> fall through; else Dn.W -= 1, and
             * if Dn.W != -1 -> branch to (PC-of-disp) + disp16, else fall through. */
            unsigned dn = top & 7u;
            unsigned pred = be16(thost + 2) & 0x3fu;
            int16_t disp16 = be16s(thost + 4);
            uint32_t disp_pc = tpc + 4;              /* disp is relative to the disp word  */
            uint32_t after = tpc + 6;                /* FDBcc is opcode+cmdword+disp = 6 B */
            { uint32_t bpc; if (j5s_engine_bsun(sb, st, pred, after, errbuf, errlen, &bpc)) {  /* [J5s] BSUN */
                if (bpc == 0xFFFFFFFFu) RFAIL("BSUN trap with no handler (vector 48)");
                g_stats.fp_cc_ops++; g_stats.exceptions_dispatched++; pc = bpc; continue; } }
            if (j5q_fp_cond_taken(pred, st->fpsr)) {
                pc = after;                          /* condition true -> terminate the loop */
            } else {
                uint16_t cnt = (uint16_t)(st->d[dn] & 0xFFFFu);
                cnt = (uint16_t)(cnt - 1);
                st->d[dn] = (st->d[dn] & 0xFFFF0000u) | cnt;   /* .W counter, hi word kept   */
                pc = (cnt == 0xFFFFu)                /* counter expired (-1) -> fall through */
                     ? after
                     : (uint32_t)((int64_t)disp_pc + disp16);
            }
            g_stats.fp_cc_ops++;
        }
        else if ((top & 0xFFF8u) == 0xF278u) {       /* FTRAPcc (vector 7 on condition) */
            /* mode = top & 7: 2 = .W operand (1 ext word), 3 = .L operand (2 ext words),
             * 4 = no operand. opcode2 = predicate. The operand word(s) are ignored (the trap
             * itself carries no data here). On a TRUE condition raise vector 7 (TRAPcc). */
            unsigned mode = top & 7u;
            unsigned pred = be16(thost + 2) & 0x3fu;
            uint32_t after = tpc + 4;                /* opcode + command word            */
            if (mode == 2) after += 2;               /* + .W operand                     */
            else if (mode == 3) after += 4;          /* + .L operand                     */
            { uint32_t bpc; if (j5s_engine_bsun(sb, st, pred, after, errbuf, errlen, &bpc)) {  /* [J5s] BSUN */
                if (bpc == 0xFFFFFFFFu) RFAIL("BSUN trap with no handler (vector 48)");
                g_stats.fp_cc_ops++; g_stats.exceptions_dispatched++; pc = bpc; continue; } }
            if (j5q_fp_cond_taken(pred, st->fpsr)) {
                uint32_t handler;
                /* TRAPcc is a group-2 exception: the saved PC is the instruction AFTER. */
                if (raise_exception(sb, st, J5I_VECTOR_TRAPcc, after, &handler, errbuf, errlen)) {
                    char dt[96]; snprintf(dt, sizeof dt, "FTRAPcc with no handler (vector %u)", J5I_VECTOR_TRAPcc);
                    J5N_FUNNEL(J5N_FAULT_ENGINE, dt, st, sb);
                    return 1;
                }
                pc = handler;
            } else {
                pc = after;
            }
            g_stats.fp_cc_ops++;
        }
        else if ((top & 0xFFC0u) == 0xF240u) {       /* FScc <ea> (set byte on condition) */
            /* opcode2 = predicate. dest EA = top & 0x3f. Set the byte to 0xFF (true) / 0x00
             * (false). Only the Dn-direct + (An)/(d16,An) forms the test uses are decoded; an
             * unsupported EA mode is a clean error. FScc does NOT transfer control — it falls
             * through to the next instruction (it is a terminator only because it is line-F). */
            unsigned pred = be16(thost + 2) & 0x3fu;
            unsigned mode = (top >> 3) & 7u, regn = top & 7u;
            uint32_t after0 = tpc + 4; if (mode == 5) after0 += 2;
            { uint32_t bpc; if (j5s_engine_bsun(sb, st, pred, after0, errbuf, errlen, &bpc)) {  /* [J5s] BSUN */
                if (bpc == 0xFFFFFFFFu) RFAIL("BSUN trap with no handler (vector 48)");
                g_stats.fp_cc_ops++; g_stats.exceptions_dispatched++; pc = bpc; continue; } }
            uint8_t val = j5q_fp_cond_taken(pred, st->fpsr) ? 0xFFu : 0x00u;
            uint32_t after = tpc + 4;                /* opcode + command word            */
            if (mode == 0) {                         /* Dn direct: low byte              */
                st->d[regn] = (st->d[regn] & 0xFFFFFF00u) | val;
            } else if (mode == 2) {                  /* (An)                              */
                uint32_t a = st->a[regn];
                if (a < sb->origin || (uint64_t)a + 1 > (uint64_t)sb->origin + sb->size)
                    RFAIL("FScc (An) destination out of sandbox");
                sb->host_mem[a - sb->origin] = val;
            } else if (mode == 5) {                  /* (d16,An)                         */
                int16_t d16 = be16s(thost + 4); after += 2;
                uint32_t a = st->a[regn] + (uint32_t)(int32_t)d16;
                if (a < sb->origin || (uint64_t)a + 1 > (uint64_t)sb->origin + sb->size)
                    RFAIL("FScc (d16,An) destination out of sandbox");
                sb->host_mem[a - sb->origin] = val;
            } else {
                RFAIL("FScc destination EA mode not in the [J5q] subset");
            }
            pc = after;                              /* fall through (no control transfer) */
            g_stats.fp_cc_ops++;
        }
        else if ((top & 0xFF80u) == 0xF280u) {       /* FBcc (.W/.L) incl FNOP (0xF280) */
            /* predicate = low 6 bits of the opcode; bit6 = size (0=.W disp16, 1=.L disp32).
             * disp is relative to the address of the first displacement word (PC+2). */
            unsigned pred = top & 0x3fu;
            int is_long = (top & 0x40u) != 0;
            uint32_t disp_pc = tpc + 2;
            int64_t disp; uint32_t after;
            if (is_long) { disp = (int32_t)be32(thost + 2); after = tpc + 6; }
            else         { disp = (int16_t)be16s(thost + 2); after = tpc + 4; }
            { uint32_t bpc; if (j5s_engine_bsun(sb, st, pred, after, errbuf, errlen, &bpc)) {  /* [J5s] BSUN */
                if (bpc == 0xFFFFFFFFu) RFAIL("BSUN trap with no handler (vector 48)");
                g_stats.fp_cc_ops++; g_stats.exceptions_dispatched++; pc = bpc; continue; } }
            pc = j5q_fp_cond_taken(pred, st->fpsr)
                 ? (uint32_t)((int64_t)disp_pc + disp)
                 : after;
            g_stats.fp_cc_ops++;
        }
        /* =================== [J5r] FMOVEM + FP SYSTEM-REGISTER MOVES (dispatcher-level C) =====
         * Decoded here in OUR code (the .x 96-bit conversion + the sandbox memory + the reglist
         * are OURS — see j5d_jit68k.h j5r_double_to_x/j5r_x_to_double + j5d_fmovem_* below). The
         * FP register file is canonical in st->fp[] at this boundary (the block epilogue flushed
         * the dirty d8..d15); FPCR/FPSR are memory-backed in st->fpcr/st->fpsr; FPIAR in
         * st->fpiar. opcode2 selects the form. */
        else if ((top & 0xFFC0u) == 0xF200u) {
            uint16_t opcode2 = be16(thost + 2);
            char e2[120] = {0};
            uint32_t after = 0;
            int rc2;
            if (is_fp_imm_arith(top, opcode2))                                            /* [J5t] #imm src */
                rc2 = j5d_fp_imm_arith(sb, st, tpc, top, opcode2, &after, e2, sizeof e2);
            else if ((opcode2 & 0xC700u) == 0xC000u)
                rc2 = j5d_fmovem_x(sb, st, tpc, top, opcode2, &after, e2, sizeof e2);     /* FMOVEM .x  */
            else /* (opcode2 & 0xE000) == 0x8000 (to ctrl) or 0xA000 (from ctrl) */
                rc2 = j5d_fmove_sysreg(sb, st, tpc, top, opcode2, &after, e2, sizeof e2); /* sys-reg    */
            if (rc2) RFAIL(e2);
            g_stats.fp_movem_ops++;
            pc = after;
        }
        else {
            RFAIL("unknown terminator opcode");
        }

        /* [J5n] the terminator just dispatched (rts/branch/jsr/nop/etc.) is one more 68k
         * instruction — advance #N so the NEXT block's body is numbered consistently with the
         * instruction-precise oracle. (Faulting terminators returned above with #N already
         * pointing at the faulting instruction.) */
        if (diag) g_insn_number += 1;

        /* [J5n] DIFFERENTIAL block-boundary hook: when the lockstep diff driver registered a
         * callback, hand it the next-block-entry PC + the flushed JIT state so it can advance
         * the oracle to the same boundary and compare. A nonzero return (a divergence was found
         * + bundled, or a replay-to-N landing) STOPS the run here cleanly. */
        if (g_block_boundary_cb &&
            g_block_boundary_cb(g_block_boundary_user, sb, st, pc)) {
            *exit_d0 = st->d[0];
            finalize_stats();
            return 0;
        }

        /* [J5k] LAZY LINK: this block ended in a chainable terminator (BRA/Bcc/jmp abs.l) and
         * we just computed the next PC. If the successor block is translated, backpatch this
         * block's tail slot to branch directly into it, so the NEXT pass over this edge skips
         * the C round-trip and keeps the register file live. No-op for non-chainable
         * terminators (b->nlinks==0: rts/jsr/library/computed-jump/exception). */
        link_if_resolved(sb, b, pc);
    }
#undef RFAIL
}

/* ============================ [J5n] THE DIFFERENTIAL (LOCKSTEP) DRIVER ============================
 * Run the JIT and the independent interpreter ORACLE in lockstep and TRAP at the first 68k
 * instruction where their state diverges — turning the test-time oracle into a runtime
 * mistranslation locator. The JIT executes per BLOCK (its natural state-flush boundary, per
 * the frozen seam); the oracle is instruction-precise. So we compare at each block boundary,
 * and when a block boundary disagrees we re-run the oracle instruction-stepped through that
 * block to NAME the first diverging instruction (block-boundary detection + instruction-precise
 * localization). On a divergence we fill the diag (diverge_*), call the funnel (writing the
 * bundle incl. diverge.txt), and stop.
 *
 * The oracle runs over its OWN mirror state + sandbox (`ref_st`,`ref_sb`), which the caller
 * loaded identically. The driver advances the oracle to each JIT block-entry PC via the
 * interp's stop-at-PC side-channel, then compares. Returns the same (0=ok / 1=error) contract
 * as j5d_run; *exit_d0 = the JIT's final d0. */
void  j5d_interp_set_stop_pc(int active, uint32_t pc);
extern uint16_t j5d_pack_sr(const struct j5d_m68k_state *st);

/* compare two states; if they differ, describe what differs into `what`. Returns 1 if differ. */
static int diff_compare(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b,
                        char *what, size_t whatlen)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) {
        snprintf(what, whatlen, "D%d: JIT=0x%08X oracle=0x%08X", i, a->d[i], b->d[i]); return 1; }
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) {
        snprintf(what, whatlen, "A%d: JIT=0x%08X oracle=0x%08X", i, a->a[i], b->a[i]); return 1; }
    if ((a->ccr & 0x1F) != (b->ccr & 0x1F)) {
        snprintf(what, whatlen, "CCR: JIT=0x%02X oracle=0x%02X", a->ccr & 0x1F, b->ccr & 0x1F); return 1; }
    if (a->pc != b->pc) {
        snprintf(what, whatlen, "PC: JIT=0x%08X oracle=0x%08X", a->pc, b->pc); return 1; }
    return 0;
}

struct diff_ctx {
    j5n_diag *d;
    struct j5d_m68k_state *ref_st;
    j5d_sandbox *ref_sb;
    uint32_t a6_libbase;
    j5d_lvo_fn ref_lvo; void *ref_user;   /* the ORACLE's bridge (may differ from the JIT's
                                           * — the injection point for the diff test)      */
    int oracle_done;          /* the oracle reached program exit                       */
    uint32_t cur_pc;          /* the oracle's current PC (boundary it is parked at)     */
};

static int diff_boundary_cb(void *user, j5d_sandbox *sb, struct j5d_m68k_state *jit_st,
                            uint32_t next_pc)
{
    struct diff_ctx *c = (struct diff_ctx *)user;
    (void)sb;
    if (c->oracle_done) return 0;

    /* Advance the oracle from its current PC to next_pc (the JIT's next block entry). The
     * oracle stops at next_pc BEFORE executing it (return 2), OR exits the program (return 0).
     * Either way the oracle state then reflects the SAME boundary the JIT is at. */
    char e[160] = {0}; uint32_t rd0 = 0;
    j5d_interp_set_stop_pc(1, next_pc);
    int irc = j5d_interp_run(c->ref_sb, c->cur_pc, c->a6_libbase, c->ref_st, &rd0,
                             c->ref_lvo, c->ref_user, e, sizeof e);
    j5d_interp_set_stop_pc(0, 0);
    if (irc == 0) c->oracle_done = 1;      /* oracle hit program exit before next_pc */
    c->cur_pc = next_pc;

    /* Compare the JIT state at this boundary against the oracle's. */
    char what[128];
    if (diff_compare(jit_st, c->ref_st, what, sizeof what)) {
        j5n_diag *d = c->d;
        d->diverged = 1;
        d->diverge_pc = next_pc;
        if (next_pc >= sb->origin && (uint64_t)(next_pc - sb->origin) + 2 <= sb->size) {
            const uint8_t *ip = sb->host_mem + (next_pc - sb->origin);
            d->diverge_op = (uint16_t)((ip[0] << 8) | ip[1]);
        }
        snprintf(d->diverge_what, sizeof d->diverge_what, "%s", what);
        d->diverge_jit = *jit_st;
        d->diverge_ref = *c->ref_st;
        d->insn_number = j5d_diag_insn_number();
        char detail[160];
        snprintf(detail, sizeof detail,
                 "JIT diverged from the reference interpreter at PC 0x%08X — %s", next_pc, what);
        j5d_fault(J5N_FAULT_DIVERGE, detail, jit_st, sb, NULL);
        return 1;     /* stop the JIT run */
    }
    return 0;
}

int j5d_run_diff(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                 struct j5d_m68k_state *jit_st, struct j5d_m68k_state *ref_st,
                 j5d_sandbox *ref_sb, uint32_t *exit_d0,
                 j5d_lvo_fn jit_lvo, void *jit_user,
                 j5d_lvo_fn ref_lvo, void *ref_user,
                 char *errbuf, unsigned errlen)
{
    j5n_diag *d = j5d_get_diag();
    if (!d) { if (errbuf) snprintf(errbuf, errlen, "diff mode: no diag config registered"); return 1; }

    struct diff_ctx c;
    memset(&c, 0, sizeof c);
    c.d = d; c.ref_st = ref_st; c.ref_sb = ref_sb; c.a6_libbase = a6_libbase;
    c.ref_lvo = ref_lvo; c.ref_user = ref_user; c.cur_pc = entry_pc;

    /* seed the oracle mirror like the JIT (A6 + SP); j5d_interp_run also seeds them. */
    ref_st->a[6] = a6_libbase;

    g_block_boundary_cb = diff_boundary_cb;
    g_block_boundary_user = &c;
    int rc = j5d_run(sb, entry_pc, a6_libbase, jit_st, exit_d0, jit_lvo, jit_user, errbuf, errlen);
    g_block_boundary_cb = NULL;
    g_block_boundary_user = NULL;
    return rc;
}
