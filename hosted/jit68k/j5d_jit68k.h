/* j5d_jit68k.h — [J5d] boundary types: BROADEN the [J5c] re-hosting so the WHOLE
 * apps68k corpus runs through the JIT. [J5c] proved Emu68's REAL per-opcode decoders
 * re-host for one straight-line register/ALU block; [J5d] turns that into a real
 * little engine: a per-basic-block translator driving the REAL decoders for every
 * data/ALU/memory/move opcode, plus OUR re-hosted dispatcher ("MainLoop") that owns
 * inter-block control flow, the (An)/(An)+ sandbox-memory EA, and the jsr-through-
 * vector -> [J3] library bridge. (OURS, AROS-licensed.)
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J5]/[J5c]/[J5d]),
 * the Motorola 68000 ISA, the AArch64 ISA, and the AROS m68k LVO ABI only. This header
 * contains NO Emu68 source — no Emu68 type, macro, or declaration is copied here.
 *
 * ===================== WHAT [J5d] ADDS OVER [J5c] (the broadening) ==============
 * [J5c] drove the real decoders for ONE straight-line block (no memory, no control
 * flow). [J5d] keeps that machinery (j5c_shims.c HOOK 2 fetch + PC/SR shims;
 * j5c_ra.c HOOK 3 memory-backed RA) and ADDS:
 *
 *  (1) PER-BASIC-BLOCK TRANSLATION + OUR DISPATCHER ("MainLoop", j5d_engine.c). The
 *      spec's section (3) says the inter-block PC funnel + the LVO bridge are OURS,
 *      provided BEHIND the translator; Emu68's own model exits each block via RET to
 *      a central C MainLoop that re-reads the m68k PC. [J5d] implements exactly that:
 *      translate a straight-line run of opcodes (driving the REAL decoders) up to a
 *      control-flow terminator, run it under W^X, then in C decode the terminator
 *      (Bcc/BRA/JSR/RTS) to compute the next PC and loop. The heavy SEMANTIC work —
 *      every register/ALU/flag/memory opcode — is done by the REAL Emu68 decoders;
 *      our dispatcher only owns block boundaries + the branch decision (read from the
 *      REAL CCR the real decoders produced) + the LVO bridge. A per-PC translation
 *      cache (the [J5d] ICache, OURS) means each block is translated once.
 *
 *  (2) THE (An)/(An)+ SANDBOX-MEMORY EA (HOOK 1, the [J5a] blocker, now bridged). The
 *      REAL Emu68 EA decoder (M68k_EA.c) emits `ldr/str [reg_An]` treating An as a raw
 *      host pointer (1:1 MMU) AND assumes a big-endian CPU. [J5d] applies the disclosed
 *      [J5a] fix to the build-dir COPY of M68k_EA.c via emu68_darwinize.pl: every
 *      (An)-class load/store site is rewritten to call OUR j5d_ea_mem emitter
 *      (j5d_ea_helpers.c), which computes the host pointer = sandbox_base_adjust + An
 *      (UXTW, from a fixed callee-saved scratch reg the prologue seeds) and byteswaps
 *      with REV. The QUARANTINE M68k_EA.c stays BYTE-VERBATIM (diff vs upstream empty);
 *      only the build-dir copy is patched. This is the [J5a] surgery the [J5c] verdict
 *      named, realised as a darwinize transform rather than a hand edit of the file.
 *
 *  (3) jsr-through-vector -> the [J3] bridge FROM THE DECODED STREAM (libcall). The
 *      dispatcher decodes `jsr d16(A6)` (0x4EAE), maps the target to (libbase, LVO n)
 *      via the REAL negative-offset rule n=(libbase-target)/6 (j3_vector_recognise),
 *      and invokes the registered library handler — which marshals the 68k regs into
 *      the native stub through the REAL [J3] marshaller (j3_build_marshal_thunk). The
 *      jsr is DECODED from the instruction stream, not hand-constructed.
 *
 * Control-flow + address-compute opcodes the DISPATCHER decodes itself (these are the
 * LINE4/5/6 entry points whose Emu68 decoders are entangled with M68k_Translator.c's
 * REG_PC/branch-target model — re-hosting that funnel is the larger, separately-scoped
 * adoption beyond [J5d]; our dispatcher IS that funnel for the corpus):
 *   bne.s/bra.s (Bcc/BRA, 8-bit) ; jsr d16(A6) ; lea abs.l,An ; rts.
 * Everything else (moveq, move.l/movea.l reg/imm, add/sub/and/or/eor/cmp/muls, addq/
 * subq, add.l (An)+) is translated by the REAL Emu68 decoders.
 * ===============================================================================
 */
#ifndef J5D_JIT68K_H
#define J5D_JIT68K_H

#include <stdint.h>
#include <stddef.h>

/* ----- The hosted 68k machine state -------------------------------------------------
 * Same field layout as the [J5c]/[J3] state (offsets are load-bearing: the emitted
 * prologue/epilogue and j5c_ra.c's CCR hook + the [J3] marshaller all use them). */
struct j5d_m68k_state {
    uint32_t d[8];     /* D0..D7 : byte off  0.. 28 */
    uint32_t a[8];     /* A0..A7 : byte off 32.. 60 */
    uint32_t ccr;      /* CCR    : byte off 64       (low byte = 68k CCR)            */
    uint32_t pc;       /* PC     : byte off 68                                       */
};

#define J5D_OFF_D(n)   ((uint16_t)((n) * 4u))
#define J5D_OFF_A(n)   ((uint16_t)(32u + (n) * 4u))
#define J5D_OFF_CCR    ((uint16_t)64u)
#define J5D_OFF_PC     ((uint16_t)68u)

/* CCR bit positions — EMU68's INTERNAL representation (a [J5g] finding). Emu68's
 * memory-backed CCR (RA_StoreCC writes the byte the decoders' EMIT_GetNZCV / EMIT_GetNZxx
 * produced, A64.h) uses the m68k SR bit order for N/Z/X but SWAPS C and V:
 *     bit0 = V , bit1 = C , bit2 = Z , bit3 = N , bit4 = X
 * (cf. SRB_Calt=1 / SRB_Valt=0 in M68k.h, and bfi(cc,..,1,1) for C / bfi(cc,..,0,1) for V
 * in A64.h EMIT_GetNZCV). The earlier corpus only ever consumed Z/N (layout-stable), so
 * this swap was latent; [J5g]'s ble.s/blt.s/bcc/bcs branches consume C and V, so the
 * dispatcher's bcc_taken + the independent oracle MUST both use Emu68's actual layout for
 * the byte-exact CCR check and the branch decisions to agree. The register RESULTS (the
 * real proof) are still byte-exact-verified; only the CCR bit POSITIONS for C/V follow
 * Emu68's storage. (A future normalisation pass could re-emit a standard-68k SR byte at
 * the SR-read boundary; not needed while the CCR is internal to the JIT+oracle.) */
#define J5D_CCR_V 0x01u
#define J5D_CCR_C 0x02u
#define J5D_CCR_Z 0x04u
#define J5D_CCR_N 0x08u
#define J5D_CCR_X 0x10u

/* ----- The sandbox (carried from [J5a]/[J4]) --------------------------------------- */
typedef struct j5d_sandbox {
    uint8_t  *host_mem;   /* host pointer to 68k address `origin`                 */
    uint32_t  origin;     /* 68k-space base address that host_mem[0] represents   */
    uint32_t  size;       /* sandbox size in bytes                                */
} j5d_sandbox;

/* The sandbox base-adjust the (An) EA helper adds to a 68k address to get a host
 * pointer:  host = base_adjust + An (UXTW). = host_mem - origin. The engine seeds a
 * fixed callee-saved AArch64 register with this before running each block; the
 * darwinize-rewritten M68k_EA.c (An)-sites read it via j5d_ea_mem. */

/* ----- The library bridge callback ------------------------------------------------
 * Invoked by the dispatcher when a decoded `jsr d16(A6)` targets a registered library
 * vector. The callback marshals the 68k regs (in *st) into the native stub through the
 * [J3] marshaller and writes any return into st->d[0]. Return 0 = handled, nonzero =
 * error (errbuf set). `lvo` is the positive LVO index n recovered by the negative-
 * offset rule. */
typedef int (*j5d_lvo_fn)(int lvo, struct j5d_m68k_state *st, void *user,
                          char *errbuf, unsigned errlen);

/* ----- The engine (j5d_engine.c) --------------------------------------------------
 * Run the 68k program from `entry_pc` through the JIT: translate each basic block via
 * the REAL Emu68 decoders into a MAP_JIT region, run it under W^X, then decode the
 * terminator to find the next block, until the top-level RTS. `a6_libbase` seeds A6
 * (the library base for jsr d16(A6)); pass 0 if the program makes no library calls.
 * `lvo`/`user` register the library bridge (NULL if none). On success *exit_d0 = the
 * final 68k D0. Returns 0 on success, nonzero on error (errbuf set).
 *
 * `g_j5d_trace` (file-scope in the engine) optionally records each translated block's
 * AArch64 word count + the terminator kind for the test's "ran through the JIT" proof. */
int j5d_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
            struct j5d_m68k_state *st, uint32_t *exit_d0,
            j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen);

/* Free every MAP_JIT region the engine allocated (the per-PC block cache). */
void j5d_run_free(void);

/* Engine stats for the test's assertion that blocks really went through the JIT. */
typedef struct j5d_stats {
    uint32_t blocks_translated;   /* distinct basic blocks emitted to AArch64        */
    uint32_t blocks_executed;     /* block executions (>= translated, loops re-run)  */
    uint32_t insns_decoded;       /* m68k opcodes the REAL decoders translated        */
    uint32_t arm_words_emitted;   /* total AArch64 words written to MAP_JIT           */
    uint32_t lib_calls;           /* jsr-through-vector library calls bridged          */
    uint32_t mem_accesses;        /* (An)-class sandbox memory accesses the JIT ran    */
    /* [J5f] PC-driven dispatch + real return stack + block cache. */
    uint32_t block_cache_hits;    /* block lookups that hit a cached translation        */
    uint32_t block_cache_misses;  /* block lookups that translated (== blocks_translated)*/
    uint32_t calls_pushed;        /* bsr/jsr-to-code -> 68k return addresses pushed      */
    uint32_t returns_popped;      /* rts -> 68k return addresses popped (incl. nested)   */
    uint32_t computed_jumps;      /* jmp(An)/jsr(An) computed-target control transfers   */
    uint32_t max_call_depth;      /* deepest nested call depth reached (return-stack)    */
    /* [J5e] register-allocator (block-scoped reg file) before/after metrics. The naive
     * scheme loaded all 16 Dn/An in the prologue + stored all 16 back in the epilogue,
     * unconditionally: 32 state-struct ldr/str per block. The [J5e] RA loads only live-in
     * regs + stores only dirty regs. These count the per-program totals over all blocks. */
    uint32_t state_ldrstr_naive;  /* 32 * blocks_translated (the old fixed frame cost)  */
    uint32_t state_ldrstr_ra;     /* sum over blocks of (live-in loads + dirty stores)  */
    uint32_t reg_loads_emitted;   /* prologue Dn/An loads emitted (live-in only)        */
    uint32_t reg_stores_emitted;  /* epilogue Dn/An stores emitted (dirty only)         */
} j5d_stats;
void j5d_get_stats(j5d_stats *out);

/* ----- The independent reference (j5d_interp.c, OURS — no Emu68) -------------------
 * Execute the SAME program from scratch (no Emu68) over its own state + the SAME
 * sandbox (memory effects real) + the SAME library bridge, so the test can assert the
 * JITed register file / sandbox memory / observed library-call args+returns are
 * byte-exact equal to this reference. */
int j5d_interp_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                   struct j5d_m68k_state *st, uint32_t *exit_d0,
                   j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen);

#endif /* J5D_JIT68K_H */
