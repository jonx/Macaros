/* j5n_diag.h — [J5n] the 68k-JIT DIAGNOSTICS subsystem: faults are never silent (OURS,
 * AROS-licensed). On ANY fault the engine routes through ONE funnel `j5d_fault()` that
 * writes a self-contained, shareable CRASH BUNDLE (a tar.gz with a two-level human report,
 * a reloadable core snapshot, the exact program, a deterministic replay command, and a
 * friendly README) and prints a LOUD banner pointing at it. Plus: a runtime DIFFERENTIAL
 * (lockstep-vs-the-oracle) mode that traps at the first mistranslated instruction, and a
 * deterministic REPLAY-TO-N that re-runs the same program and breaks at instruction #N.
 *
 * Authored from the spec ([J5n] in docs/features/68k-jit/spec.md), the Motorola 68000 ISA,
 * the AArch64 ISA + the macOS ucontext shape (graft/cpu_aarch64.h), and the AmigaOS hunk
 * format. Contains NO Emu68 source — it #includes the quarantined emitter NOWHERE; it only
 * reads struct j5d_m68k_state (the frozen seam) and the sandbox. It does NOT change the
 * frozen seam: it is wired via a side-channel j5d_set_diag() that MIRRORS the existing
 * j5d_set_exc_log(), never touching struct layout, jit_region.h, the [J3] LVO contract, or
 * the [J5i] exception model.
 *
 * ============================== THE BUNDLE (the headline) ==============================
 * <crashdir>/jit68k-crash-<UTCstamp>-<faultkind>.tar.gz containing:
 *   README.txt      friendly plain-English overview — what this is + the one thing to do.
 *   MANIFEST.txt    the precise file index + the one reproduce step.
 *   REPORT.txt      the two-level human report: coordinate (kind/detail/#N/PC/block/depth),
 *                   the faulting 68k instruction (opcode words + disassembly), 68k regs
 *                   (D0-D7/A0-A7/PC/SR), host AArch64 regs (x0-x30/sp/pc), the 68k call
 *                   stack (return-stack walk -> symbols) AND the native host backtrace, and
 *                   the flight recorder (last N (PC,opcode) executed).
 *   core.snapshot   struct j5d_m68k_state + the full raw sandbox image (reloadable).
 *   program.exe     the exact 68k hunk that was running; program.sha256 its digest.
 *   diverge.txt     (differential mode only) the first instruction where JIT != oracle.
 *   REPRODUCE.txt   the deterministic replay command (run-to #N) + git commit + build cfg.
 * ===================================================================================== */
#ifndef J5N_DIAG_H
#define J5N_DIAG_H

#include <stdint.h>
#include <stddef.h>
#include "j5d_jit68k.h"     /* struct j5d_m68k_state, j5d_sandbox (the frozen state) */
#include "j5n_symbols.h"    /* the PC->symbol map for the 68k call stack             */

/* ----- fault kinds (the <faultkind> tag in the bundle name + the report coordinate) ---- */
typedef enum {
    J5N_FAULT_OOB_READ = 0,   /* out-of-sandbox READ                                  */
    J5N_FAULT_OOB_WRITE,      /* out-of-sandbox WRITE                                 */
    J5N_FAULT_BUS,            /* bad PC / jump to nonexistent memory (68k vector 2)   */
    J5N_FAULT_ADDRESS,        /* misaligned PC (68k vector 3)                         */
    J5N_FAULT_ILLEGAL,        /* illegal instruction (68k vector 4)                   */
    J5N_FAULT_DIVZERO,        /* divide by zero (68k vector 5)                        */
    J5N_FAULT_HOST_SIGNAL,    /* a genuine host SIGSEGV/SIGBUS/SIGILL/SIGFPE          */
    J5N_FAULT_DIVERGE,        /* differential mode: JIT != oracle                     */
    J5N_FAULT_ENGINE,         /* a clean dispatcher error (RFAIL)                     */
    J5N_FAULT_KIND_COUNT
} j5n_fault_kind;

const char *j5n_fault_kind_name(j5n_fault_kind k);

/* ----- the host CPU context captured at a fault (mirrors graft/cpu_aarch64.h shape) ---- */
typedef struct {
    int      have;            /* 0 = no host context (a clean in-band 68k fault)      */
    uint64_t x[31];           /* x0..x30 (x29=fp, x30=lr)                            */
    uint64_t sp, pc;          /* host stack pointer + program counter                */
    uint32_t cpsr;
} j5n_hostregs;

/* ----- the flight recorder: a ring of the last N executed (PC, opcode) ------------- */
#define J5N_FLIGHT_N 64
typedef struct {
    uint32_t pc[J5N_FLIGHT_N];
    uint16_t op[J5N_FLIGHT_N];
    uint64_t head;            /* total pushed (head % N = next slot)                  */
} j5n_flight;

/* ----- the diagnostics configuration (set by j5d_set_diag; NULL = the fast path) -----
 * Carries everything the funnel needs. The engine + the interp read it; the test builds it.
 * It lives OUTSIDE struct j5d_m68k_state (the frozen seam) — a side-channel exactly like the
 * existing j5i_exc_log. */
typedef struct j5n_diag {
    /* the program + sandbox (for the snapshot + program.exe + the bundle). */
    const uint8_t *prog;          /* the exact hunk file bytes that were loaded       */
    size_t         prog_len;
    j5d_sandbox   *sb;            /* the live sandbox (host_mem + origin + size)       */
    uint32_t       entry_pc;      /* the program's entry PC (for REPRODUCE)            */
    uint32_t       a6_libbase;    /* the A6 library base used for the run              */
    const j5n_symtab *symtab;     /* the PC->symbol map (may be empty)                 */

    /* where to write bundles. NULL -> $JIT68K_CRASH_DIR or <cwd>/crash. */
    const char    *crash_dir;

    /* the flight recorder + the deterministic instruction counter (engine-owned, here so
     * the funnel can read them). insn_number is bumped once per dispatched 68k insn. */
    j5n_flight     flight;
    uint64_t       insn_number;

    /* ----- the differential ("lockstep") mode (JIT68K_DIFF=1) -----
     * When diff_enabled, the engine runs the interpreter oracle in lockstep over a MIRROR
     * state + sandbox and traps at the first instruction where they diverge. The mirror is
     * owned by the engine's run; these fields are the configuration + the captured result. */
    int            diff_enabled;
    /* result (filled on a divergence): */
    int            diverged;
    uint32_t       diverge_pc;        /* the 68k PC of the first diverging instruction */
    uint16_t       diverge_op;        /* its opcode word                               */
    char           diverge_what[128]; /* a short description of what diverged          */
    struct j5d_m68k_state diverge_jit; /* JIT state at the divergence                  */
    struct j5d_m68k_state diverge_ref; /* oracle state at the divergence               */

    /* ----- replay-to-N (JIT68K_RUNTO=N) -----
     * When runto_enabled, the run BREAKS exactly when insn_number == runto_n, landing on the
     * crash coordinate deterministically. runto_hit is set when it lands. */
    int            runto_enabled;
    uint64_t       runto_n;
    int            runto_hit;
    uint32_t       runto_pc;          /* the PC the run was at when it hit #N           */

    /* test bookkeeping: where the last bundle landed + how many bundles written. */
    char           last_bundle[1024];
    int            bundles_written;
    int            quiet_banner;      /* 1 = suppress the LOUD banner (test noise control)*/
} j5n_diag;

/* Register the diagnostics config with the engine + the interp oracle. NULL = disable
 * (the whole existing corpus runs on this NULL fast path). Mirrors j5d_set_exc_log. */
void j5d_set_diag(j5n_diag *d);
void j5d_interp_set_diag(j5n_diag *d);   /* the oracle side (instruction stepping)      */
j5n_diag *j5d_get_diag(void);            /* engine reads its own config                  */

/* Initialise a config from the environment (JIT68K_CRASH_DIR, JIT68K_DIFF, JIT68K_RUNTO)
 * plus the explicit program/sandbox/symtab. Zeroes result fields. */
void j5n_diag_init(j5n_diag *d, const uint8_t *prog, size_t prog_len, j5d_sandbox *sb,
                   uint32_t entry_pc, uint32_t a6_libbase, const j5n_symtab *symtab);

/* ----- THE FUNNEL. Every fault path routes here. Writes the bundle, prints the banner.
 * `detail` is the human one-liner ("out-of-sandbox WRITE to 0x.. ; sandbox 0x..0x..").
 * `host` may be NULL (a clean in-band fault). `st`/`sb` are the faulting 68k state + sandbox.
 * Returns the absolute bundle path (into d->last_bundle), or NULL on a write failure. */
const char *j5d_fault(j5n_fault_kind kind, const char *detail,
                      const struct j5d_m68k_state *st, j5d_sandbox *sb,
                      const j5n_hostregs *host);

/* ----- the flight recorder push (the engine calls it per dispatched instruction). ----- */
void j5n_flight_push(j5n_diag *d, uint32_t pc, uint16_t op);

/* ----- record a whole block's body instructions into the flight recorder (the engine calls
 * it per block, since the JIT executes per block). Walks [pc, end_pc) decoding instruction
 * boundaries via a minimal length table; pushes (pc, opcode) for each. ----- */
void j5n_diag_record_block(j5n_diag *d, j5d_sandbox *sb, uint32_t pc, uint32_t end_pc);

/* ----- the host-signal safety net. Installs SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers on a
 * sigaltstack so a genuine host fault in translated code is recovered (host mcontext +
 * the current 68k state) and bundled via j5d_fault — never a silent host crash. The engine
 * sets the "current 68k state" via j5n_signal_set_context before entering a block. ----- */
void j5n_signal_install(j5n_diag *d);
void j5n_signal_remove(void);
void j5n_signal_set_context(const struct j5d_m68k_state *st, j5d_sandbox *sb);

/* ----- the per-instruction diagnostics hook (the interp oracle calls it at its loop top).
 * It owns the deterministic instruction counter, the flight recorder, replay-to-N break,
 * and (in diff mode) the lockstep compare. Returns 0 to continue, nonzero to STOP the run
 * (a break-to-N landing or a divergence trap already bundled). ----- */
int  j5n_diag_step(j5n_diag *d, const struct j5d_m68k_state *st, j5d_sandbox *sb,
                   uint32_t pc, uint16_t op);

/* ----- snapshot load/inspect (the optional reload mode). Reads a core.snapshot written by
 * the bundle writer and fills `st` + a freshly-allocated sandbox image. Returns 0 on
 * success. `*image`/`*image_len` receive a malloc'd copy of the raw sandbox memory. ----- */
int  j5n_snapshot_load(const char *path, struct j5d_m68k_state *st,
                       uint8_t **image, size_t *image_len, uint32_t *origin);

/* A minimal 68k disassembler for the report (mnemonic + operands for the common set;
 * falls back to "dc.w $XXXX" for the unrecognised). Writes into `out`. */
void j5n_disasm(const uint8_t *code, uint32_t pc, uint32_t origin, uint32_t size,
                char *out, size_t outlen);

/* ----- engine-side [J5n] entry points (implemented in j5d_engine.c; declared here so the
 * frozen seam header j5d_jit68k.h stays untouched). These are side-channels exactly like
 * j5d_set_exc_log — they add no struct/contract change. ----- */

/* The deterministic global 68k instruction number the engine is at (the crash #N). */
uint64_t j5d_diag_insn_number(void);

/* The DIFFERENTIAL (lockstep) run: run the JIT and the oracle interpreter in lockstep and
 * trap at the first instruction where they diverge (filling the registered diag's diverge_*
 * + writing the bundle). `jit_st` runs the JIT over `sb`; `ref_st`/`ref_sb` are the oracle's
 * identically-loaded mirror. Returns the j5d_run contract (0 ok / 1 error); *exit_d0 = JIT d0.
 * Requires a diag config registered via j5d_set_diag (with diff fields). */
int  j5d_run_diff(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                  struct j5d_m68k_state *jit_st, struct j5d_m68k_state *ref_st,
                  j5d_sandbox *ref_sb, uint32_t *exit_d0,
                  j5d_lvo_fn jit_lvo, void *jit_user,
                  j5d_lvo_fn ref_lvo, void *ref_user,
                  char *errbuf, unsigned errlen);

/* The interp's [J5n] side-channels (implemented in j5d_interp.c). */
void j5d_interp_set_stop_pc(int active, uint32_t pc);

#endif /* J5N_DIAG_H */
