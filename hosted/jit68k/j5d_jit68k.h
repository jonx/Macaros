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
    /* [J5i] the rest of the SR (above the CCR byte) + the exception bookkeeping. These
     * sit AFTER pc so EVERY existing offset (D/A/CCR/PC) is byte-for-byte unchanged —
     * the emitted prologue/epilogue + the (An) EA helpers reference only J5D_OFF_D/A/
     * CCR/PC, none of which move. The JIT'd code never touches these fields; the C
     * dispatcher owns the SR-high byte + the exception state (the spec's "68k exceptions
     * handled in C", §Architecture (2) "no host vectors"). */
    uint16_t sr_high;  /* SR bits 8..15 : the SUPERVISOR (S, bit13) + trace/int-mask. The
                          full SR = (sr_high << 8) | (ccr & 0x1F)  — but note the CCR byte
                          here is Emu68's INTERNAL bit layout (see J5D_CCR_* below), so the
                          architectural 68k SR is reconstructed by j5d_pack_sr().          */
    uint16_t exc_count;/* [J5i] number of exceptions dispatched so far (test bookkeeping)  */
};

#define J5D_OFF_D(n)   ((uint16_t)((n) * 4u))
#define J5D_OFF_A(n)   ((uint16_t)(32u + (n) * 4u))
#define J5D_OFF_CCR    ((uint16_t)64u)
#define J5D_OFF_PC     ((uint16_t)68u)

/* ----- [J5i] the SR (status register) supervisor / trace / interrupt-mask bits --------
 * Modeled in sr_high (SR bits 8..15). The S bit (supervisor) is the one with real
 * semantics here: set on every exception entry, saved in the pushed frame, restored by
 * rte. (M68000 PRM §1.3.2 "Status Register"; the high byte is the system byte.) */
#define J5D_SR_S       0x2000u   /* bit 13: Supervisor state                            */
#define J5D_SR_T       0x8000u   /* bit 15: Trace (modeled as a stored bit; not traced)  */
#define J5D_SR_IMASK   0x0700u   /* bits 8..10: interrupt priority mask (stored only)    */

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
    /* [J5i] the exception / SR model. */
    uint32_t exceptions_dispatched; /* 68k exceptions vectored (TRAP/div0/illegal/bus)  */
    uint32_t rte_returns;           /* rte instructions executed (frame popped, resumed) */
} j5d_stats;
void j5d_get_stats(j5d_stats *out);

/* ===================== [J5i] THE 68k EXCEPTION / SR MODEL ==========================
 * A bounded, dispatcher-level (C) 68k exception model — the spec's "68k exceptions
 * handled in C" (Architecture §(2): we re-host Emu68's machine-owning runtime, so there
 * is NO host VBAR and Emu68's bare-metal EMIT_Exception/VBR path is a no-op stub here;
 * the exception dispatch is OURS, in the dispatcher, exactly like the inter-block PC
 * funnel + the LVO bridge).
 *
 * WHAT WE MODEL (and what is deferred — honest scope, also in spec [J5i]):
 *   - A 256-longword 68k VECTOR TABLE in the sandbox at a fixed base (J5I_VBR). The tested
 *     vectors are populated with a tiny 68k STUB HANDLER (built into the test program);
 *     the dispatcher reads handler[vector] as a big-endian longword and jumps there.
 *   - The standard short-frame EXCEPTION FRAME: on entry the dispatcher pushes (to a7,
 *     the supervisor stack — we DEFER the USP/SSP split, see below) the 16-bit SR then the
 *     32-bit return PC, big-endian, predecrement (a7 -= 6). It sets S in sr_high and jumps
 *     to the vector's handler. This is the 68000 (format-less) 6-byte frame: SR @ (a7),
 *     PC @ (a7+2). (68010+ add a 2-byte format/vector word — DEFERRED.)
 *   - rte (0x4E73): pop SR (16-bit) then PC (32-bit) big-endian, a7 += 6, restore S from
 *     the popped SR, resume at the popped PC.
 *   - The S (supervisor) bit: set on entry, saved/restored via the frame.
 *
 * DEFERRED (stated): the real VBR register (we use a fixed sandbox base), the USP/SSP
 * split (one a7 — supervisor and user share it; fine for self-contained programs that do
 * not switch modes mid-exception), all 68010+ frame formats (format/vector word, the long
 * bus/address-error frame), group-0 vs group-1/2 exception priorities (we dispatch the one
 * faulting instruction; no nesting-priority arbitration), and the actual host-SIGSEGV ->
 * this-path wiring (that lands at AROS integration via graft/cpu_aarch64.h — see below).
 *
 * THE graft/cpu_aarch64.h INTEGRATION SEAM. In a JIT integrated into AROS, a genuinely
 * wild 68k access (a translated ldr/str off a corrupt An) faults the HOST with SIGSEGV/
 * SIGBUS. graft/cpu_aarch64.h's SAVEREGS/RESTOREREGS + struct ExceptionContext bridge that
 * host signal into AROS's trap machinery at the AArch64 level. The 68k-level layer here is
 * the PIECE THAT PAIRS WITH IT: the host handler recovers the faulting m68k PC (Emu68 keeps
 * it in x18 at a block boundary; our hosted blocks keep st->pc) and the access kind, then
 * calls j5d_raise_exception() to build the 68k frame + vector through THIS model. The seam
 * is: { host SIGSEGV in translated code } --(graft/cpu_aarch64.h SAVEREGS)--> { recover m68k
 * PC + fault address } --> j5d_raise_exception(BUS/ADDRESS) --> { 68k vector dispatch }.
 * In THIS spike the sandbox bounds-check raises the SAME j5d_raise_exception path WITHOUT a
 * real host SIGSEGV (the clean-fault [J5a] detection turned into a real 68k dispatch), so
 * the 68k model is exercised end-to-end while the host-signal wiring stays an AROS-side
 * integration task — documented, not faked. */

/* The 68k vector numbers we cover (vector NUMBER; byte offset = number*4). */
#define J5I_VEC_BUS_ERROR        2u    /* spurious/bus access fault   (group 0)          */
#define J5I_VEC_ADDRESS_ERROR    3u    /* misaligned / bad address    (group 0)          */
#define J5I_VEC_ILLEGAL          4u    /* illegal instruction         (group 1)          */
#define J5I_VEC_DIV_BY_ZERO      5u    /* divu/divs by zero           (group 2)          */
#define J5I_VEC_TRAP_BASE        32u   /* TRAP #n -> vector 32+n      (group 2)          */

/* The sandbox-space base of the 256-longword vector table (our fixed stand-in for the VBR;
 * the real VBR register is deferred). The test program plants handler addresses here at
 * runtime (lea handler(pc),An ; move.l An, vbr+vec*4). It sits ABOVE the code/data the
 * loader bump-allocates from the sandbox origin (0x00210000), so it never collides with the
 * program. 256 longwords = 0x400 bytes. */
#define J5I_VBR                  0x00240000u
#define J5I_VECTOR_COUNT         256u

/* Reason codes j5d_raise_exception accepts (it maps cause -> vector + frame). */
typedef enum {
    J5I_CAUSE_TRAP = 0,    /* arg = trap number n (vector 32+n)                          */
    J5I_CAUSE_DIVZERO,     /* vector 5                                                   */
    J5I_CAUSE_ILLEGAL,     /* vector 4                                                   */
    J5I_CAUSE_BUS,         /* vector 2 (out-of-sandbox access / corrupt PC)              */
    J5I_CAUSE_ADDRESS,     /* vector 3 (misaligned address)                              */
} j5i_cause;

/* A record of one dispatched exception (the test asserts these against the oracle). */
typedef struct {
    uint8_t  vector;       /* the 68k vector NUMBER it dispatched to                     */
    uint32_t frame_sr;     /* the 16-bit SR pushed in the frame (architectural 68k SR)   */
    uint32_t frame_pc;     /* the 32-bit return PC pushed in the frame                   */
    uint32_t a7_at_entry;  /* a7 AFTER the 6-byte frame push (the frame's address)       */
    uint32_t handler_pc;   /* the handler address read from the vector table             */
} j5i_exc_record;

#define J5I_MAX_EXC 32
/* The exception log the engine + the oracle each fill. Reset at the start of each run. */
typedef struct {
    int            n;
    j5i_exc_record rec[J5I_MAX_EXC];
} j5i_exc_log;

/* Engine + oracle write their per-run exception log here when the caller supplies one.
 * Pass NULL if the program raises no exceptions (the whole existing corpus). The log is
 * part of the j5d_run / j5d_interp_run signatures via j5d_set_exc_log (below) to keep the
 * existing call sites byte-for-byte unchanged. */
void j5d_set_exc_log(j5i_exc_log *log);          /* engine  (j5d_engine.c) */
void j5d_interp_set_exc_log(j5i_exc_log *log);   /* oracle  (j5d_interp.c) */

/* Reconstruct the architectural 68k SR (CCR low byte in standard order + the system byte)
 * from the internal state. Defined in j5d_engine.c; the oracle uses the same packer so the
 * pushed-frame SR is identical. */
uint16_t j5d_pack_sr(const struct j5d_m68k_state *st);

/* ----- The independent reference (j5d_interp.c, OURS — no Emu68) -------------------
 * Execute the SAME program from scratch (no Emu68) over its own state + the SAME
 * sandbox (memory effects real) + the SAME library bridge, so the test can assert the
 * JITed register file / sandbox memory / observed library-call args+returns are
 * byte-exact equal to this reference. */
int j5d_interp_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                   struct j5d_m68k_state *st, uint32_t *exit_d0,
                   j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen);

#endif /* J5D_JIT68K_H */
