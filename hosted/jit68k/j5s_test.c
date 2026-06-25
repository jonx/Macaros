/* j5s_test.c — [J5s] the 68881/68882 FP EXCEPTION MODEL: the FPSR exception (EXC) + accrued
 * (AEXC) bytes, the FPCR exception-enable + rounding-mode/precision bytes, the FP exception
 * traps (vectors 48..54), and BSUN. Each exception is raised from a REAL cause and the FPSR is
 * read back + asserted BIT-EXACT (EXC + AEXC) vs the INDEPENDENT oracle (j5d_interp.c); the four
 * rounding modes round 1/3 differently; a trap-enabled OPERR fires vector 52 through a planted
 * handler (observed via the [J5i] exception path). (OURS, AROS-licensed; no Emu68 source.)
 *
 * THE fenv MAPPING (host <fenv.h> -> 68k FPSR EXC), the load-bearing realization (j5s_fpu_exc.h):
 * the JIT runs the FP arithmetic NATIVELY; the host AArch64 FP hardware honours the FPCR rounding
 * we set with fesetround and ACCUMULATES the IEEE cumulative-exception flags, which we read back
 * with fetestexcept and map into the FPSR EXC byte. The oracle does the SAME fenv dance per-op, so
 * the asserted FPSR EXC + AEXC is bit-exact. (SNAN vs OPERR is split by an operand sNaN scan, done
 * identically on both sides; single-precision rounding rounds the result through float.)
 *
 * THE PROOF (correctness GATES the marker):
 *   1. j5s.exe runs through the JIT (REAL EMIT_FPU for the FP arithmetic + OUR dispatcher for the
 *      FP exception model) AND through the independent oracle. Asserted BYTE-EXACT on the integer
 *      regs, the FP regs, the FULL FPSR (cc + EXC + AEXC), FPCR/FPIAR, and the WHOLE sandbox memory
 *      (incl. the stored FPSR longwords, the four rounding-mode doubles, and the trap frame/marker).
 *   2. A THIRD, fully-independent hand-check verifies each EXC test's EXC + AEXC bits, the four
 *      rounding-mode 1/3 doubles (RP differs from RN/RZ/RM in the last bit), BSUN, and the trap.
 *   3. NEGATIVE CONTROL — clear the OPERR-enable bit in the JIT copy ONLY: the trap-enabled 0/0 no
 *      longer fires vector 52, so the handler marker is absent -> JIT vs oracle DIVERGE (assert bites).
 *   4. REGRESSION — the whole corpus + the [J5o]/[J5p]/[J5q]/[J5r] FP programs re-run byte-exact.
 *
 * Watchdog: SIGALRM -> [J5s] FAIL.
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j5s_fpu_exc.h"
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
#define INIT_SP   0x00250000u

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
    if (!f) { fprintf(stderr, "[J5s] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5s] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_int_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
/* [J5s] compare the FP register file AND the FULL FPSR (cc + EXC + AEXC) + FPCR + FPIAR. */
static int eq_fp_state(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) {
        uint64_t ba, bb;
        memcpy(&ba, &a->fp[i], 8); memcpy(&bb, &b->fp[i], 8);
        if (ba != bb) return 0;
    }
    /* the WHOLE FPSR EXCEPT the QUOTIENT byte (bits 23..16): the FMOD/FREM quotient byte is
     * computed by the native decoder but is documented (since [J5p]) as NOT asserted — the oracle
     * does not model it. We assert cc (27..24) + EXC (15..8) + AEXC (7..0). */
    if ((a->fpsr & ~0x00FF0000u) != (b->fpsr & ~0x00FF0000u)) return 0;
    if (a->fpcr != b->fpcr) return 0;
    if (a->fpiar != b->fpiar) return 0;
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
 * integer regs + FP regs + FULL FPSR + the FULL sandbox memory are byte-exact and d0 agrees. */
static int run_byte_exact(const char *label, const char *rel, uint32_t want, int check_want,
                          int check_fp, j5d_stats *out_s)
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
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit); jit.a[7] = INIT_SP;
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s); if (out_s) *out_s = s;

    stub_lib rlib; struct bctx rc_ = { &rlib, &sb2 };
    stublib_init(&rlib, &sb2, LIBBASE, HEAP_BASE, HEAP_END);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref); ref.a[7] = INIT_SP;
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, entry2, LIBBASE, &ref, &rd0, bridge, &rc_, e2, sizeof e2);

    int regs_ok = (rc == 0) && (irc == 0) && eq_int_regs(&jit, &ref);
    int fp_ok   = !check_fp || ((rc == 0) && (irc == 0) && eq_fp_state(&jit, &ref));
    int mem_ok  = (memcmp(mem, mem2, SZ) == 0);
    int d0_ok   = (rc == 0) && (d0 == rd0) && (!check_want || d0 == want);
    ok_overall = regs_ok && fp_ok && mem_ok && d0_ok;

    printf("    %-12s JIT d0=0x%08x ORACLE d0=0x%08x | byte-exact: regs=%s%s mem=%s -> %s\n",
           label, d0, rd0, regs_ok ? "yes" : "NO",
           check_fp ? (fp_ok ? " fp=yes" : " fp=NO") : "",
           mem_ok ? "yes" : "NO", ok_overall ? "OK" : "FAIL");
    if (!ok_overall) {
        if (rc)  printf("      JIT err: %s\n", err);
        if (irc) printf("      oracle err: %s\n", e2);
        if (check_want && d0 != want) printf("      expected d0=0x%08x\n", want);
        if (check_fp && !fp_ok) {
            for (int i = 0; i < 8; i++) {
                uint64_t bj, br; memcpy(&bj, &jit.fp[i], 8); memcpy(&br, &ref.fp[i], 8);
                if (bj != br) printf("        fp%d JIT=%016llx ORACLE=%016llx\n", i,
                                     (unsigned long long)bj, (unsigned long long)br);
            }
            printf("        FPSR JIT=%08x ORACLE=%08x  FPCR JIT=%08x ORACLE=%08x\n",
                   jit.fpsr, ref.fpsr, jit.fpcr, ref.fpcr);
        }
    }
out:
    free(mem); free(mem2);
    j5d_run_free(); j3_free_all_thunks();
    return ok_overall ? 0 : 1;
}

static uint32_t mem_be32(const uint8_t *mem, uint32_t addr)
{
    const uint8_t *p = mem + (addr - ORG);
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint64_t mem_be64(const uint8_t *mem, uint32_t addr)
{
    return ((uint64_t)mem_be32(mem, addr) << 32) | mem_be32(mem, addr + 4);
}

/* The EXC + AEXC bits the harness EXPECTS for each test, computed INDEPENDENTLY from the cause
 * (so the hand-check does not rely on the oracle). EXC is bits 15..8, AEXC bits 7..0. */
static void run_exc_handcheck(void)
{
    printf("  (1) j5s.exe — FPSR EXC + AEXC bits per real FP exception cause (bit-exact vs oracle).\n");

    j5d_stats s;
    /* d0 == 0x00000034 = the OPERR trap vector number (52), set by the planted handler. */
    int bad = run_byte_exact("j5s.exe", "bin/j5s.exe", 0x00000034u,
                             /*check_want=*/1, /*check_fp=*/1, &s);
    if (bad) g_fail = 1;
    printf("      FP-mem ops: %u ; exceptions dispatched (BSUN-set + the OPERR trap): %u\n",
           s.fp_movem_ops, s.exceptions_dispatched);

    /* Independent hand-check of the JIT alone: read the stored FPSR longwords + rounding doubles. */
    uint8_t *mem = calloc(1, SZ);
    j4_sandbox sb; uint32_t entry = 0;
    if (!load_one("bin/j5s.exe", mem, &sb, &entry)) {
        j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
        stub_lib jlib; struct bctx jc = { &jlib, &sb };
        stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
        struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit); jit.a[7] = INIT_SP;
        uint32_t d0 = 0; char err[200] = {0};
        j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);

        uint32_t r = jit.a[0];                         /* a0 = result frame base */
        uint32_t fpsr_operr = mem_be32(mem, r + 0);
        uint32_t fpsr_dz    = mem_be32(mem, r + 4);
        uint32_t fpsr_ovfl  = mem_be32(mem, r + 8);
        uint32_t fpsr_unfl  = mem_be32(mem, r + 12);
        uint32_t fpsr_inex  = mem_be32(mem, r + 16);
        uint32_t fpsr_snan  = mem_be32(mem, r + 20);
        uint64_t rn = mem_be64(mem, r + 24);
        uint64_t rz = mem_be64(mem, r + 32);
        uint64_t rm = mem_be64(mem, r + 40);
        uint64_t rp = mem_be64(mem, r + 48);
        uint32_t fpsr_bsun  = mem_be32(mem, r + 56);
        uint32_t trap_marker= mem_be32(mem, r + 60);

        /* The EXC + AEXC bits each cause must show (independent of the oracle). We mask the EXC
         * byte (15..8) + AEXC byte (7..0); the cc byte (27..24) varies per op and is not asserted
         * here (the byte-exact vs oracle in (1) already covered the whole FPSR). */
        struct { const char *name; uint32_t got, mask, want; } chk[] = {
            { "OPERR EXC",  fpsr_operr, J5S_FPSR_EXC_MASK,  J5S_FPSR_OPERR },
            { "OPERR AEXC", fpsr_operr, J5S_FPSR_AEXC_MASK, J5S_FPSR_AIOP },
            { "DZ EXC",     fpsr_dz,    J5S_FPSR_EXC_MASK,  J5S_FPSR_DZ },
            { "DZ AEXC",    fpsr_dz,    J5S_FPSR_AEXC_MASK, J5S_FPSR_ADZ },
            { "OVFL EXC",   fpsr_ovfl,  J5S_FPSR_EXC_MASK,  J5S_FPSR_OVFL | J5S_FPSR_INEX2 },
            { "OVFL AEXC",  fpsr_ovfl,  J5S_FPSR_AEXC_MASK, J5S_FPSR_AOVFL | J5S_FPSR_AINEX },
            { "UNFL EXC",   fpsr_unfl,  J5S_FPSR_EXC_MASK,  J5S_FPSR_UNFL | J5S_FPSR_INEX2 },
            { "UNFL AEXC",  fpsr_unfl,  J5S_FPSR_AEXC_MASK, J5S_FPSR_AUNFL | J5S_FPSR_AINEX },
            { "INEX EXC",   fpsr_inex,  J5S_FPSR_EXC_MASK,  J5S_FPSR_INEX2 },
            { "INEX AEXC",  fpsr_inex,  J5S_FPSR_AEXC_MASK, J5S_FPSR_AINEX },
            { "SNAN EXC",   fpsr_snan,  J5S_FPSR_EXC_MASK,  J5S_FPSR_SNAN },
            { "SNAN AEXC",  fpsr_snan,  J5S_FPSR_AEXC_MASK, J5S_FPSR_AIOP },
            { "BSUN EXC",   fpsr_bsun,  J5S_FPSR_BSUN,      J5S_FPSR_BSUN },
            { "BSUN AEXC",  fpsr_bsun,  J5S_FPSR_AIOP,      J5S_FPSR_AIOP },
            { "OPERR trap vector52", trap_marker, 0xFFFFFFFFu, 0x00000034u },
        };
        int allok = 1;
        for (unsigned i = 0; i < sizeof chk/sizeof chk[0]; i++)
            if ((chk[i].got & chk[i].mask) != chk[i].want) {
                allok = 0;
                printf("      EXPECT FAIL %-22s got=0x%08x (&0x%08x)=0x%08x want=0x%08x\n",
                       chk[i].name, chk[i].got, chk[i].mask, chk[i].got & chk[i].mask, chk[i].want);
            }
        /* the four rounding modes: 1/3. RN/RZ/RM agree (round-to-nearest == round-down for this
         * value); RP rounds the last mantissa bit UP, so it differs. */
        int round_ok = (rn == 0x3fd5555555555555ull) && (rz == 0x3fd5555555555555ull) &&
                       (rm == 0x3fd5555555555555ull) && (rp == 0x3fd5555555555556ull);
        if (!round_ok) {
            allok = 0;
            printf("      ROUNDING FAIL  RN=%016llx RZ=%016llx RM=%016llx RP=%016llx\n",
                   (unsigned long long)rn, (unsigned long long)rz,
                   (unsigned long long)rm, (unsigned long long)rp);
        } else {
            printf("      rounding 1/3:  RN=RZ=RM=...5555  RP=...5556 (RP rounds up) -> distinct\n");
        }
        printf("      independent hand-check (EXC+AEXC bits per cause + 4 rounding modes + BSUN + trap) -> %s\n",
               allok ? "all match" : "MISMATCH");
        if (!allok) g_fail = 1;
    }
    free(mem); j5d_run_free(); j3_free_all_thunks();
}

/* NEGATIVE CONTROL: clear the OPERR-enable bit in the JIT copy ONLY. The trap-enabled 0/0 then
 * does NOT fire vector 52, so the handler marker (result[60]) stays 0 in the JIT but the oracle
 * (un-patched, OPERR enabled) fires the trap -> the byte-exact sandbox + d0 DIVERGE. We patch the
 * `move.l #$00002000,d1` immediate (the OPERR-enable load) to #$00000000 in the JIT copy. */
static void run_negctrl(void)
{
    uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
    j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
    if (!load_one("bin/j5s.exe", m3, &s3, &e3) && !load_one("bin/j5s.exe", m4, &s4, &e4)) {
        /* find the longword immediate 0x00002000 in the code (the OPERR-enable FPCR value) and
         * zero it in the JIT copy only. move.l #imm,d1 = 0x223C imm32. */
        uint8_t *code = s3.host_mem; int patched = 0;
        for (uint32_t i = ORG; i + 6 <= ORG + 0x400; i += 2) {
            uint8_t *p = code + (i - ORG);
            if (p[0] == 0x22 && p[1] == 0x3C &&
                p[2] == 0x00 && p[3] == 0x00 && p[4] == 0x20 && p[5] == 0x00) {  /* move.l #$2000,d1 */
                p[4] = 0x00; p[5] = 0x00;   /* enable bits -> 0 (OPERR disable) */
                patched = 1; break;
            }
        }
        j5d_sandbox a2 = { s3.host_mem, s3.sandbox_origin, s3.size };
        j5d_sandbox b2 = { s4.host_mem, s4.sandbox_origin, s4.size };
        struct j5d_m68k_state j2, r2; memset(&j2,0,sizeof j2); memset(&r2,0,sizeof r2);
        j2.a[7] = INIT_SP; r2.a[7] = INIT_SP;
        uint32_t jd=0, rdv=0; char x1[200]={0}, x2[200]={0};
        stub_lib jl2, rl2; stublib_init(&jl2,&s3,LIBBASE,HEAP_BASE,HEAP_END);
        stublib_init(&rl2,&s4,LIBBASE,HEAP_BASE,HEAP_END);
        struct bctx jc2 = { &jl2, &s3 }, rc2 = { &rl2, &s4 };
        j5d_run(&a2, e3, LIBBASE, &j2, &jd, bridge, &jc2, x1, sizeof x1);
        j5d_interp_run(&b2, e4, LIBBASE, &r2, &rdv, bridge, &rc2, x2, sizeof x2);

        int diverged = patched && (jd != rdv) && (memcmp(m3, m4, SZ) != 0);
        printf("  (2) neg-ctrl: clear OPERR-enable in the JIT copy only (trap-enabled 0/0 no longer\n");
        printf("      fires vector 52) -> %s\n",
               diverged ? "DIVERGED (oracle fires the trap + marker, JIT does not: assert bites)"
                        : "FAILED TO BITE");
        printf("      JIT d0=0x%08x  ORACLE d0=0x%08x\n", jd, rdv);
        if (!diverged) g_fail = 1;
    }
    free(m3); free(m4);
    j5d_run_free(); j3_free_all_thunks();
}

/* The whole corpus + the [J5o]..[J5r] FP programs through the SAME engine — each byte-exact vs the
 * oracle, so the [J5s] exception-model additions perturb nothing (the FP exception model is
 * additive + opcode-gated: a non-FP block emits zero FP load/store and never touches fenv). */
static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the corpus + the [J5o]/[J5p]/[J5q]/[J5r] FP programs:\n");
    struct { const char *label, *rel; uint32_t want; int fp; } progs[] = {
        { "mul",      "bin/mul.exe",      42u,         0 },
        { "fact",     "bin/fact.exe",     120u,        0 },
        { "arraysum", "bin/arraysum.exe", 150u,        0 },
        { "libcall",  "bin/libcall.exe",  0u,          0 },
        { "sumsq",    "bin/sumsq.exe",    55u,         0 },
        { "bubsort",  "bin/bubsort.exe",  0x00F5B9F5u, 0 },
        { "mp64",     "bin/mp64.exe",     0x000004FCu, 0 },
        { "j5l",      "bin/j5l.exe",      11u,         0 },
        { "j5o",      "bin/j5o.exe",      0u,          1 },
        { "j5p",      "bin/j5p.exe",      0u,          1 },
        { "j5q",      "bin/j5q.exe",      0x000103FFu, 1 },
        { "j5r",      "bin/j5r.exe",      1u,          1 },
    };
    for (unsigned i = 0; i < sizeof(progs)/sizeof(progs[0]); i++) {
        if (run_byte_exact(progs[i].label, progs[i].rel, progs[i].want,
                           /*check_want=*/1, progs[i].fp, NULL))
            g_fail = 1;
    }
    if (run_byte_exact("mandel", "bin/mandel.exe", 0u, /*check_want=*/1, 0, NULL))
        g_fail = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(40);

    printf("[J5s] 68881/68882 FP EXCEPTION MODEL: the FPSR exception (EXC) + accrued (AEXC) bytes,\n");
    printf("      the FPCR exception-enable + rounding-mode/precision bytes, the FP exception traps\n");
    printf("      (vectors 48..54: BSUN/INEX/DZ/UNFL/OPERR/OVFL/SNAN), and BSUN (the signalling FP\n");
    printf("      predicate's unordered trap). The JIT runs the FP arithmetic NATIVELY; we set the\n");
    printf("      host rounding direction (fesetround) before the block and read the host fenv\n");
    printf("      exceptions (fetestexcept) after, mapping them to the FPSR EXC byte; the oracle does\n");
    printf("      the SAME fenv dance per-op, so the asserted FPSR EXC + AEXC is BIT-EXACT. Grounded:\n");
    printf("      FPSR/FPCR bit layout + the FP vectors against the AROS m68k FPSP (fpsp.h/FPSP.sa).\n\n");

    run_exc_handcheck();
    run_negctrl();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5s] FAIL\n"); return 1; }
    printf("\n  VERDICT: the 68881/68882 FP exception model runs BYTE-EXACT vs an INDEPENDENT oracle:\n");
    printf("           OPERR (0/0), DZ (1/0), OVFL, UNFL, INEX, SNAN (signalling-NaN operand) each\n");
    printf("           set the right FPSR EXC + AEXC bits; the four rounding modes round 1/3 distinctly\n");
    printf("           (RP differs); a TRAP-ENABLED OPERR fires vector 52 through the [J5i] path; and\n");
    printf("           a SIGNALLING FP predicate on a NaN sets BSUN. The negative control bites and the\n");
    printf("           whole corpus + the [J5o]/[J5p]/[J5q]/[J5r] FP programs stay byte-exact. The\n");
    printf("           packed-decimal (.p) memory format is DEFERRED (precise rationale in spec [J5s]);\n");
    printf("           it is the one remaining FP memory format. Next: the vbcc-compiled FP capstone.\n");
    printf("\n[J5s] PASS\n");
    return 0;
}
