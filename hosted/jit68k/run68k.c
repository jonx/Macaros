/* run68k.c — a real command-line tool that RUNS a self-contained 68k Amiga hunk
 * executable through the JIT (CPU + FPU) from the macOS terminal, piping the
 * program's output to stdout.  (OURS, AROS-licensed.)
 *
 * Clean-room / OURS.  This is a USABILITY WRAPPER over the existing engine — it adds
 * NO emulation.  It composes the already-proven pieces:
 *   - the [J4] hunk loader + relocator (j4_loader.c / j4_hunk.h)
 *   - the stub OS (stublib.[ch]): AllocMem / FreeMem / PutChar over the [J3] LVO bridge
 *   - the full [J5d] JIT engine (j5d_engine.c): per-block translation driving Emu68's
 *     REAL decoders (integer LINE0..E + the LINEF FPU), block chaining, the [J5i]
 *     exception model, the [J3] library bridge
 *   - the [J5n] crash-bundle diagnostics (j5n_diag.[ch] / j5n_symbols.[ch]): on a fault
 *     it writes a shareable tar.gz bundle and prints a "send this file" banner
 *
 * It is a FRONT-END: it does not touch the frozen seam (struct j5d_m68k_state), the
 * engine internals, jit_region.h, the [J3] LVO contract, or the [J5i] model.  It only
 * CALLS the public entry points (j5d_run / j5d_run_diff / j5d_interp_run, j4_load_hunks,
 * stublib_*, j5d_set_diag / j5n_signal_install).  Contains NO Emu68 source.
 *
 * ============================ THE CONTRACT (the CLI behavior) =========================
 *   run68k [options] <program.exe> [program-args...]
 *
 *   - The PROGRAM's PutChar output goes to STDOUT (the terminal), cleanly — nothing else
 *     of ours lands on stdout, so it pipes.  All of the tool's own chatter (loading
 *     errors, the crash banner, the -v stats) goes to STDERR.
 *   - The EXIT CODE is the program's exit code: the 68k D0 at the top-level RTS, clamped
 *     to a shell-meaningful 0..255 (full D0 also reported under -v).  So it composes in
 *     pipelines:  run68k prog.exe | grep ... ; echo $?
 *   - On a FAULT (out-of-sandbox access, bad PC, illegal instruction, div-by-zero, a host
 *     signal in translated code): the [J5n] diagnostics write a crash bundle and print the
 *     "send this file" banner to stderr; run68k exits nonzero.
 *   - GRACEFUL ERRORS (no raw crash): file-not-found, not-a-hunk (bad magic), an
 *     unsupported/undecodable opcode -> a clear one-line stderr message, nonzero exit.
 * ===================================================================================== */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "j5n_diag.h"
#include "j5n_symbols.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* The sandbox layout — identical to the corpus harness so every committed program loads
 * and runs exactly as it does under the regression tests. */
#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00040000u     /* 256 KiB: code + data + lib table + heap */
#define LIBBASE         0x00230000u     /* stub library base (vectors grow downward) */
#define HEAP_BASE       0x00231000u     /* AllocMem heap                            */
#define HEAP_END        0x00238000u

#define MAX_PROG_BYTES  (1u << 20)      /* 1 MiB cap on a hunk file (generous)      */

/* ---- The library bridge: marshal 68k regs into the native stub via the [J3] thunk. ---- */
struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

static void usage(FILE *f, const char *argv0)
{
    fprintf(f,
"usage: %s [options] <program.exe> [program-args...]\n"
"\n"
"Run a self-contained 68k Amiga hunk executable through the JIT (CPU + FPU) and\n"
"pipe its output to stdout.  The program's exit code becomes run68k's exit code.\n"
"\n"
"options:\n"
"  -h, --help        show this help and exit\n"
"  -v                verbose: print engine stats (blocks/insns/cache/FP) to stderr\n"
"  --diff            request the [J5n] differential lockstep checker.  NOTE: the engine's\n"
"                    whole-program lockstep driver reports block-boundary false positives on\n"
"                    multi-block programs, so run68k prints a notice and runs normally.  The\n"
"                    real JIT-vs-interpreter check is the regression (make hosted-jit68k-j5m\n"
"                    / -j5t); the single-divergence locator is make hosted-jit68k-j5n.\n"
"  --crash-dir DIR   write crash bundles to DIR (default: $JIT68K_CRASH_DIR or ./crash)\n"
"\n"
"output / exit code:\n"
"  The program's PutChar output goes to STDOUT (clean, pipe-able).  run68k's own\n"
"  messages (errors, the crash banner, -v stats) go to STDERR.  The exit code is the\n"
"  program's top-level D0 (clamped 0..255), so it composes in shell pipelines.\n"
"\n"
"what it can run:\n"
"  Self-contained / stub-OS 68k programs (integer + 68881/68882 hardware FP), incl.\n"
"  vbcc-compiled C.  The stub OS provides AllocMem/FreeMem/PutChar via the [J3] bridge.\n"
"what it can't run YET:\n"
"  Programs that call real AmigaOS/AROS libraries (needs the AROS integration), and\n"
"  hardware-banging games (needs a full-chipset emulator like UAE).\n"
"\n"
"program-args:  ACCEPTED but NOT YET passed into the 68k program.  Passing an Amiga\n"
"  CLI argument string into the program (via the crt0/stub) is a follow-on; the args\n"
"  are parsed off the command line here but do not reach the program.\n",
        argv0);
}

/* Read a whole file into a malloc'd buffer.  Returns NULL on error (msg to stderr). */
static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "run68k: cannot open '%s': %s\n", path, strerror(errno)); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fprintf(stderr, "run68k: seek '%s' failed\n", path); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fprintf(stderr, "run68k: tell '%s' failed\n", path); fclose(f); return NULL; }
    if (sz == 0) { fprintf(stderr, "run68k: '%s' is empty (not a hunk file)\n", path); fclose(f); return NULL; }
    if ((size_t)sz > MAX_PROG_BYTES) {
        fprintf(stderr, "run68k: '%s' is %ld bytes (> %u-byte cap)\n", path, sz, MAX_PROG_BYTES);
        fclose(f); return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fprintf(stderr, "run68k: out of memory reading '%s'\n", path); fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { fprintf(stderr, "run68k: short read on '%s'\n", path); free(buf); return NULL; }
    *len_out = got;
    return buf;
}

int main(int argc, char **argv)
{
    const char *prog_path = NULL;
    const char *crash_dir = NULL;
    int verbose = 0;
    int diff    = 0;

    /* ---- argument parsing: options up to the program name; everything after it is the
     * program's own argv (accepted, documented as not-yet-passed). ---- */
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || strcmp(a, "-") == 0) break;     /* the program path */
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout, argv[0]); return 0; }
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "--diff")) diff = 1;
        else if (!strcmp(a, "--crash-dir")) {
            if (i + 1 >= argc) { fprintf(stderr, "run68k: --crash-dir needs a DIR argument\n"); return 2; }
            crash_dir = argv[++i];
        }
        else if (!strcmp(a, "--")) { i++; break; }         /* end of options */
        else { fprintf(stderr, "run68k: unknown option '%s' (try --help)\n", a); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "run68k: no program given\n"); usage(stderr, argv[0]); return 2; }
    prog_path = argv[i];
    int prog_argc = argc - i - 1;                          /* the 68k program's own args */
    (void)prog_argc;                                       /* accepted; not yet passed in */

    /* ---- load the file ---- */
    size_t prog_len = 0;
    uint8_t *prog = slurp(prog_path, &prog_len);
    if (!prog) return 1;

    /* ---- load + relocate into a fresh sandbox ---- */
    uint8_t *mem = calloc(1, SANDBOX_SIZE);
    if (!mem) { fprintf(stderr, "run68k: out of memory (sandbox)\n"); free(prog); return 1; }
    j4_sandbox sb; j4_seglist seg; char err[256] = {0};
    j4_sandbox_init(&sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    if (j4_load_hunks(&sb, prog, prog_len, /*skip_reloc=*/0, &seg, err, sizeof err)) {
        fprintf(stderr, "run68k: cannot load '%s': %s\n", prog_path, err);
        free(mem); free(prog); return 1;
    }

    /* ---- parse the symbol table (optional; names the crash call stack) ---- */
    static j5n_symtab symtab;
    j5n_symbols_parse(prog, prog_len, &seg, &symtab);

    /* ---- set up the stub OS ---- */
    stub_lib lib;
    if (stublib_init(&lib, &sb, LIBBASE, HEAP_BASE, HEAP_END)) {
        fprintf(stderr, "run68k: stub library init failed\n");
        free(mem); free(prog); return 1;
    }
    struct bctx c = { &lib, &sb };

    /* ---- wire the [J5n] diagnostics: a fault writes a bundle + prints the banner ---- */
    j5d_sandbox j5sb = { sb.host_mem, sb.sandbox_origin, sb.size };
    static j5n_diag diag;
    j5n_diag_init(&diag, prog, prog_len, &j5sb, seg.entry, LIBBASE, &symtab);
    if (crash_dir) diag.crash_dir = crash_dir;             /* --crash-dir overrides the env */
    /* Suppress the engine's per-funnel banner: a single fault can route through several
     * internal funnels (e.g. an illegal that vectors to a handler that itself faults), and
     * we want exactly ONE clean "send this file" banner for the user. We print it ourselves
     * from diag.last_bundle after the run.  The bundle(s) are still written. */
    diag.quiet_banner = 1;
    j5d_set_diag(&diag);
    j5d_interp_set_diag(&diag);
    j5n_signal_install(&diag);                             /* catch a genuine host fault too */

    /* ====================================================================================
     * --diff: the [J5n] differential lockstep checker.  HONEST SCOPE: the engine's lockstep
     * driver (j5d_run_diff) compares the JIT and the interpreter at every BLOCK BOUNDARY.
     * That comparison is validated for the single-injected-divergence micro-case ([J5n] test),
     * but on real MULTI-BLOCK corpus programs the block-boundary granularity makes the oracle
     * and the JIT line up at transiently-different points, so it flags boundary FALSE POSITIVES
     * on programs the corpus regression PROVES are byte-exact.  Rather than write a misleading
     * "divergence" crash bundle on a correct program, run68k does NOT run the lockstep over a
     * whole program: it prints this honest notice and runs the program normally so the user
     * still gets the real output + exit.  The genuine JIT-vs-oracle check for the whole corpus
     * is the regression itself (`make hosted-jit68k-j5m` / `-j5t` / ... — byte-exact per program);
     * the micro-locator lives in `make hosted-jit68k-j5n`.  This is a documented follow-on. */
    if (diff) {
        fprintf(stderr,
"run68k: note: --diff (whole-program lockstep) is not supported by the current engine\n"
"run68k:       driver — it reports block-boundary false positives on multi-block programs.\n"
"run68k:       Running normally instead.  The byte-exact JIT-vs-interpreter check for the\n"
"run68k:       corpus is the regression (make hosted-jit68k-j5m / -j5t); the single-divergence\n"
"run68k:       locator is `make hosted-jit68k-j5n`.\n");
        /* fall through to the normal run so the user still gets output + the real exit code. */
    }

    /* ---- the normal path: run it through the JIT ---- */
    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    uint32_t d0 = 0;
    int rc = j5d_run(&j5sb, seg.entry, LIBBASE, &st, &d0, bridge, &c, err, sizeof err);

    /* ---- stream the program's captured output to STDOUT (clean / pipe-able). Even on a
     * fault we flush what was produced before the fault, so a partial render is visible. ---- */
    if (lib.outlen > 0) {
        fwrite(lib.out, 1, (size_t)lib.outlen, stdout);
        fflush(stdout);
    }

    /* ---- verbose engine stats -> stderr (never stdout) ---- */
    if (verbose) {
        j5d_stats s; j5d_get_stats(&s);
        fprintf(stderr,
            "run68k: ---- engine stats ----------------------------------------------\n"
            "run68k: program        : %s\n"
            "run68k: hunks          : %d (entry pc = 0x%08X)\n"
            "run68k: blocks         : %u translated, %u executed (%u cache hits)\n"
            "run68k: m68k insns     : %u decoded\n"
            "run68k: memory accesses: %u (An)-class sandbox loads/stores\n"
            "run68k: library calls  : %u bridged via the [J3] LVO marshaller\n"
            "run68k: FP             : %u FP cc-ops, %u FMOVEM/FP-sys ops\n"
            "run68k: chaining       : %u direct block->block branches (no C dispatch hop)\n"
            "run68k: exceptions     : %u dispatched, %u rte returns\n"
            "run68k: AArch64 emitted: %u words into MAP_JIT\n"
            "run68k: exit D0        : 0x%08X (%u)\n"
            "run68k: ----------------------------------------------------------------\n",
            prog_path, seg.numhunks, seg.entry,
            s.blocks_translated, s.blocks_executed, s.block_cache_hits,
            s.insns_decoded, s.mem_accesses, s.lib_calls,
            s.fp_cc_ops, s.fp_movem_ops, s.chain_branches_taken,
            s.exceptions_dispatched, s.rte_returns, s.arm_words_emitted,
            d0, d0);
    }

    /* ---- teardown ---- */
    j5n_signal_remove();
    j5d_set_diag(NULL);
    j5d_interp_set_diag(NULL);
    j5d_run_free();
    j3_free_all_thunks();
    free(mem);
    free(prog);

    /* ---- the exit code ----
     * Success: the program's D0 clamped to a shell-meaningful 0..255.
     * Fault (a bundle was written): exit nonzero (the banner already pointed at the file).
     * Clean engine error (unsupported opcode, etc.): print it to stderr, exit nonzero. */
    if (rc != 0) {
        if (diag.bundles_written > 0) {
            /* A fault was diagnosed + bundled.  Print ONE clean "send this file" banner
             * (the engine's own per-funnel banner was suppressed via diag.quiet_banner). */
            fprintf(stderr,
"\n"
"============================================================================\n"
"  !!  AROS 68k JIT FAULT  —  the program faulted; a crash bundle was written.\n"
"  !!  %s\n"
"  !!  SEND THIS FILE to the developer.\n"
"  !!  (open README.txt inside if you're not sure what this is.)\n"
"============================================================================\n",
                diag.last_bundle);
            return 70;   /* EX_SOFTWARE-ish: a fault was diagnosed + bundled */
        }
        fprintf(stderr, "run68k: %s: %s\n", prog_path,
                err[0] ? err : "the program could not be run (unsupported/undecodable)");
        return 71;
    }
    return (int)(d0 & 0xFFu);
}
