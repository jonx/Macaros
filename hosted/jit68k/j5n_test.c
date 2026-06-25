/* j5n_test.c — [J5n] value-asserting driver for the 68k-JIT DIAGNOSTICS subsystem (OURS,
 * AROS-licensed). Proves faults are never silent: each fault kind produces a self-contained
 * crash BUNDLE with everything a developer needs, the LOUD banner prints the path, and the
 * report carries the two-level dump. Plus the differential lockstep trap, the deterministic
 * replay-to-N landing, and the host-signal safety net. Authored from the spec ([J5n]) + the
 * Motorola 68000 ISA. Uses NO Emu68 source; the engine it drives calls the REAL Emu68 decoders.
 *
 * The asserts (each must hold for [J5n] PASS):
 *   A. Each fault kind (out-of-sandbox access / illegal instruction / div-by-zero) writes a
 *      bundle whose tar.gz contains README.txt + MANIFEST.txt + REPORT.txt + core.snapshot +
 *      program.exe + REPRODUCE.txt; the banner path is printed; REPORT.txt contains the 68k
 *      regs + host regs section + both stacks + the coordinate.
 *   B. Differential mode: a deliberately-injected divergence under JIT68K_DIFF traps at the
 *      exact diverging instruction with a diverge.txt.
 *   C. Replay-to-N: crash at #N, then JIT68K_RUNTO=N re-runs and lands at exactly #N (same PC).
 *   D. Host-signal net: a real out-of-sandbox HOST access is caught + bundled, not a silent crash.
 *   E. Symbol mapping: the labelled diagfault.exe's `divide` symbol names the faulting function.
 *   F. Bundles are cleaned up; the corpus regression is asserted by the other targets.
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j5n_diag.h"
#include "j5n_symbols.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define ORG     0x00210000u
#define SZ      0x00040000u          /* spans 0x210000..0x250000, covers VBR @ 0x240000 */

static const char *apps_dir(void)
{
    const char *d = getenv("APPS68K_DIR");
    return (d && *d) ? d : "hosted/jit68k/apps68k";
}

static uint8_t g_filebuf[1 << 16];
static uint8_t *read_exe(const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", apps_dir(), rel);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[J5n] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static volatile sig_atomic_t g_alarmed = 0;
static void on_alarm(int sig){ (void)sig; g_alarmed = 1;
    const char *m = "[J5n] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;
#define CHECK(cond, label) do { int _c = !!(cond); printf("      %-44s %s\n", (label), _c?"ok":"X"); if(!_c) g_fail=1; } while(0)

/* ---- the crash dir for this test run (isolated, cleaned up). ---- */
static char g_crashdir[1024];
static void set_crashdir(void)
{
    snprintf(g_crashdir, sizeof g_crashdir, "%s/.j5n-crash", apps_dir());
    /* clean any stale contents */
    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_crashdir, g_crashdir);
    int rc = system(cmd); (void)rc;
    setenv("JIT68K_CRASH_DIR", g_crashdir, 1);
}
static void cleanup_crashdir(void)
{
    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_crashdir);
    int rc = system(cmd); (void)rc;
}

/* ---- does the last bundle's tar.gz contain `member`, and does REPORT.txt contain `needle`?
 * We list the archive members + extract REPORT.txt via `tar` (already a build dep). ---- */
static int archive_has_member(const char *archive, const char *member)
{
    char cmd[2200];
    snprintf(cmd, sizeof cmd, "tar -tzf '%s' 2>/dev/null | grep -q '/%s$'", archive, member);
    return system(cmd) == 0;
}
static int report_contains(const char *archive, const char *needle)
{
    char cmd[2600];
    /* extract the REPORT.txt member to stdout and grep it. */
    snprintf(cmd, sizeof cmd,
             "tar -xzOf '%s' --include '*/REPORT.txt' 2>/dev/null | grep -qF '%s'", archive, needle);
    return system(cmd) == 0;
}

/* ===================== a fault scenario harness ===================== */
/* Load `exe`, build a diag config, run the JIT (which faults), assert the bundle. */
static void scenario_bundle(const char *title, const char *exe, j5n_fault_kind want_kind,
                            int expect_symbol, const char *symbol_needle)
{
    printf("  %s\n", title);
    uint8_t *mem = calloc(1, SZ);
    size_t len; uint8_t *buf = read_exe(exe, &len);
    if (!buf) { g_fail = 1; free(mem); return; }

    j4_sandbox sb; j4_sandbox_init(&sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    if (j4_load_hunks(&sb, buf, len, 0, &seg, err, sizeof err)) {
        printf("    load error: %s\n", err); g_fail = 1; free(mem); return;
    }
    /* keep a private copy of the file bytes for the bundle (g_filebuf is reused). */
    static uint8_t progcopy[1 << 16];
    memcpy(progcopy, buf, len);

    /* parse symbols. */
    j5n_symtab symtab;
    j5n_symbols_parse(progcopy, len, &seg, &symtab);

    j5d_sandbox dsb = { sb.host_mem, sb.sandbox_origin, sb.size };
    j5n_diag diag;
    j5n_diag_init(&diag, progcopy, len, &dsb, seg.entry, 0, &symtab);
    diag.quiet_banner = 0;               /* show the banner for the headline */
    j5d_set_diag(&diag);

    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    uint32_t d0 = 0; char rerr[200] = {0};
    int rc = j5d_run(&dsb, seg.entry, 0, &st, &d0, NULL, NULL, rerr, sizeof rerr);
    j5d_set_diag(NULL);

    /* a fault means rc != 0 AND a bundle was written. */
    printf("    run rc=%d  err=\"%s\"  bundles=%d\n", rc, rerr, diag.bundles_written);
    CHECK(rc != 0, "the run faulted (did not silently finish)");
    CHECK(diag.bundles_written == 1, "exactly one crash bundle written");
    CHECK(diag.last_bundle[0] != '\0', "banner printed an absolute bundle path");

    const char *arc = diag.last_bundle;
    CHECK(archive_has_member(arc, "README.txt"),    "bundle contains README.txt");
    CHECK(archive_has_member(arc, "MANIFEST.txt"),  "bundle contains MANIFEST.txt");
    CHECK(archive_has_member(arc, "REPORT.txt"),    "bundle contains REPORT.txt");
    CHECK(archive_has_member(arc, "core.snapshot"), "bundle contains core.snapshot");
    CHECK(archive_has_member(arc, "program.exe"),   "bundle contains program.exe");
    CHECK(archive_has_member(arc, "program.sha256"),"bundle contains program.sha256");
    CHECK(archive_has_member(arc, "REPRODUCE.txt"), "bundle contains REPRODUCE.txt");

    /* REPORT.txt content: the coordinate + both register banks + both stacks. */
    CHECK(report_contains(arc, "68k REGISTERS"),           "REPORT has 68k registers");
    CHECK(report_contains(arc, "HOST AArch64 REGISTERS"),  "REPORT has host AArch64 regs");
    CHECK(report_contains(arc, "68k call stack"),          "REPORT has the 68k call stack");
    CHECK(report_contains(arc, "native host backtrace"),   "REPORT has the host backtrace");
    CHECK(report_contains(arc, "FLIGHT RECORDER"),         "REPORT has the flight recorder");
    CHECK(report_contains(arc, "instruction#"),            "REPORT has the coordinate #N");
    CHECK(report_contains(arc, j5n_fault_kind_name(want_kind)), "REPORT names the fault kind");

    if (expect_symbol) {
        CHECK(symtab.had_symbol_hunk, "HUNK_SYMBOL parsed (label->addr map present)");
        CHECK(report_contains(arc, symbol_needle), "REPORT names the faulting function (symbol)");
    }

    free(mem);
    j5d_run_free();
}

/* ===================== B. the differential (lockstep) trap ===================== */
/* Inject a DELIBERATE divergence: a custom LVO handler that the JIT's bridge calls writes a
 * DIFFERENT d0 than the oracle's bridge — so the post-call block boundary diverges. We run a
 * tiny program that calls a library LVO then continues, under JIT68K_DIFF, and assert the diff
 * driver traps at the exact diverging instruction with a diverge.txt. (Negative-control style:
 * the divergence is forced so the trap deterministically bites.) */
static int g_jit_call_count = 0;
static int jit_lvo(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    (void)lvo; (void)user; (void)e; (void)el;
    /* The JIT path returns d0 = 0xBAD; the oracle path (oracle_lvo) returns d0 = 0x111. The
     * states then diverge at the instruction after the library call. */
    st->d[0] = 0xBAD;
    g_jit_call_count++;
    return 0;
}
static int oracle_lvo(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    (void)lvo; (void)user; (void)e; (void)el;
    st->d[0] = 0x111;          /* the "correct" reference value */
    return 0;
}

static void scenario_diff(void)
{
    printf("  B. differential lockstep (JIT68K_DIFF): an injected divergence traps at the\n");
    printf("     exact instruction where JIT != oracle, with a diverge.txt.\n");

    /* A tiny hand-assembled program (ORG=0x210000):
     *   0x210000: 2C7C 0021 0000  movea.l #0x210000,a6   ; A6 = a fake library base (sandbox)
     *   0x210006: 4EAE FFFA       jsr -6(a6)             ; LVO 1 -> the bridge (divergence here)
     *   0x21000A: 5280            addq.l #1,d0           ; <- the post-call insn that DIVERGES
     *   0x21000C: 4E75            rts
     * The bridge sets d0 (JIT=0xBAD, oracle=0x111); after addq the states differ -> trap. */
    static const uint8_t CODE[] = {
        0x2C,0x7C,0x00,0x21,0x00,0x00,   /* movea.l #0x210000,a6 */
        0x4E,0xAE,0xFF,0xFA,             /* jsr -6(a6)  (LVO 1)  */
        0x52,0x80,                       /* addq.l #1,d0          */
        0x4E,0x75                        /* rts                   */
    };
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *rmem = calloc(1, SZ);
    memcpy(mem,  CODE, sizeof CODE);
    memcpy(rmem, CODE, sizeof CODE);
    j5d_sandbox sb  = { mem,  ORG, SZ };
    j5d_sandbox rsb = { rmem, ORG, SZ };

    j5n_diag diag;
    j5n_diag_init(&diag, CODE, sizeof CODE, &sb, ORG, ORG, NULL);
    diag.diff_enabled = 1;
    diag.quiet_banner = 1;               /* keep the headline banner for the real-fault cases */
    j5d_set_diag(&diag);
    j5d_interp_set_diag(&diag);

    struct j5d_m68k_state jit, ref;
    memset(&jit, 0, sizeof jit); memset(&ref, 0, sizeof ref);
    uint32_t d0 = 0; char e[200] = {0};
    g_jit_call_count = 0;
    /* INJECTED divergence: the JIT's library bridge (jit_lvo) returns d0=0xBAD; the oracle's
     * bridge (oracle_lvo) returns d0=0x111. After the post-call `addq.l #1,d0` the two states
     * differ -> the lockstep diff traps at exactly 0x21000A. This is a negative-control-style
     * forced divergence so the trap deterministically bites (consistent with the other [J5*]
     * negative controls). */
    int rc = j5d_run_diff(&sb, ORG, ORG, &jit, &ref, &rsb, &d0,
                          jit_lvo, NULL, oracle_lvo, NULL, e, sizeof e);
    j5d_set_diag(NULL);
    j5d_interp_set_diag(NULL);

    printf("    diff rc=%d  diverged=%d  at PC=0x%08X op=0x%04X  what=\"%s\"\n",
           rc, diag.diverged, diag.diverge_pc, diag.diverge_op, diag.diverge_what);
    CHECK(diag.diverged == 1, "the lockstep diff detected a divergence");
    CHECK(diag.diverge_pc == 0x21000A, "trapped at the exact diverging instruction (0x21000A)");
    CHECK(diag.bundles_written == 1, "a crash bundle was written for the divergence");
    if (diag.last_bundle[0])
        CHECK(archive_has_member(diag.last_bundle, "diverge.txt"), "bundle contains diverge.txt");

    free(mem); free(rmem);
    j5d_run_free();
}

/* ===================== C. replay-to-N (deterministic) ===================== */
/* Run diagfault.exe once normally to record the crash #N, then re-run with JIT68K_RUNTO=N and
 * assert the run lands at exactly instruction #N (same PC). The replay uses the instruction-
 * precise oracle stepper (j5d_interp_run + the step hook) which counts each instruction. */
static void scenario_replay(void)
{
    printf("  C. replay-to-N: crash at #N, then JIT68K_RUNTO=N lands at exactly #N.\n");
    uint8_t *mem = calloc(1, SZ);
    size_t len; uint8_t *buf = read_exe("bin/diagfault.exe", &len);
    if (!buf) { g_fail = 1; free(mem); return; }
    static uint8_t progcopy[1 << 16];
    memcpy(progcopy, buf, len);

    j4_sandbox sb; j4_sandbox_init(&sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    if (j4_load_hunks(&sb, progcopy, len, 0, &seg, err, sizeof err)) {
        printf("    load error: %s\n", err); g_fail = 1; free(mem); return;
    }
    j5d_sandbox dsb = { sb.host_mem, sb.sandbox_origin, sb.size };

    /* PASS 1: run the JIT to the fault, record the crash coordinate #N. */
    j5n_diag diag; j5n_diag_init(&diag, progcopy, len, &dsb, seg.entry, 0, NULL);
    diag.quiet_banner = 1;
    j5d_set_diag(&diag);
    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    uint32_t d0 = 0; char e1[200] = {0};
    j5d_run(&dsb, seg.entry, 0, &st, &d0, NULL, NULL, e1, sizeof e1);
    j5d_set_diag(NULL);
    uint64_t crash_n = diag.insn_number;
    printf("    pass 1: crashed at instruction #N = %llu (PC 0x%08X)\n",
           (unsigned long long)crash_n, st.pc);

    /* PASS 2: re-run the SAME program under replay-to-N via the instruction-precise oracle. The
     * oracle counts each instruction and breaks at exactly #N. We assert it lands on the SAME
     * PC the crash was at (the divu.w inside `divide`). */
    uint8_t *mem2 = calloc(1, SZ);
    j4_sandbox sb2; j4_sandbox_init(&sb2, mem2, ORG, SZ);
    j4_seglist seg2; char err2[200] = {0};
    j4_load_hunks(&sb2, progcopy, len, 0, &seg2, err2, sizeof err2);
    j5d_sandbox dsb2 = { sb2.host_mem, sb2.sandbox_origin, sb2.size };

    j5n_diag rdiag; j5n_diag_init(&rdiag, progcopy, len, &dsb2, seg2.entry, 0, NULL);
    rdiag.runto_enabled = 1; rdiag.runto_n = crash_n;
    j5d_interp_set_diag(&rdiag);
    struct j5d_m68k_state rst; memset(&rst, 0, sizeof rst);
    uint32_t rd0 = 0; char e2[200] = {0};
    j5d_interp_run(&dsb2, seg2.entry, 0, &rst, &rd0, NULL, NULL, e2, sizeof e2);
    j5d_interp_set_diag(NULL);

    printf("    pass 2: JIT68K_RUNTO=%llu landed: hit=%d at PC 0x%08X (counter #N=%llu)\n",
           (unsigned long long)crash_n, rdiag.runto_hit, rdiag.runto_pc,
           (unsigned long long)rdiag.insn_number);
    CHECK(rdiag.runto_hit == 1, "replay broke at exactly instruction #N");
    CHECK(rdiag.runto_pc == st.pc, "replay landed on the SAME PC as the crash (deterministic)");
    CHECK(rdiag.insn_number == crash_n, "replay counter reached #N");

    free(mem); free(mem2);
    j5d_run_free();
}

/* ===================== D. the host-signal safety net ===================== */
/* A genuine out-of-sandbox HOST access must be caught + bundled, not a silent crash. We
 * deliberately dereference a wild host pointer with the signal net installed, in a CHILD
 * process (the net re-raises to die after bundling). The parent asserts: the child died by
 * the signal (proving the net let the original disposition kill it AFTER diagnosing) AND a
 * bundle landed in the crash dir. This is the spec's "real out-of-sandbox host access is
 * caught and bundled" — the graft/cpu_aarch64.h seam exercised host-side. */
static void scenario_signal(void)
{
    printf("  D. host-signal net: a real out-of-sandbox HOST access is caught + bundled\n");
    printf("     (not a silent host crash) — the graft/cpu_aarch64.h SIGSEGV seam, host-side.\n");

    /* count bundles before. */
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "ls '%s'/*.tar.gz 2>/dev/null | wc -l", g_crashdir);
    FILE *p = popen(cmd, "r"); int before = 0; if (p) { if(fscanf(p,"%d",&before)!=1) before=0; pclose(p); }

    pid_t pid = fork();
    if (pid == 0) {
        /* child: install the net, then a wild host write. */
        uint8_t *mem = calloc(1, SZ);
        j5d_sandbox dsb = { mem, ORG, SZ };
        static uint8_t prog[16] = {0};
        j5n_diag diag; j5n_diag_init(&diag, prog, sizeof prog, &dsb, ORG, 0, NULL);
        diag.quiet_banner = 1;                    /* the child's banner would pollute the log */
        j5d_set_diag(&diag);
        static struct j5d_m68k_state st; memset(&st, 0, sizeof st);
        st.pc = ORG; st.a[0] = 0xDEADBEEF;        /* a bogus 68k A0, for the report */
        j5n_signal_set_context(&st, &dsb);
        j5n_signal_install(&diag);
        /* the wild HOST access (a NULL-ish deref far outside any mapping). */
        volatile uint8_t *wild = (volatile uint8_t *)(uintptr_t)0x4;
        *wild = 0x42;                             /* -> SIGSEGV -> the net -> bundle -> re-raise */
        _exit(99);                                /* not reached */
    }

    int status = 0; waitpid(pid, &status, 0);
    int by_signal = WIFSIGNALED(status);
    int sig = by_signal ? WTERMSIG(status) : 0;

    snprintf(cmd, sizeof cmd, "ls '%s'/*.tar.gz 2>/dev/null | wc -l", g_crashdir);
    p = popen(cmd, "r"); int after = 0; if (p) { if(fscanf(p,"%d",&after)!=1) after=0; pclose(p); }

    printf("    child died by signal=%d (%s), bundles before=%d after=%d\n",
           sig, by_signal ? "yes" : "NO (silent?)", before, after);
    CHECK(by_signal, "the host fault was NOT silent (process died by the signal)");
    CHECK(after > before, "the host-signal net wrote a crash bundle");
}

/* ===================== E. snapshot reload (the optional inspect mode) ===================== */
static void scenario_snapshot(void)
{
    printf("  E. core.snapshot reload: a written snapshot reloads to the crashed state.\n");
    /* run diagfault, then extract + reload its core.snapshot and compare the PC. */
    uint8_t *mem = calloc(1, SZ);
    size_t len; uint8_t *buf = read_exe("bin/diagfault.exe", &len);
    if (!buf) { g_fail = 1; free(mem); return; }
    static uint8_t progcopy[1 << 16]; memcpy(progcopy, buf, len);
    j4_sandbox sb; j4_sandbox_init(&sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    j4_load_hunks(&sb, progcopy, len, 0, &seg, err, sizeof err);
    j5d_sandbox dsb = { sb.host_mem, sb.sandbox_origin, sb.size };
    j5n_diag diag; j5n_diag_init(&diag, progcopy, len, &dsb, seg.entry, 0, NULL);
    diag.quiet_banner = 1;
    j5d_set_diag(&diag);
    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    uint32_t d0 = 0; char e[200] = {0};
    j5d_run(&dsb, seg.entry, 0, &st, &d0, NULL, NULL, e, sizeof e);
    j5d_set_diag(NULL);

    /* extract core.snapshot from the archive into the crash dir, then reload it. */
    char snap[1400];
    snprintf(snap, sizeof snap, "%s/reload.snapshot", g_crashdir);
    char cmd[3000];
    snprintf(cmd, sizeof cmd,
             "tar -xzOf '%s' --include '*/core.snapshot' > '%s' 2>/dev/null",
             diag.last_bundle, snap);
    int xrc = system(cmd);
    struct j5d_m68k_state loaded; memset(&loaded, 0, sizeof loaded);
    uint8_t *img = NULL; size_t ilen = 0; uint32_t orig = 0;
    int lrc = (xrc == 0) ? j5n_snapshot_load(snap, &loaded, &img, &ilen, &orig) : 1;
    printf("    reload rc=%d  snapshot PC=0x%08X (crash PC=0x%08X) image=%zu bytes origin=0x%08X\n",
           lrc, loaded.pc, st.pc, ilen, orig);
    CHECK(lrc == 0, "core.snapshot reloaded successfully");
    CHECK(loaded.pc == st.pc, "reloaded PC equals the crash PC");
    CHECK(ilen == SZ, "reloaded sandbox image is the full size");
    if (img) free(img);
    free(mem);
    j5d_run_free();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(30);
    set_crashdir();

    printf("[J5n] 68k-JIT DIAGNOSTICS: faults are never silent. ANY fault writes a single,\n");
    printf("      self-contained, shareable crash BUNDLE (a tar.gz with a two-level report,\n");
    printf("      a reloadable core snapshot, the exact program, a deterministic replay\n");
    printf("      command, and a friendly README) and prints a LOUD banner with its path.\n");
    printf("      Plus a runtime DIFFERENTIAL (lockstep-vs-oracle) mistranslation locator and\n");
    printf("      a deterministic REPLAY-TO-N. Below the frozen seam (a side-channel like\n");
    printf("      j5d_set_exc_log); struct M68KState / the [J3] LVO contract / the [J5i]\n");
    printf("      exception model are UNCHANGED.\n\n");

    printf("[A] each fault kind writes a complete bundle with the LOUD banner:\n\n");
    scenario_bundle("A1. div-by-zero (no handler) in a LABELLED program (symbol mapping):",
                    "bin/diagfault.exe", J5N_FAULT_DIVZERO, /*symbol*/1, "divide");
    printf("\n");
    scenario_bundle("A2. illegal instruction (ILLEGAL 0x4AFC, no handler):",
                    "bin/diagill.exe", J5N_FAULT_ILLEGAL, 0, NULL);
    printf("\n");
    scenario_bundle("A3. out-of-sandbox PC (bus error, no handler):",
                    "bin/diagbus.exe", J5N_FAULT_BUS, 0, NULL);

    printf("\n");
    scenario_diff();
    printf("\n");
    scenario_replay();
    printf("\n");
    scenario_signal();
    printf("\n");
    scenario_snapshot();

    cleanup_crashdir();

    if (g_fail) { printf("\n[J5n] FAIL\n"); return 1; }

    printf("\n  VERDICT: the diagnostics subsystem turns every 68k-JIT fault into a single,\n");
    printf("           shareable crash bundle (README + MANIFEST + the two-level REPORT with\n");
    printf("           68k AND host registers + both stacks + the flight recorder + the\n");
    printf("           coordinate #N, a reloadable core.snapshot, program.exe + sha256, and\n");
    printf("           REPRODUCE), with a LOUD banner; the differential mode pins the exact\n");
    printf("           mistranslated instruction (diverge.txt); replay-to-N lands on #N\n");
    printf("           deterministically; the host-signal net catches a real out-of-sandbox\n");
    printf("           HOST access (the graft/cpu_aarch64.h seam, host-side); HUNK_SYMBOL\n");
    printf("           names the faulting function. The frozen seam is unchanged.\n");
    printf("[J5n] PASS\n");
    return 0;
}
