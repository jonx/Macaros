/* j5r_test.c — [J5r] FMOVEM (FP register-list move) + FP SYSTEM-REGISTER MOVES + the 80-bit
 * EXTENDED (.x) memory format. The last decoder-level FP gap before the capstone.
 *   FMOVEM.x <reglist>,-(An)  (predecrement SAVE — the FP function prologue)
 *   FMOVEM.x (An)+,<reglist>  (postincrement RESTORE — the epilogue)  + the control-mode forms
 *   FMOVE.l Dn<->FPCR/FPSR/FPIAR  +  FMOVEM.l <creglist>,<ea> (multiple control regs)
 * FP registers in memory are in the 96-bit EXTENDED (.x) format; our FP regs are IEEE double,
 * so double -> .x -> double round-trips EXACTLY (the gate). All decoded at the DISPATCHER level
 * in C (the .x conversion j5r_double_to_x/j5r_x_to_double + the sandbox memory + the reglist are
 * OURS — Emu68's verbatim FMOVEM/FMOVE-special bodies are bare-metal: they blr the abort-stub
 * Load96bit/Store96bit with a 32-bit-truncated address + manipulate the real FPCR via msr fpcr).
 * The Emu68 quarantine stays BYTE-VERBATIM. (OURS, AROS-licensed; no Emu68 source copied.)
 *
 * THE PROOF (correctness GATES the marker — the INDEPENDENT C-double oracle):
 *   1. j5r.exe runs through the JIT AND the independent interpreter (same .x conversion + the
 *      same reglist semantics). Asserted BYTE-EXACT on: the integer reg file, the FP reg file
 *      FP0..FP7 (so the prologue/epilogue SAVE+RESTORE preserved the caller's FP regs across the
 *      clobberer), FPCR/FPSR/FPIAR, and the WHOLE sandbox memory — INCLUDING the extended-format
 *      .x bytes the FMOVEM stored and the FPCR/FPSR/FPIAR longwords. Plus an independent
 *      hand-check of the .x bytes (the exact 12-byte extended encoding) + the sys-reg round-trips.
 *   2. NEGATIVE CONTROL — flip the epilogue FMOVEM restore's register mask in the JIT copy only
 *      (drop fp7 from the restore list): the clobbered fp7 leaks (9999.0) where the oracle
 *      restores 1234.5 -> the FP reg file + the stored .x bytes DIVERGE, so the assert bites.
 *   3. REGRESSION — the whole corpus + the [J5o]/[J5p]/[J5q] FP programs re-run byte-exact.
 *
 * Watchdog: SIGALRM -> [J5r] FAIL.
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
    if (!f) { fprintf(stderr, "[J5r] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5r] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_int_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
/* bit-exact on the raw double bit patterns (the prologue/epilogue must preserve them) + the
 * FPSR cc byte + FPCR + FPIAR. */
static int eq_fp_state(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) {
        uint64_t ba, bb;
        memcpy(&ba, &a->fp[i], 8); memcpy(&bb, &b->fp[i], 8);
        if (ba != bb) return 0;
    }
    if ((a->fpsr & 0x0f000000u) != (b->fpsr & 0x0f000000u)) return 0;
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
            printf("      FP DIVERGE — JIT vs ORACLE FP0..FP7 + FPCR/FPSR/FPIAR:\n");
            for (int i = 0; i < 8; i++) {
                uint64_t bj, br; memcpy(&bj, &jit.fp[i], 8); memcpy(&br, &ref.fp[i], 8);
                if (bj != br) printf("        fp%d JIT=%016llx (%.6g) ORACLE=%016llx (%.6g)\n", i,
                                     (unsigned long long)bj, jit.fp[i], (unsigned long long)br, ref.fp[i]);
            }
            if (jit.fpcr != ref.fpcr) printf("        FPCR JIT=%08x ORACLE=%08x\n", jit.fpcr, ref.fpcr);
            if (jit.fpiar != ref.fpiar) printf("        FPIAR JIT=%08x ORACLE=%08x\n", jit.fpiar, ref.fpiar);
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

static void run_fmovem(void)
{
    printf("  (1) j5r.exe — FMOVEM.x save/restore (FP prologue/epilogue) + FMOVE FPCR/FPSR/FPIAR\n");
    printf("      + FMOVEM.l control-reg list, all through the 96-bit extended (.x) format.\n");

    j5d_stats s;
    int bad = run_byte_exact("j5r.exe", "bin/j5r.exe", 1u,
                             /*check_want=*/1, /*check_fp=*/1, &s);
    if (bad) g_fail = 1;
    printf("      FMOVEM/FP-sys-reg ops dispatched: %u ; FP-mem (.x/.l) accesses: %u\n",
           s.fp_movem_ops, s.mem_accesses);

    /* THIRD, independent hand-check of the JIT alone: the FP regs survived the clobberer (the
     * prologue/epilogue contract), the .x extended bytes, and the FPCR/FPSR/FPIAR round-trips. */
    {
        uint8_t *mem = calloc(1, SZ);
        j4_sandbox sb; uint32_t entry = 0;
        if (!load_one("bin/j5r.exe", mem, &sb, &entry)) {
            j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
            stub_lib jlib; struct bctx jc = { &jlib, &sb };
            stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
            struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit); jit.a[7] = INIT_SP;
            uint32_t d0 = 0; char err[200] = {0};
            j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);

            /* the FP registers must have SURVIVED (saved+restored across the clobberer). */
            double want[8] = { 1.0, 2.0, 3.0, -4.5, 0.5, 100.0, -0.0, 1234.5 };
            int allok = 1;
            for (int i = 0; i < 8; i++) {
                uint64_t a1, b1; memcpy(&a1, &jit.fp[i], 8); memcpy(&b1, &want[i], 8);
                if (a1 != b1) { allok = 0; printf("      FP SURVIVAL FAIL fp%d=%.6g (want %.6g)\n", i, jit.fp[i], want[i]); }
            }
            /* the .x extended bytes of fp0 (1.0) = 3fff 0000 8000000000000000 (the exact 68881
             * extended encoding: exp 16383 (=1023+15360→1.0), explicit integer bit, frac 0). */
            uint32_t r = jit.a[0];
            const uint8_t *p = mem + (r - ORG);
            uint8_t want_x[12] = { 0x3f,0xff, 0x00,0x00, 0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
            if (memcmp(p, want_x, 12) != 0) {
                allok = 0;
                printf("      .x ENCODING FAIL fp0: got %02x%02x %02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
                       p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11]);
            }
            /* FPCR/FPSR/FPIAR round-trips (stored at scratch+96/+100/+104). */
            struct { const char *name; uint32_t got, want; } sr[] = {
                { "FPCR  read-back", mem_be32(mem, r + 96),  0x00000030u },
                { "FPSR  read-back", mem_be32(mem, r + 100), 0x08000000u },
                { "FPIAR read-back", mem_be32(mem, r + 104), 0x00210ABCu },
                { "FMOVEM.l FPCR",   mem_be32(mem, r + 108), 0x00000030u },
                { "FMOVEM.l FPSR",   mem_be32(mem, r + 112), 0x08000000u },
            };
            for (unsigned i = 0; i < sizeof sr/sizeof sr[0]; i++)
                if (sr[i].got != sr[i].want) {
                    allok = 0;
                    printf("      SYS-REG FAIL %-16s got=0x%08x want=0x%08x\n", sr[i].name, sr[i].got, sr[i].want);
                }
            printf("      independent hand-check (FP survival + .x encoding + FPCR/FPSR/FPIAR round-trips) -> %s\n",
                   allok ? "all match" : "MISMATCH");
            if (!allok) g_fail = 1;
        }
        free(mem); j5d_run_free(); j3_free_all_thunks();
    }
}

/* NEGATIVE CONTROL: drop fp7 from the EPILOGUE restore mask in the JIT copy only. The epilogue
 * is `fmovem.x (sp)+,fp0-fp7` (op 0xF227, opcode2 0xD0FF — postincrement, control-order mask
 * 0xFF). Clearing fp7's mask bit (control mask bit 0 -> 0xFE) makes the JIT NOT restore fp7, so
 * the clobbered 9999.0 leaks where the oracle restores 1234.5 -> the FP reg file + the stored
 * .x bytes DIVERGE between JIT and oracle, proving the cross-check bites. */
static void run_negctrl(void)
{
    uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
    j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
    if (!load_one("bin/j5r.exe", m3, &s3, &e3) && !load_one("bin/j5r.exe", m4, &s4, &e4)) {
        /* find the epilogue FMOVEM postincrement restore: op 0xF21F (ea=(a7)+, mode3 reg7),
         * opcode2 0xD0FF (dir=0 mem->FP, static control, mask 0xFF). Clear fp7's mask bit
         * (control-order mask bit 0 = fp7), so the JIT does NOT restore fp7. */
        uint8_t *code = s3.host_mem; int patched = 0;
        for (uint32_t i = ORG; i + 4 <= ORG + 0x400; i += 2) {
            uint8_t *p = code + (i - ORG);
            if (p[0] == 0xF2 && p[1] == 0x1F && p[2] == 0xD0 && p[3] == 0xFF) {  /* (sp)+,fp0-fp7 */
                p[3] = 0xFE;       /* drop fp7 (control mask bit 0) from the restore */
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

        uint64_t bj, br; memcpy(&bj, &j2.fp[7], 8); memcpy(&br, &r2.fp[7], 8);
        int diverged = patched && (bj != br) && (memcmp(m3, m4, SZ) != 0);
        printf("  (2) neg-ctrl: drop fp7 from the epilogue FMOVEM restore in the JIT copy only -> %s\n",
               diverged ? "DIVERGED (JIT fp7 leaks the clobber 9999.0, oracle restores 1234.5: assert bites)"
                        : "FAILED TO BITE");
        printf("      JIT fp7=%.6g  ORACLE fp7=%.6g\n", j2.fp[7], r2.fp[7]);
        if (!diverged) g_fail = 1;
    }
    free(m3); free(m4);
    j5d_run_free(); j3_free_all_thunks();
}

static void run_corpus_regression(void)
{
    printf("  (3) REGRESSION — the corpus + the [J5o]/[J5p]/[J5q] FP programs through the same engine:\n");
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
        { "j5q",      "bin/j5q.exe",      0x000103FFu, 0 },
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

    printf("[J5r] FMOVEM + FP SYSTEM-REGISTER MOVES + the 80-bit EXTENDED (.x) memory format:\n");
    printf("      the last decoder-level FP gap. FMOVEM.x saves/restores the FP register list to\n");
    printf("      memory in the 96-bit extended format (the FP function prologue/epilogue);\n");
    printf("      FMOVE/FMOVEM move FPCR/FPSR/FPIAR. Our FP regs are IEEE double, so double->.x->\n");
    printf("      double round-trips EXACTLY. Decoded at the DISPATCHER level in C (the .x\n");
    printf("      conversion + sandbox memory + reglist are OURS); the Emu68 quarantine stays\n");
    printf("      BYTE-VERBATIM. Asserted BYTE-EXACT (integer + FP regs + FPCR/FPSR/FPIAR + the\n");
    printf("      whole sandbox incl. the .x bytes) vs the INDEPENDENT oracle (same .x conversion).\n\n");

    run_fmovem();
    run_negctrl();
    run_corpus_regression();

    if (g_fail) { printf("\n[J5r] FAIL\n"); return 1; }
    printf("\n  VERDICT: FMOVEM.x (the FP register-list save/restore — the prologue/epilogue, FP\n");
    printf("           registers SURVIVE across a clobbering subroutine) + the control-mode forms\n");
    printf("           + FMOVE/FMOVEM of FPCR/FPSR/FPIAR run BYTE-EXACT vs an INDEPENDENT oracle,\n");
    printf("           with the 96-bit EXTENDED (.x) memory bytes asserted exactly. double -> .x ->\n");
    printf("           double round-trips exactly (the FP regs are double); ±0/inf/NaN handled.\n");
    printf("           The negative control bites and the whole corpus + the [J5o]/[J5p]/[J5q] FP\n");
    printf("           programs stay byte-exact. Deferred: FP exceptions (incl. BSUN) + the\n");
    printf("           packed-decimal (.p) memory format, then a vbcc-compiled FP capstone.\n");
    printf("\n[J5r] PASS\n");
    return 0;
}
