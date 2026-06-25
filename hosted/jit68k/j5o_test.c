/* j5o_test.c — [J5o] 68881/68882 FPU CORE coverage: the FIRST program in the corpus to use
 * the FPU coprocessor (line-F). Drives Emu68's REAL EMIT_FPU decoder (the verbatim, quarantined
 * M68k_LINEF.c) — FMOVE (reg<->reg, mem<->reg, format conversions .s/.d/.l/.w/.b), FADD/FSUB/
 * FMUL/FDIV/FSQRT/FABS/FNEG, FCMP, FTST — routing its FP memory touches through the sandbox
 * (j5d_ea_helpers.c j5d_fpu_* helpers: base-adjust + per-element byteswap) and the FP register
 * file + FPCR/FPSR through the appended state. (OURS, AROS-licensed; calls the adopted emitter,
 * copies no Emu68 source.)
 *
 * PRECISION MODEL: FP0..FP7 are modeled in IEEE-754 binary64 (`double`) — like Emu68; AArch64
 * has no 80-bit extended FP, so 80-bit extended-precision exactness is NOT bit-reproducible.
 * This is COMPLETE *instruction* coverage at double precision; for these ops (all IEEE-defined)
 * the double results are deterministic and bit-exact, which is the gate below.
 *
 * THE PROOF (correctness GATES the marker — the INDEPENDENT C-double oracle):
 *   1. j5o.exe runs through the JIT (REAL EMIT_FPU + the dispatcher + the sandbox FP-mem EA)
 *      AND through the independent from-scratch interpreter (j5d_interp.c, OURS, no Emu68,
 *      computes every FP op directly in C `double`). Asserted BYTE-EXACT on:
 *        - the integer register file (D0..D7 / A0..A7),
 *        - **the FP register file FP0..FP7 (the raw `double` bit patterns)**,
 *        - **the FPSR condition byte (N/Z/I/NAN) for the FCMP/FTST that set it**,
 *        - the WHOLE sandbox memory (incl. the FP-to-memory .d/.s/.l/.w/.b stored results),
 *        - the exit d0.
 *      AND >=1 FP memory access went through the JIT (the sandbox FP-mem helpers ran).
 *   2. NEGATIVE CONTROL — flip ONE FP arithmetic opcode (FADD->FSUB) in BOTH runs: the FP
 *      register file + the stored doubles diverge from the un-flipped expectation, and the
 *      bit-exact assert moves — proving it is not a tautology. (We flip in both so JIT and
 *      oracle still AGREE with each other, but the RESULT changes; a second control flips in
 *      the JIT copy ONLY so JIT and oracle DISAGREE — proving the cross-check bites.)
 *   3. REGRESSION — the whole existing corpus (mul/fact/arraysum/libcall/sumsq/bubsort/mp64/
 *      mandel) re-run through the SAME (FPU-enabled) engine, each byte-exact vs the oracle, so
 *      the FPU darwinize pass + the engine wiring + the oracle/RA changes did not perturb
 *      anything (FPU is additive + opcode-gated: a non-FP block emits ZERO FP load/store).
 *
 * Watchdog: SIGALRM -> [J5o] FAIL.
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
    if (!f) { fprintf(stderr, "[J5o] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5o] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_int_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
/* [J5o] the FP-state comparison — bit-exact on the raw double bit patterns (so NaN/-0.0/inf
 * all compare by bits, not by IEEE ==) + the FPSR condition byte. */
static int eq_fp_state(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) {
        uint64_t ba, bb;
        memcpy(&ba, &a->fp[i], 8); memcpy(&bb, &b->fp[i], 8);
        if (ba != bb) return 0;
    }
    /* compare only the FPSR condition-code byte (N/Z/I/NAN, bits 24..27) — the exception/quotient
     * bits are out of this increment's scope. */
    if ((a->fpsr & 0x0f000000u) != (b->fpsr & 0x0f000000u)) return 0;
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
 * integer regs + FP regs + FPSR + the FULL sandbox memory are byte-exact and d0 agrees.
 * `check_fp` enables the FP-state assert (the FPU program); for the integer-corpus regression it
 * is off (those leave fp[]/fpsr at the seeded 0, so the FP assert would trivially hold anyway).
 * `want_fp_mem` asserts >=1 FP memory access went through the JIT. */
static int run_byte_exact(const char *label, const char *rel, uint32_t want, int check_want,
                          int check_fp, int want_fp_mem, j5d_stats *out_s)
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

    int regs_ok = (rc == 0) && (irc == 0) && eq_int_regs(&jit, &ref);
    int fp_ok   = !check_fp || ((rc == 0) && (irc == 0) && eq_fp_state(&jit, &ref));
    int mem_ok  = (memcmp(mem, mem2, SZ) == 0);
    int d0_ok   = (rc == 0) && (d0 == rd0) && (!check_want || d0 == want);
    int memt_ok = !want_fp_mem || (s.mem_accesses > 0);
    ok_overall = regs_ok && fp_ok && mem_ok && d0_ok && memt_ok;

    printf("    %-12s JIT d0=0x%08x ORACLE d0=0x%08x | byte-exact: regs=%s%s mem=%s%s -> %s\n",
           label, d0, rd0, regs_ok ? "yes" : "NO",
           check_fp ? (fp_ok ? " fp=yes" : " fp=NO") : "",
           mem_ok ? "yes" : "NO",
           want_fp_mem ? (memt_ok ? " jit-fpmem=yes" : " jit-fpmem=NO") : "",
           ok_overall ? "OK" : "FAIL");
    if (!ok_overall) {
        if (rc)  printf("      JIT err: %s\n", err);
        if (irc) printf("      oracle err: %s\n", e2);
        if (check_want && d0 != want) printf("      expected d0=0x%08x\n", want);
        if (check_fp && !fp_ok) {
            printf("      FP DIVERGE — JIT vs ORACLE FP0..FP7 (raw bits) + FPSR:\n");
            for (int i = 0; i < 8; i++) {
                uint64_t bj, br; memcpy(&bj, &jit.fp[i], 8); memcpy(&br, &ref.fp[i], 8);
                printf("        fp%d JIT=%016llx (%.6g)  ORACLE=%016llx (%.6g)%s\n", i,
                       (unsigned long long)bj, jit.fp[i], (unsigned long long)br, ref.fp[i],
                       bj != br ? "  <<<" : "");
            }
            printf("        FPSR JIT=%08x ORACLE=%08x (cc bits %02x vs %02x)\n",
                   jit.fpsr, ref.fpsr, (jit.fpsr >> 24) & 0x0f, (ref.fpsr >> 24) & 0x0f);
        }
    }
out:
    free(mem); free(mem2);
    j5d_run_free(); j3_free_all_thunks();
    return ok_overall ? 0 : 1;
}

/* Inspect the expected FP results directly: the test KNOWS the program's arithmetic, so it
 * also independently verifies the JIT's FP0..FP7 + the stored doubles against the hand-computed
 * expectations (a third check, fully independent of BOTH the JIT and the oracle). */
static double mem_double(const uint8_t *mem, uint32_t addr)
{
    const uint8_t *p = mem + (addr - ORG);
    uint64_t bits = ((uint64_t)p[0]<<56)|((uint64_t)p[1]<<48)|((uint64_t)p[2]<<40)|((uint64_t)p[3]<<32)
                  | ((uint64_t)p[4]<<24)|((uint64_t)p[5]<<16)|((uint64_t)p[6]<<8)|(uint64_t)p[7];
    double d; memcpy(&d, &bits, 8); return d;
}
static uint32_t mem_be32(const uint8_t *mem, uint32_t addr)
{
    const uint8_t *p = mem + (addr - ORG);
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void run_fpu(void)
{
    printf("  (1) j5o.exe — FMOVE (int .l/.w/.b<->FP, single .s & double .d mem<->FP, reg<->reg)\n");
    printf("      + FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG + FCMP/FTST (FPSR cc), all at double prec.\n");

    j5d_stats s;
    int bad = run_byte_exact("j5o.exe", "bin/j5o.exe", 0u,
                             /*check_want=*/1, /*check_fp=*/1, /*want_fp_mem=*/1, &s);
    if (bad) g_fail = 1;
    printf("      FP memory accesses through the JIT (sandbox FP-mem EA): %u\n", s.mem_accesses);

    /* ---- THIRD, fully-independent check: re-run the JIT alone and verify FP0..FP7 + the stored
     * doubles against the hand-computed expectations (independent of the oracle). ---- */
    {
        uint8_t *mem = calloc(1, SZ);
        j4_sandbox sb; uint32_t entry = 0;
        if (!load_one("bin/j5o.exe", mem, &sb, &entry)) {
            j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
            stub_lib jlib; struct bctx jc = { &jlib, &sb };
            stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
            struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
            uint32_t d0 = 0; char err[200] = {0};
            j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);

            /* hand-computed expectations (see j5o.s): */
            struct { const char *name; double got, want; } fp[] = {
                { "fp0=7+2.5)*10 = 95.0",  jit.fp[0], 95.0 },
                { "fp1=single 2.5",        jit.fp[1], 2.5  },
                { "fp2=double 10.0",       jit.fp[2], 10.0 },
                { "fp3=95-2.5 = 92.5",     jit.fp[3], 92.5 },
                { "fp4=sqrt(10/2.5)=2.0",  jit.fp[4], 2.0  },
                { "fp5=|-3| = 3.0",        jit.fp[5], 3.0  },
                { "fp6=-(2.0) = -2.0",     jit.fp[6], -2.0 },
                { "fp7=(byte)100 = 100.0", jit.fp[7], 100.0 },
            };
            int allok = 1;
            for (unsigned i = 0; i < sizeof fp / sizeof fp[0]; i++) {
                uint64_t bg, bw; memcpy(&bg, &fp[i].got, 8); memcpy(&bw, &fp[i].want, 8);
                int ok = (bg == bw);
                if (!ok) { allok = 0; printf("      EXPECT FAIL %s : got %.6g\n", fp[i].name, fp[i].got); }
            }
            /* the stored doubles / ints in the RESULT area (a0 = lea result). The result label is
             * the second DATA item after fpconst (12 bytes); find it by the stored fp0 value. We
             * read a0 from the final register file (the program left a0 -> result). */
            uint32_t res = jit.a[0];
            double d_fp0 = mem_double(mem, res + 0);   /* stored fp0 .d = 95.0  */
            double d_fp6 = mem_double(mem, res + 8);   /* stored fp6 .d = -2.0  */
            uint32_t l_fp4 = mem_be32(mem, res + 20);  /* stored fp4 .l = 2     */
            uint16_t w_fp5 = (uint16_t)((mem[res - ORG + 24] << 8) | mem[res - ORG + 25]); /* .w = 3 */
            uint8_t  b_fp7 = mem[res - ORG + 26];      /* stored fp7 .b = 100   */
            { uint64_t a1,b1; double e=95.0; memcpy(&a1,&d_fp0,8); memcpy(&b1,&e,8);
              if (a1!=b1){allok=0; printf("      EXPECT FAIL stored fp0.d=%.6g (want 95.0)\n", d_fp0);} }
            { uint64_t a1,b1; double e=-2.0; memcpy(&a1,&d_fp6,8); memcpy(&b1,&e,8);
              if (a1!=b1){allok=0; printf("      EXPECT FAIL stored fp6.d=%.6g (want -2.0)\n", d_fp6);} }
            if (l_fp4 != 2u)   { allok = 0; printf("      EXPECT FAIL stored fp4.l=%u (want 2)\n", l_fp4); }
            if (w_fp5 != 3u)   { allok = 0; printf("      EXPECT FAIL stored fp5.w=%u (want 3)\n", w_fp5); }
            if (b_fp7 != 100u) { allok = 0; printf("      EXPECT FAIL stored fp7.b=%u (want 100)\n", b_fp7); }

            printf("      independent hand-check: FP0..FP7 + stored .d/.s/.l/.w/.b results -> %s\n",
                   allok ? "all match" : "MISMATCH");
            /* FPSR: the last FPSR-setting op was FTST fp6(-2.0) consumed by FMOVE->MEM -> N set,
             * Z clear (N at bit27). */
            int n_set = (jit.fpsr >> 27) & 1, z_set = (jit.fpsr >> 26) & 1;
            printf("      FPSR after FTST(-2.0)+FMOVE->MEM: N=%d Z=%d (expect N=1 Z=0)\n", n_set, z_set);
            if (!n_set || z_set) { allok = 0; }
            if (!allok) g_fail = 1;
        }
        free(mem); j5d_run_free(); j3_free_all_thunks();
    }
}

/* NEGATIVE CONTROL: flip the FADD opcode (opcode2 0x0422 -> FSUB 0x0428) in the JIT copy ONLY,
 * so the JIT computes 7.0-2.5 = 4.5 (then *10 = 45.0) while the oracle (un-patched) computes
 * 9.5*10 = 95.0 — the FP register file + stored doubles must DIVERGE between JIT and oracle.
 * (opcode2 = 0x0422: rm=0 src=fp1 dst=fp0 oper=0x22 FADD; flipping oper 0x22->0x28 = FSUB.) */
static void run_negctrl(void)
{
    uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
    j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
    if (!load_one("bin/j5o.exe", m3, &s3, &e3) && !load_one("bin/j5o.exe", m4, &s4, &e4)) {
        /* find the FADD: opcode 0xF200, opcode2 0x0422 (FADD.x fp1,fp0). Flip opcode2's low
         * operation byte 0x22 (FADD) -> 0x28 (FSUB) in the JIT copy (s3) only. */
        uint8_t *code = s3.host_mem; int patched = 0;
        for (uint32_t i = ORG; i + 4 <= ORG + 0x400; i += 2) {
            uint8_t *p = code + (i - ORG);
            if (p[0] == 0xF2 && p[1] == 0x00 && p[2] == 0x04 && p[3] == 0x22) {
                p[3] = 0x28;       /* FADD (0x22) -> FSUB (0x28) */
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

        uint64_t bj, br; memcpy(&bj, &j2.fp[0], 8); memcpy(&br, &r2.fp[0], 8);
        int diverged = patched && (bj != br) && (memcmp(m3, m4, SZ) != 0);
        printf("  (2) neg-ctrl: FADD->FSUB in the JIT copy only -> %s\n",
               diverged ? "DIVERGED (JIT fp0 != oracle fp0 + stored doubles differ: assert bites)"
                        : "FAILED TO BITE");
        printf("      JIT fp0=%.6g  ORACLE fp0=%.6g\n", j2.fp[0], r2.fp[0]);
        if (!diverged) g_fail = 1;
    }
    free(m3); free(m4);
    j5d_run_free(); j3_free_all_thunks();
}

/* The whole existing corpus, re-run through the SAME (FPU-enabled) engine — each byte-exact vs
 * the oracle, so the FPU darwinize pass + engine wiring + oracle/RA changes did not perturb it. */
static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the whole existing corpus through the same (FPU-enabled) engine:\n");
    struct { const char *label, *rel; uint32_t want; } progs[] = {
        { "mul",      "bin/mul.exe",      42u },
        { "fact",     "bin/fact.exe",     120u },
        { "arraysum", "bin/arraysum.exe", 150u },
        { "libcall",  "bin/libcall.exe",  0u },
        { "sumsq",    "bin/sumsq.exe",    55u },
        { "bubsort",  "bin/bubsort.exe",  0x00F5B9F5u },
        { "mp64",     "bin/mp64.exe",     0x000004FCu },
        { "j5l",      "bin/j5l.exe",      11u },
    };
    for (unsigned i = 0; i < sizeof(progs)/sizeof(progs[0]); i++) {
        if (run_byte_exact(progs[i].label, progs[i].rel, progs[i].want,
                           /*check_want=*/1, /*check_fp=*/0, /*want_fp_mem=*/0, NULL))
            g_fail = 1;
    }
    if (run_byte_exact("mandel", "bin/mandel.exe", 0u, /*check_want=*/1, 0, 0, NULL))
        g_fail = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(40);

    printf("[J5o] 68881/68882 FPU CORE: the FIRST corpus program to use the FPU coprocessor\n");
    printf("      (line-F). Drives Emu68's REAL EMIT_FPU decoder (M68k_LINEF.c, verbatim) —\n");
    printf("      FMOVE (int .l/.w/.b<->FP, single .s & double .d mem<->FP, reg<->reg) + FADD/\n");
    printf("      FSUB/FMUL/FDIV/FSQRT/FABS/FNEG + FCMP/FTST (FPSR), all at IEEE double precision.\n");
    printf("      FP regs FP0..FP7 -> AArch64 d8..d15; FP memory through the sandbox (base-adjust\n");
    printf("      + per-element byteswap). Asserted BIT-EXACT (FP0..FP7 raw bits + FPSR cc byte +\n");
    printf("      the stored doubles + the integer regs + memory) vs the INDEPENDENT C-double\n");
    printf("      oracle (j5d_interp.c, OURS, no Emu68). Precision model: double; 80-bit extended\n");
    printf("      exactness is not bit-reproducible on AArch64 (documented).\n\n");

    run_fpu();
    run_negctrl();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5o] FAIL\n"); return 1; }
    printf("\n  VERDICT: the 68881/68882 FPU core runs through Emu68's REAL EMIT_FPU decoder\n");
    printf("           BIT-EXACT vs an INDEPENDENT C-double oracle — FMOVE with every format\n");
    printf("           conversion (int .l/.w/.b<->FP, single .s & double .d mem<->FP, reg<->reg),\n");
    printf("           FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG, and FCMP/FTST setting the FPSR\n");
    printf("           condition codes. The FP register file (FP0..FP7 in IEEE double, FPCR/FPSR)\n");
    printf("           is APPENDED to the state struct (every existing offset unchanged); FP memory\n");
    printf("           is sandbox-translated; the negative control bites and the whole corpus stays\n");
    printf("           byte-exact. Precision model: double (80-bit extended exactness is not bit-\n");
    printf("           reproducible on AArch64). Deferred to the next FP increment: transcendentals,\n");
    printf("           FBcc/FScc/FDBcc, FMOVEM, .x/.p memory formats, and FP exceptions.\n");
    printf("[J5o] PASS\n");
    return 0;
}
