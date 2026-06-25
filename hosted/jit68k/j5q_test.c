/* j5q_test.c — [J5q] 68881/68882 FP CONDITIONAL CONTROL-FLOW coverage: FBcc, FScc, FDBcc,
 * FTRAPcc. These read the FPSR condition byte (N/Z/NAN) the preceding FCMP/FTST produced
 * (made live + verified in [J5o]/[J5p]) and branch/set/trap on the 68881 FP predicate. They
 * are decoded at the DISPATCHER level in C (the way the integer Bcc is — j5d_engine.c), NOT
 * through Emu68's bare-metal REG_PC branch funnel (FBcc/FScc emit it; FDBcc/FTRAPcc have no
 * decoder body at all). The FP predicate (j5q_fp_cond_taken, shared header) is OUR re-derivation
 * of the table Emu68's verbatim FBcc decoder emits in AArch64, evaluated in C over {N,Z,NAN}.
 * (OURS, AROS-licensed; no Emu68 source copied.)
 *
 * THE LOAD-BEARING PART — IEEE UNORDERED (NaN): the ORDERED predicates (OGT/OR/…) are FALSE on
 * a NaN operand; the UNORDERED predicates (UN/ULE/UGT/…) are TRUE on NaN. j5q.s FTSTs a NaN and
 * the ordered-vs-unordered predicates MUST take opposite paths — the byte-exact d0 + control
 * flow gate that.
 *
 * THE PROOF (correctness GATES the marker — the INDEPENDENT C-double oracle):
 *   1. j5q.exe runs through the JIT (REAL EMIT_FPU for the FCMP/FTST bodies + OUR dispatcher
 *      for the FP control-flow) AND through the independent interpreter (j5d_interp.c, same
 *      predicate table). Asserted BYTE-EXACT on the integer reg file, the FP reg file, the FPSR
 *      cc byte, the WHOLE sandbox memory (incl. the FScc-stored bytes + the FDBcc count + the
 *      FTRAPcc frame), and the exit d0. d0 == 0x000103FF (every FBcc path + the trap marker).
 *   2. NEGATIVE CONTROL — flip FBOR (predicate 0x07, ordered) -> FBUN (0x08, unordered) in the
 *      JIT copy ONLY: on the NaN FTST the JIT's now-FBUN branches to the fail label (d0=-1)
 *      where the oracle's FBOR falls through (d0=0x103FF) -> the assert bites.
 *   3. REGRESSION — the whole corpus + the [J5o] FP core + the [J5p] transcendentals re-run
 *      through the SAME engine, each byte-exact vs the oracle.
 *
 * Watchdog: SIGALRM -> [J5q] FAIL.
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
    if (!f) { fprintf(stderr, "[J5q] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5q] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_int_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
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
 * integer regs + FP regs + FPSR + the FULL sandbox memory are byte-exact and d0 agrees. The
 * supervisor stack is seeded at INIT_SP (a7) so the FTRAPcc frame push has room. */
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
            printf("      FP DIVERGE — JIT vs ORACLE FP0..FP7 + FPSR cc:\n");
            for (int i = 0; i < 8; i++) {
                uint64_t bj, br; memcpy(&bj, &jit.fp[i], 8); memcpy(&br, &ref.fp[i], 8);
                if (bj != br) printf("        fp%d JIT=%016llx ORACLE=%016llx\n", i,
                                     (unsigned long long)bj, (unsigned long long)br);
            }
            printf("        FPSR cc JIT=%02x ORACLE=%02x\n",
                   (jit.fpsr >> 24) & 0x0f, (ref.fpsr >> 24) & 0x0f);
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

static void run_fpcc(void)
{
    printf("  (1) j5q.exe — FBcc/FScc/FDBcc/FTRAPcc reading the FPSR cc; incl. NaN/unordered.\n");

    j5d_stats s;
    /* d0 == 0x000103FF: every FBcc path (1023) + the FTRAPcc handler marker (0x10000). */
    int bad = run_byte_exact("j5q.exe", "bin/j5q.exe", 0x000103FFu,
                             /*check_want=*/1, /*check_fp=*/1, &s);
    if (bad) g_fail = 1;
    printf("      FP control-flow ops dispatched (FBcc/FScc/FDBcc/FTRAPcc): %u ; FTRAPcc->vector7 fired: %u\n",
           s.fp_cc_ops, s.exceptions_dispatched);

    /* THIRD, independent hand-check of the JIT alone: the FScc bytes + the FDBcc count + the
     * ordered/unordered NaN paths (independent of the oracle). */
    {
        uint8_t *mem = calloc(1, SZ);
        j4_sandbox sb; uint32_t entry = 0;
        if (!load_one("bin/j5q.exe", mem, &sb, &entry)) {
            j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
            stub_lib jlib; struct bctx jc = { &jlib, &sb };
            stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
            struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit); jit.a[7] = INIT_SP;
            uint32_t d0 = 0; char err[200] = {0};
            j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);

            uint32_t r = jit.a[0];
            const uint8_t *p = mem + (r - ORG);
            struct { const char *name; unsigned got, want; } chk[] = {
                { "FScc fseq(F)->0x00",       p[0], 0x00 },
                { "FScc fsun(NaN,T)->0xFF",   p[1], 0xFF },
                { "FScc fsor(NaN,F)->0x00",   p[2], 0x00 },
                { "FScc d3 fslt(T)->0xFF",    p[3], 0xFF },
                { "FScc d4 fsgt(F)->0x00",    p[4], 0x00 },
                { "FDBcc loop iters",         mem_be32(mem, r + 8), 4 },
                { "exit d0 (paths+trap)",     d0, 0x000103FFu },
                { "d5 FDBcc counter (.W -1)", jit.d[5], 0x0000FFFFu },
                { "d7 FTRAPcc handler marker",jit.d[7], 0x00010000u },
            };
            int allok = 1;
            for (unsigned i = 0; i < sizeof chk/sizeof chk[0]; i++)
                if (chk[i].got != chk[i].want) {
                    allok = 0;
                    printf("      EXPECT FAIL %-26s got=0x%x want=0x%x\n",
                           chk[i].name, chk[i].got, chk[i].want);
                }
            printf("      independent hand-check (FScc bytes + FDBcc count + ordered/unordered NaN paths + FTRAPcc) -> %s\n",
                   allok ? "all match" : "MISMATCH");
            if (!allok) g_fail = 1;
        }
        free(mem); j5d_run_free(); j3_free_all_thunks();
    }
}

/* NEGATIVE CONTROL: flip FBOR (predicate 0x07, ORDERED) -> FBUN (0x08, UNORDERED) in the JIT
 * copy ONLY. The program FTSTs a NaN then `fbor lfail`; ordered-OR is FALSE on NaN (falls
 * through, d0 continues to 0x103FF). The patched JIT runs FBUN, which is TRUE on NaN -> branches
 * to lfail -> d0 = -1. The oracle (un-patched FBOR) reaches 0x103FF. So the byte-exact d0 + the
 * control flow DIVERGE between JIT and oracle — proving the FP-predicate cross-check bites. */
static void run_negctrl(void)
{
    uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
    j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
    if (!load_one("bin/j5q.exe", m3, &s3, &e3) && !load_one("bin/j5q.exe", m4, &s4, &e4)) {
        /* find FBOR.W: op = 0xF280 | 0x07 = 0xF287 (FBcc base 0xF280, bit7 always set; predicate
         * = op & 0x3f = 0x07; bit6 = size, 0 = .W). Bytes 0xF2 0x87. Patch the predicate 0x07 ->
         * 0x08 (FBUN), preserving the size/base bits (0x80) -> low byte 0x88, in the JIT copy only. */
        uint8_t *code = s3.host_mem; int patched = 0;
        for (uint32_t i = ORG; i + 2 <= ORG + 0x300; i += 2) {
            uint8_t *p = code + (i - ORG);
            if (p[0] == 0xF2 && (p[1] & 0x3f) == 0x07 && (p[1] & 0x40) == 0x00) {  /* FBOR.W */
                p[1] = (uint8_t)((p[1] & 0xC0) | 0x08);   /* FBOR (0x07) -> FBUN (0x08) */
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
        printf("  (2) neg-ctrl: FBOR->FBUN in the JIT copy only (ordered->unordered on NaN) -> %s\n",
               diverged ? "DIVERGED (JIT takes the unordered path, oracle the ordered: assert bites)"
                        : "FAILED TO BITE");
        printf("      JIT d0=0x%08x  ORACLE d0=0x%08x\n", jd, rdv);
        if (!diverged) g_fail = 1;
    }
    free(m3); free(m4);
    j5d_run_free(); j3_free_all_thunks();
}

/* The whole corpus + the [J5o] FP core + the [J5p] transcendentals, re-run through the SAME
 * engine — each byte-exact vs the oracle, so the [J5q] dispatcher/oracle additions perturb
 * nothing (FP control-flow is additive + opcode-gated). */
static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the corpus + the [J5o]/[J5p] FP programs through the same engine:\n");
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

    printf("[J5q] 68881/68882 FP CONDITIONAL CONTROL-FLOW: FBcc/FScc/FDBcc/FTRAPcc read the FPSR\n");
    printf("      condition byte (N/Z/NAN — made live + verified in [J5o]/[J5p]) and branch/set/\n");
    printf("      trap on the 68881 FP predicate. Decoded at the DISPATCHER level in C (the way\n");
    printf("      integer Bcc is), evaluating the predicate over {N,Z,NAN}; the ORDERED predicates\n");
    printf("      are FALSE on NaN, the UNORDERED ones TRUE on NaN. Asserted BYTE-EXACT (integer +\n");
    printf("      FP regs + FPSR cc + the whole sandbox + d0) + correct control flow vs the\n");
    printf("      INDEPENDENT oracle (j5d_interp.c, same predicate table). The Emu68 quarantine\n");
    printf("      stays BYTE-VERBATIM (the predicate + the control-flow are OURS in the dispatcher).\n\n");

    run_fpcc();
    run_negctrl();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5q] FAIL\n"); return 1; }
    printf("\n  VERDICT: the 68881/68882 FP conditional control-flow family (FBcc .W/.L, FScc to\n");
    printf("           Dn/memory, FDBcc decrement-and-branch, FTRAPcc -> vector 7) runs BYTE-EXACT\n");
    printf("           + correct control flow vs an INDEPENDENT oracle, with the IEEE unordered\n");
    printf("           (NaN) predicates verified to take the opposite path from the ordered ones.\n");
    printf("           The FPSR cc the [J5o]/[J5p] FP ops produce drives the predicate; the\n");
    printf("           negative control bites and the whole corpus + the [J5o]/[J5p] FP programs\n");
    printf("           stay byte-exact. Deferred: FMOVEM + FP system-register moves, the 80-bit .x\n");
    printf("           / packed .p memory formats, FP exceptions (BSUN on the signalling\n");
    printf("           predicates), then a vbcc-compiled FP capstone.\n");
    printf("\n[J5q] PASS\n");
    return 0;
}
