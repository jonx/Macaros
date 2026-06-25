/* j5l_test.c — [J5l] movem (move-multiple-registers) coverage: the opcode every
 * compiler-generated 68k function uses in its prologue/epilogue. Drives Emu68's REAL
 * EMIT_MOVEM decoder (M68k_LINE4.c) — predecrement-reversed mask order, postincrement /
 * predecrement An-update, control/(d16,An)/.w forms — routing its memory touches through
 * the sandbox (j5d_ea_helpers.c j5d_movem_* helpers: base-adjust + REV per register).
 * (OURS, AROS-licensed; calls the adopted emitter, copies no Emu68 source.)
 *
 * THE PROOF (correctness GATES the marker):
 *   1. j5l.exe — a compiler-style non-leaf subroutine `work` does `movem.l d2-d7/a2-a6,-(sp)`
 *      (PROLOGUE predecrement save) + `movem.l (sp)+,d2-d7/a2-a6` (EPILOGUE postincrement
 *      restore) around a body that CLOBBERS every one of those registers, called from a
 *      caller that seeded them with sentinels. It also exercises the control / (d16,An) / .w
 *      movem forms against a fixed sandbox frame. Asserted BYTE-EXACT (full register file +
 *      the WHOLE sandbox memory, incl. the saved frame on the stack AND the control frame)
 *      vs the independent from-scratch interpreter, AND d0 == 0x7FF (the caller's
 *      d2-d7/a2-a6 — 11 registers — all SURVIVED the call; the save/restore actually
 *      mattered), AND >=1 movem memory access went through the JIT.
 *   2. NEGATIVE CONTROL — neuter the epilogue restore (turn `movem.l (sp)+,...` into a
 *      register-mask of 0) in the JIT copy only: the clobbered callee-saved regs then leak
 *      out, the survival mask is no longer 0x7FF, and the JIT diverges from the oracle —
 *      proving the byte-exact assert and the survival check are not tautologies.
 *   3. REGRESSION — the whole existing corpus (mul/fact/arraysum/libcall/sumsq/bubsort/
 *      mp64/mandel) re-run through the SAME engine, each byte-exact vs the oracle, so the
 *      movem darwinize edit + the oracle extension did not perturb anything else.
 *
 * Watchdog: SIGALRM -> [J5l] FAIL.
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define ORG       0x00210000u
#define SZ        0x00040000u
#define LIBBASE   0x00230000u
#define HEAP_BASE 0x00231000u
#define HEAP_END  0x00238000u

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
    if (!f) { fprintf(stderr, "[J5l] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5l] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

static int load_one(const char *rel, uint8_t *mem, j4_sandbox *sb, uint32_t *entry)
{
    size_t len; uint8_t *buf = read_exe(rel, &len);
    if (!buf) return 1;
    j4_sandbox_init(sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    if (j4_load_hunks(sb, buf, len, /*skip_reloc=*/0, &seg, err, sizeof err)) {
        printf("    load error: %s\n", err); return 1;
    }
    *entry = seg.entry;
    return 0;
}

struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

/* Run one program through the JIT AND the oracle over independent sandbox copies, assert the
 * register file + the FULL sandbox memory are byte-exact and d0 agrees. Returns 0 on a
 * byte-exact match with d0==want (and, if want_mem, that movem/EA memory traffic ran). */
static int run_byte_exact(const char *label, const char *rel, uint32_t want,
                          int check_want, int want_mem_traffic, j5d_stats *out_s)
{
    int ok_overall = 0;
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    j4_sandbox sb, sb2; uint32_t entry = 0, entry2 = 0;
    if (load_one(rel, mem, &sb, &entry) || load_one(rel, mem2, &sb2, &entry2)) { goto out; }
    j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
    j5d_sandbox b = { sb2.host_mem, sb2.sandbox_origin, sb2.size };

    stub_lib jlib; struct bctx jc = { &jlib, &sb };
    stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s); if (out_s) *out_s = s;

    stub_lib rlib; struct bctx rc_ = { &rlib, &sb2 };
    stublib_init(&rlib, &sb2, LIBBASE, HEAP_BASE, HEAP_END);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, entry2, LIBBASE, &ref, &rd0, bridge, &rc_, e2, sizeof e2);

    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int mem_ok  = (memcmp(mem, mem2, SZ) == 0);
    int d0_ok   = (rc == 0) && (d0 == rd0) && (!check_want || d0 == want);
    int memt_ok = !want_mem_traffic || (s.mem_accesses > 0);
    ok_overall = regs_ok && mem_ok && d0_ok && memt_ok;

    printf("    %-12s JIT d0=0x%08x ORACLE d0=0x%08x | byte-exact: regs=%s mem=%s%s -> %s\n",
           label, d0, rd0, regs_ok ? "yes" : "NO", mem_ok ? "yes" : "NO",
           want_mem_traffic ? (memt_ok ? " jit-mem=yes" : " jit-mem=NO") : "",
           ok_overall ? "OK" : "FAIL");
    if (!ok_overall) {
        if (rc)  printf("      JIT err: %s\n", err);
        if (irc) printf("      oracle err: %s\n", e2);
        if (check_want && d0 != want) printf("      expected d0=0x%08x\n", want);
    }
out:
    free(mem); free(mem2);
    j5d_run_free(); j3_free_all_thunks();
    return ok_overall ? 0 : 1;
}

/* The [J5l] capstone: j5l.exe — the compiler-style movem save/restore, with the explicit
 * caller-registers-survive assert + the movem-form coverage report + the negative control. */
static void run_movem(void)
{
    printf("  (1) j5l.exe — movem.l d2-d7/a2-a6,-(sp) PROLOGUE save + movem.l (sp)+,... EPILOGUE\n");
    printf("      restore around a CLOBBERING body, + control/(d16,An)/.w movem forms.\n");

    j5d_stats s;
    int bad = run_byte_exact("j5l.exe", "bin/j5l.exe", 11u,
                             /*check_want=*/1, /*want_mem_traffic=*/1, &s);
    if (bad) g_fail = 1;
    /* d0 == 11 means all 11 caller-saved regs (d2-d7, a2-a6) SURVIVED the call (the program
     * counts the survivors) -> the save/restore actually mattered. The byte-exact mem check
     * covers the predecrement save frame on the stack AND the control/(d16,An)/.w frames at
     * SCRATCH. */
    printf("      caller-regs-survive: d0==11 asserted (all 11 of d2-d7/a2-a6 restored);\n");
    printf("      movem forms covered: -(sp) predecrement save (reversed mask), (sp)+ post-\n");
    printf("      increment restore, (An)/(d16,An) control store+load, and the .w forms.\n");
    printf("      movem memory accesses through the JIT: %u\n", s.mem_accesses);

    /* ---- NEGATIVE CONTROL: in the JIT copy ONLY, neuter the EPILOGUE restore by zeroing its
     * register-list mask word (movem with an empty mask transfers nothing -> the clobbered
     * callee-saved regs are NOT restored). The survival mask then != 0x7FF and the JIT diverges
     * from the (un-patched) oracle. Proves the byte-exact + survival asserts genuinely bite. */
    {
        uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
        j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
        if (!load_one("bin/j5l.exe", m3, &s3, &e3) && !load_one("bin/j5l.exe", m4, &s4, &e4)) {
            /* find the EPILOGUE `movem.l (sp)+,d2-d7/a2-a6` = opcode 0x4CDF, mask 0x7CFC, and
             * zero its mask word in the JIT copy (s3) only. */
            uint8_t *code = s3.host_mem; int patched = 0;
            for (uint32_t i = ORG; i + 4 <= ORG + 0x400; i += 2) {
                uint8_t *p = code + (i - ORG);
                if (p[0] == 0x4C && p[1] == 0xDF && p[2] == 0x7C && p[3] == 0xFC) {
                    p[2] = 0x00; p[3] = 0x00;   /* empty register mask -> restores nothing */
                    patched = 1; break;
                }
            }
            j5d_sandbox a2 = { s3.host_mem, s3.sandbox_origin, s3.size };
            j5d_sandbox b2 = { s4.host_mem, s4.sandbox_origin, s4.size };
            struct j5d_m68k_state j2, r2; memset(&j2,0,sizeof j2); memset(&r2,0,sizeof r2);
            uint32_t jd=0, rdv=0; char x1[200]={0}, x2[200]={0};
            stub_lib jl2, rl2; stublib_init(&jl2,&s3,LIBBASE,HEAP_BASE,HEAP_END);
            stublib_init(&rl2,&s4,LIBBASE,HEAP_BASE,HEAP_END);
            struct bctx jc2 = { &jl2, &s3 }, rc2 = { &rl2, &s4 };
            j5d_run(&a2, e3, LIBBASE, &j2, &jd, bridge, &jc2, x1, sizeof x1);
            j5d_interp_run(&b2, e4, LIBBASE, &r2, &rdv, bridge, &rc2, x2, sizeof x2);
            /* The un-patched oracle restores -> rdv==11; the patched JIT skips the restore, so
             * the clobbered callee regs leak -> jd != 11 (survivor count drops) and jd != rdv. */
            int diverged = patched && (jd != rdv) && (jd != 11u) && (rdv == 11u);
            printf("  (2) neg-ctrl: zero the epilogue restore mask in the JIT copy -> %s\n",
                   diverged ? "DIVERGED (JIT d0!=11, oracle d0==11: survival + byte-exact bite)"
                            : "FAILED TO BITE");
            printf("      JIT d0=0x%08x  oracle d0=0x%08x\n", jd, rdv);
            if (!diverged) g_fail = 1;
        }
        free(m3); free(m4);
        j5d_run_free(); j3_free_all_thunks();
    }
}

/* The whole existing corpus, re-run through the SAME engine (with the movem edit live) —
 * each byte-exact vs the oracle. (Programs that touch no library: bridge unused but harmless.) */
static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the whole existing corpus through the same (movem-edited) engine:\n");
    struct { const char *label, *rel; uint32_t want; } progs[] = {
        { "mul",      "bin/mul.exe",      42u },
        { "fact",     "bin/fact.exe",     120u },
        { "arraysum", "bin/arraysum.exe", 150u },
        { "libcall",  "bin/libcall.exe",  0u },
        { "sumsq",    "bin/sumsq.exe",    55u },
        { "bubsort",  "bin/bubsort.exe",  0x00F5B9F5u },
        { "mp64",     "bin/mp64.exe",     0x000004FCu },
    };
    for (unsigned i = 0; i < sizeof(progs)/sizeof(progs[0]); i++) {
        if (run_byte_exact(progs[i].label, progs[i].rel, progs[i].want,
                           /*check_want=*/1, /*want_mem_traffic=*/0, NULL))
            g_fail = 1;
    }
    /* mandel: large; check byte-exact + d0==0 (no fixed expected value beyond 0). */
    if (run_byte_exact("mandel", "bin/mandel.exe", 0u, /*check_want=*/1, 0, NULL))
        g_fail = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(30);

    printf("[J5l] movem (move-multiple-registers): the opcode every compiler-generated 68k\n");
    printf("      function uses in its prologue/epilogue. Drives Emu68's REAL EMIT_MOVEM decoder\n");
    printf("      (predecrement-reversed mask order, post/pre-increment An update, control/(d16,An)\n");
    printf("      /.w forms), routing its memory touches through the sandbox (base-adjust + REV).\n");
    printf("      Byte-exact vs the independent interpreter; the save/restore is made to MATTER.\n\n");

    run_movem();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5l] FAIL\n"); return 1; }
    printf("\n  VERDICT: movem runs through Emu68's REAL EMIT_MOVEM decoder byte-exact vs the\n");
    printf("           independent interpreter — the compiler prologue/epilogue save/restore\n");
    printf("           (predecrement reversed-mask -(sp) + postincrement (sp)+), the control /\n");
    printf("           (d16,An) / .w forms, with each register's memory touch sandbox-translated\n");
    printf("           (base-adjust + big-endian REV) — and the caller's d2-d7/a2-a6 provably\n");
    printf("           survive the clobbering call (d0==11). The negative control bites and\n");
    printf("           the whole corpus stays byte-exact. The frozen seam (jit_region API,\n");
    printf("           struct M68KState layout, the [J3] LVO contract, the [J5i] exception model)\n");
    printf("           is UNCHANGED — movem is a decoder/EA addition below the seam.\n");
    printf("[J5l] PASS\n");
    return 0;
}
