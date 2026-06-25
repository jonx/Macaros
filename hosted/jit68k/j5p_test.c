/* j5p_test.c — [J5p] 68881/68882 TRANSCENDENTAL + FP-UTILITY coverage. Drives Emu68's REAL
 * EMIT_FPU decoder (the verbatim, quarantined M68k_LINEF.c) for the 68881 transcendentals
 * (FSIN/FCOS/FTAN/FASIN/FACOS/FATAN, FSINH/FCOSH/FTANH/FATANH, FETOX/FETOXM1/FTWOTOX/FTENTOX,
 * FLOGN/FLOGNP1/FLOG10/FLOG2, FSINCOS) and the FP-utility ops (FINT/FINTRZ/FGETEXP/FGETMAN/
 * FMOD/FREM/FSCALE), each routed to the HOST libm (the transcendental helper sites bake in
 * &sin/&cos/... at translate time; the routing is in j5o_fpu_shims.c — the standard libm names
 * resolve to the system libm; exp10/sincos/remquo are our thin wrappers). The INDEPENDENT
 * C-double oracle (j5d_interp.c, OURS, no Emu68) computes every op with the SAME host libm.
 * (OURS, AROS-licensed; calls the adopted emitter, copies no Emu68 source.)
 *
 * PRECISION MODEL: FP0..FP7 are modeled in IEEE-754 binary64 (`double`) — like Emu68 + the
 * [J5o] core. The 68881 transcendentals are implementation-defined in their last ULPs, so the
 * faithful hosted realization routes BOTH the JIT and the oracle to the SAME host libm. The
 * assert is bit-exact and verifies the TRANSLATION (correct decode + the right register passed
 * as the argument + the correct store), NOT a re-derivation of sin(). (Real 68881 used 80-bit +
 * its own polynomials; not bit-reproducible on AArch64 — consistent with the [J5o] model.)
 *
 * THE PROOF (correctness GATES the marker — the INDEPENDENT C-double oracle):
 *   1. j5p.exe runs through the JIT (REAL EMIT_FPU + dispatcher + sandbox FP-mem EA) AND the
 *      independent interpreter. Asserted BYTE-EXACT on: the integer reg file, the FP reg file
 *      FP0..FP7 (raw `double` bit patterns — so NaN/-0.0/inf compare by BITS), the FPSR cc byte
 *      (N/Z/I/NAN — incl. the NaN edge cases FACOS(10)/FLOGN(-1)/FATANH(10)), the WHOLE sandbox
 *      memory (incl. the FP-to-memory stored doubles), and the exit d0. AND >=1 FP memory access
 *      went through the JIT.
 *   2. NEGATIVE CONTROL — flip ONE transcendental opcode (FSIN opmode 0x0e -> FCOS 0x1d) in the
 *      JIT copy ONLY: the JIT computes cos(0.5) where the oracle computes sin(0.5) -> the FP reg
 *      file + stored doubles DIVERGE between JIT and oracle, proving the cross-check bites.
 *   3. REGRESSION — the whole existing corpus + the [J5o] FP core re-run through the SAME engine,
 *      each byte-exact vs the oracle (transcendentals are additive + opcode-gated).
 *
 * Watchdog: SIGALRM -> [J5p] FAIL.
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
    if (!f) { fprintf(stderr, "[J5p] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5p] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_int_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
/* bit-exact on the raw double bit patterns (NaN/-0.0/inf by bits) + the FPSR cc byte. */
static int eq_fp_state(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) {
        uint64_t ba, bb;
        memcpy(&ba, &a->fp[i], 8); memcpy(&bb, &b->fp[i], 8);
        if (ba != bb) return 0;
    }
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
 * integer regs + FP regs + FPSR + the FULL sandbox memory are byte-exact and d0 agrees. */
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
                printf("        fp%d JIT=%016llx (%.9g)  ORACLE=%016llx (%.9g)%s\n", i,
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

/* host-libm hand-check of a stored double in the RESULT area (a0 = lea result), big-endian. */
static double mem_double(const uint8_t *mem, uint32_t addr)
{
    const uint8_t *p = mem + (addr - ORG);
    uint64_t bits = ((uint64_t)p[0]<<56)|((uint64_t)p[1]<<48)|((uint64_t)p[2]<<40)|((uint64_t)p[3]<<32)
                  | ((uint64_t)p[4]<<24)|((uint64_t)p[5]<<16)|((uint64_t)p[6]<<8)|(uint64_t)p[7];
    double d; memcpy(&d, &bits, 8); return d;
}

/* extern host libm — the SAME reference the JIT's blr + the oracle reach. */
extern double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double);
extern double sinh(double), cosh(double), tanh(double), atanh(double);
extern double exp(double), expm1(double), exp2(double), pow(double,double);
extern double log(double), log1p(double), log10(double), log2(double);
extern double rint(double), trunc(double), remainder(double,double), ldexp(double,int);

static int bits_eq(double a, double b)
{ uint64_t x,y; memcpy(&x,&a,8); memcpy(&y,&b,8); return x==y; }

static void run_fpu(void)
{
    printf("  (1) j5p.exe — transcendentals (FSIN/FCOS/FTAN/FASIN/FACOS/FATAN, FSINH/FCOSH/FTANH/\n");
    printf("      FATANH, FETOX/FETOXM1/FTWOTOX/FTENTOX, FLOGN/FLOGNP1/FLOG10/FLOG2, FSINCOS) +\n");
    printf("      FP-utility (FINT/FINTRZ/FGETEXP/FGETMAN/FMOD/FREM/FSCALE), routed to host libm.\n");

    j5d_stats s;
    int bad = run_byte_exact("j5p.exe", "bin/j5p.exe", 0u,
                             /*check_want=*/1, /*check_fp=*/1, /*want_fp_mem=*/1, &s);
    if (bad) g_fail = 1;
    printf("      FP memory accesses through the JIT (sandbox FP-mem EA): %u\n", s.mem_accesses);

    /* ---- THIRD, fully-independent check: re-run the JIT alone, verify each stored double
     * against the host-libm hand-computed expectation (independent of the oracle). ---- */
    {
        uint8_t *mem = calloc(1, SZ);
        j4_sandbox sb; uint32_t entry = 0;
        if (!load_one("bin/j5p.exe", mem, &sb, &entry)) {
            j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
            stub_lib jlib; struct bctx jc = { &jlib, &sb };
            stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
            struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
            uint32_t d0 = 0; char err[200] = {0};
            j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);

            uint32_t r = jit.a[0];   /* a0 -> result area at exit */
            struct { const char *name; double got, want; } chk[] = {
                { "sin(0.5)",      mem_double(mem, r+0),   sin(0.5)   },
                { "cos(0.5)",      mem_double(mem, r+8),   cos(0.5)   },
                { "tan(0.5)",      mem_double(mem, r+16),  tan(0.5)   },
                { "asin(0.5)",     mem_double(mem, r+24),  asin(0.5)  },
                { "acos(0.5)",     mem_double(mem, r+32),  acos(0.5)  },
                { "atan(0.5)",     mem_double(mem, r+40),  atan(0.5)  },
                { "sinh(0.5)",     mem_double(mem, r+48),  sinh(0.5)  },
                { "cosh(0.5)",     mem_double(mem, r+56),  cosh(0.5)  },
                { "tanh(0.5)",     mem_double(mem, r+64),  tanh(0.5)  },
                { "atanh(0.5)",    mem_double(mem, r+72),  atanh(0.5) },
                { "exp(1.0)",      mem_double(mem, r+80),  exp(1.0)   },
                { "expm1(1.0)",    mem_double(mem, r+88),  expm1(1.0) },
                { "exp2(1.0)",     mem_double(mem, r+96),  exp2(1.0)  },
                { "exp10(1.0)",    mem_double(mem, r+104), pow(10.0,1.0) },
                { "log(10.0)",     mem_double(mem, r+112), log(10.0)  },
                { "log1p(10.0)",   mem_double(mem, r+120), log1p(10.0)},
                { "log10(10.0)",   mem_double(mem, r+128), log10(10.0)},
                { "log2(10.0)",    mem_double(mem, r+136), log2(10.0) },
                { "sincos.sin",    mem_double(mem, r+144), sin(0.5)   },
                { "sincos.cos",    mem_double(mem, r+152), cos(0.5)   },
                { "fint(2.5)",     mem_double(mem, r+160), rint(2.5)  },
                { "fintrz(2.5)",   mem_double(mem, r+168), trunc(2.5) },
                { "fgetexp(12.0)", mem_double(mem, r+176), 3.0        },
                { "fgetman(12.0)", mem_double(mem, r+184), 1.5        },
                { "fmod(7,3)",     mem_double(mem, r+192), 1.0        },
                { "frem(7,3)",     mem_double(mem, r+200), remainder(7.0,3.0) },
                { "fscale(1.5,3)", mem_double(mem, r+208), ldexp(1.5,3) },
            };
            int allok = 1;
            for (unsigned i = 0; i < sizeof chk/sizeof chk[0]; i++) {
                if (!bits_eq(chk[i].got, chk[i].want)) {
                    allok = 0;
                    printf("      EXPECT FAIL %-14s got=%.17g want=%.17g\n",
                           chk[i].name, chk[i].got, chk[i].want);
                }
            }
            /* NaN edge cases: each stored result must be a NaN (bit-exact to the host libm). */
            struct { const char *name; double got, want; } nanchk[] = {
                { "acos(10.0)",  mem_double(mem, r+216), acos(10.0)  },
                { "ln(-1.0)",    mem_double(mem, r+224), log(-1.0)   },
                { "atanh(10.0)", mem_double(mem, r+232), atanh(10.0) },
            };
            for (unsigned i = 0; i < sizeof nanchk/sizeof nanchk[0]; i++) {
                int got_nan = (nanchk[i].got != nanchk[i].got);
                if (!got_nan || !bits_eq(nanchk[i].got, nanchk[i].want)) {
                    allok = 0;
                    printf("      EXPECT FAIL %-12s NaN-edge: got=%.17g (want NaN)\n",
                           nanchk[i].name, nanchk[i].got);
                }
            }
            printf("      independent host-libm hand-check (27 transcendental/utility + 3 NaN edges) -> %s\n",
                   allok ? "all match" : "MISMATCH");

            /* FPSR: the LAST FPSR-setting op was FATANH(10.0)=NaN consumed by FMOVE->MEM. The
             * unordered fcmpzd sets the NAN bit (V->bit24); EMIT_GetFPUFlags CLEARS the AArch64
             * C bit, so the 68881 I (infinity) bit stays 0 (verified empirically — see the FPSR
             * model note in j5d_interp.c). So the cc byte for a NaN result is exactly NAN=1. */
            int nan_set = (jit.fpsr >> 24) & 1, i_set = (jit.fpsr >> 25) & 1;
            int n_set = (jit.fpsr >> 27) & 1, z_set = (jit.fpsr >> 26) & 1;
            printf("      FPSR after FATANH(NaN)+FMOVE->MEM: N=%d Z=%d I=%d NAN=%d (expect NAN=1, rest 0)\n",
                   n_set, z_set, i_set, nan_set);
            if (!nan_set || i_set || n_set || z_set) allok = 0;
            if (!allok) g_fail = 1;
        }
        free(mem); j5d_run_free(); j3_free_all_thunks();
    }
}

/* NEGATIVE CONTROL: flip the FSIN opcode (opmode 0x0e) -> FCOS (0x1d) in the JIT copy ONLY, so
 * the JIT computes cos(0.5) while the oracle computes sin(0.5) — the FP reg file + stored doubles
 * must DIVERGE between JIT and oracle. (The FSIN command word is 0x0?0e -> low byte 0x0e; FCOS is
 * 0x1d. We find the opcode2 whose low byte is 0x0e following an 0xF200 line-F word.) */
static void run_negctrl(void)
{
    uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
    j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
    if (!load_one("bin/j5p.exe", m3, &s3, &e3) && !load_one("bin/j5p.exe", m4, &s4, &e4)) {
        uint8_t *code = s3.host_mem; int patched = 0;
        for (uint32_t i = ORG; i + 4 <= ORG + 0x400; i += 2) {
            uint8_t *p = code + (i - ORG);
            /* line-F coprocessor 1 (0xF2 0x00) + an opcode2 whose low 7 bits (the opmode) == 0x0e
             * (FSIN). For `fsin.x fp0,fp1` opcode2 = 0x008e (bit7 = the dst-reg low bit), so match
             * (p[3] & 0x7f) == 0x0e and (p[2] & 0x20) == 0 (R/M=0, src is the FP reg). Patch the
             * opmode 0x0e -> 0x1d (FCOS), preserving the dst bit (p[3] & 0x80). */
            if (p[0] == 0xF2 && p[1] == 0x00 && (p[3] & 0x7f) == 0x0e && (p[2] & 0x20) == 0x00) {
                p[3] = (uint8_t)((p[3] & 0x80) | 0x1d);   /* FSIN (0x0e) -> FCOS (0x1d) */
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

        /* fp1 is REUSED later in j5p.s (overwritten by a NaN), so compare the STORED double at
         * result+0 — the sin/cos result written right after the first FSIN, before fp1 is reused.
         * The JIT (FSIN->FCOS patched) stores cos(0.5); the oracle (un-patched) stores sin(0.5). */
        double js = mem_double(m3, j2.a[0] + 0), rs = mem_double(m4, r2.a[0] + 0);
        int diverged = patched && !bits_eq(js, rs) && (memcmp(m3, m4, SZ) != 0);
        printf("  (2) neg-ctrl: FSIN->FCOS in the JIT copy only -> %s\n",
               diverged ? "DIVERGED (JIT stored cos(0.5) != oracle stored sin(0.5): assert bites)"
                        : "FAILED TO BITE");
        printf("      JIT stored[0]=%.9g (cos 0.5)  ORACLE stored[0]=%.9g (sin 0.5)\n", js, rs);
        if (!diverged) g_fail = 1;
    }
    free(m3); free(m4);
    j5d_run_free(); j3_free_all_thunks();
}

/* The whole existing corpus + the [J5o] FP core, re-run through the SAME engine — each byte-exact
 * vs the oracle, so the [J5p] routing + oracle changes did not perturb anything. */
static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the whole existing corpus + [J5o] FP core through the same engine:\n");
    struct { const char *label, *rel; uint32_t want; int fp; } progs[] = {
        { "mul",      "bin/mul.exe",      42u,         0 },
        { "fact",     "bin/fact.exe",     120u,        0 },
        { "arraysum", "bin/arraysum.exe", 150u,        0 },
        { "libcall",  "bin/libcall.exe",  0u,          0 },
        { "sumsq",    "bin/sumsq.exe",    55u,         0 },
        { "bubsort",  "bin/bubsort.exe",  0x00F5B9F5u, 0 },
        { "mp64",     "bin/mp64.exe",     0x000004FCu, 0 },
        { "j5l",      "bin/j5l.exe",      11u,         0 },
        { "j5o",      "bin/j5o.exe",      0u,          1 },   /* the [J5o] FP core, FP-checked */
    };
    for (unsigned i = 0; i < sizeof(progs)/sizeof(progs[0]); i++) {
        if (run_byte_exact(progs[i].label, progs[i].rel, progs[i].want,
                           /*check_want=*/1, progs[i].fp, /*want_fp_mem=*/0, NULL))
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

    printf("[J5p] 68881/68882 TRANSCENDENTAL + FP-UTILITY: drives Emu68's REAL EMIT_FPU decoder\n");
    printf("      (M68k_LINEF.c, verbatim) for the 68881 transcendentals + FP-utility ops, ROUTED\n");
    printf("      to the HOST libm (the helper sites bake in &sin/&cos/... at translate time; the\n");
    printf("      routing is in j5o_fpu_shims.c). Asserted BIT-EXACT (FP0..FP7 raw bits + FPSR cc\n");
    printf("      byte + the stored doubles + the integer regs + memory) vs the INDEPENDENT\n");
    printf("      C-double oracle (j5d_interp.c, OURS, no Emu68) using the SAME host libm.\n");
    printf("      Precision model: double; the 68881 transcendentals are implementation-defined in\n");
    printf("      their last ULPs, so host libm is the reference both sides use — bit-exact verifies\n");
    printf("      the TRANSLATION (decode + register-as-argument + store), not a re-derivation.\n\n");

    run_fpu();
    run_negctrl();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5p] FAIL\n"); return 1; }
    printf("\n  VERDICT: the 68881/68882 transcendentals (FSIN/FCOS/FTAN/FASIN/FACOS/FATAN, FSINH/\n");
    printf("           FCOSH/FTANH/FATANH, FETOX/FETOXM1/FTWOTOX/FTENTOX, FLOGN/FLOGNP1/FLOG10/\n");
    printf("           FLOG2, FSINCOS) + the FP-utility ops (FINT/FINTRZ/FGETEXP/FGETMAN/FMOD/FREM/\n");
    printf("           FSCALE) run through Emu68's REAL EMIT_FPU decoder BIT-EXACT vs an INDEPENDENT\n");
    printf("           C-double oracle, both routed to the SAME host libm. The FPSR NAN bit is\n");
    printf("           set on NaN results (FACOS(10)/FLOGN(-1)/FATANH(10)). The negative control\n");
    printf("           bites and the whole corpus + [J5o] FP core stay byte-exact. Deferred: FBcc/\n");
    printf("           FScc/FDBcc/FTRAPcc, FMOVEM, the 80-bit .x / packed .p memory formats, and\n");
    printf("           FP exceptions (the FPSR exception/accrued bits + FP traps).\n");
    printf("\n[J5p] PASS\n");
    return 0;
}
