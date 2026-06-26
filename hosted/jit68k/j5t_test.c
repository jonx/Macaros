/* j5t_test.c — [J5t] THE FP CAPSTONE: run a vbcc-COMPILER-GENERATED 68k program that does
 * REAL HARDWARE floating-point work through the JIT and prove it byte-exact vs the
 * independent interpreter.  (OURS, AROS-licensed.)  This is the FP analog of [J5m] (the
 * integer capstone) and closes out the FPU goal ([J5o]-[J5t]).
 *
 * The program (apps68k/j5t.c) is compiled ON THIS MAC by the from-source 68k cross-toolchain
 *   vbcc -cpu=68020 -fpu=68881  (C -> vasm-mot asm, HARDWARE line-F FP)
 *     ->  vasm -m68882 (asm -> vobj)  ->  vlink (vobj -> AmigaOS hunk .exe)
 * (tools/build-vbcc.sh, rebuilt with dtgen cross=y for big-endian FP constants, +
 * tools/compile-j5t.sh -> apps68k/bin/j5t.exe).  It is then loaded by the [J4] loader,
 * relocated into the sandbox, and run THROUGH the [J5d] engine — Emu68's REAL EMIT_FPU
 * decoder for the line-F FP arithmetic + our FP shim (FSQRT/FSIN/FETOX) + the (An)/(d16,An)/
 * (d8,An,Xn) sandbox EA + the [J3] LVO bridge for the integer-ized PutChar output.
 *
 * The FP it exercises (the compiler lowers each to the full m68k FP convention — fmovem.x
 * FP prologues/epilogues, double stack args, fp0 returns):
 *   (1) NEWTON'S METHOD sqrt vs the HARDWARE fsqrt shim  (fdiv/fadd/fmul + fcmp.d/fbge/fblt)
 *   (2) a TAYLOR series for exp vs the HARDWARE fetox shim (fmul/fdiv/fadd + fcmp/fbcc)
 *   (3) VECTOR STATISTICS mean/variance/stddev (stddev via the HARDWARE fsqrt)
 *   (4) a sin() table via the HARDWARE fsin shim
 * All results are integer-ized (scaled x1000/x1000000, double->int via the HARDWARE fintrz.x
 * + fmove.l fp0,d0) and printed via the existing integer PutChar path — so the deferred
 * FP->decimal packed-decimal (.p) format is NOT needed.
 *
 * PASS == BOTH:
 *   (a) the JIT run is BYTE-EXACT vs the independent from-scratch interpreter
 *       (j5d_interp.c, OURS, NO Emu68) on: the 16 integer registers, the FP register file
 *       (FP0..FP7 binary64 bits) + FPSR cc/FPCR, the ENTIRE sandbox memory, the full PutChar
 *       output stream, and the exit d0; AND
 *   (b) a NEGATIVE CONTROL — corrupting one byte of the loaded code before BOTH runs makes
 *       them DIVERGE (so the byte-exact assert genuinely bites).
 * Watchdog: SIGALRM hard-kills a hang -> [J5t] FAIL.
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static void watchdog(int sig){ (void)sig;
    const char *m = "[J5t] FAIL: watchdog timeout\n"; write(2, m, strlen(m)); _exit(2); }

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00040000u
#define LIBBASE         0x00230000u
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u

static const char *apps_dir(void)
{ const char *d = getenv("APPS68K_DIR"); return (d && *d) ? d : "."; }

/* The library bridge: marshal 68k regs into the stub PutChar via the [J3] thunk. */
struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{ struct bctx *c = user; return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el); }

/* Load j5t.exe into a fresh sandbox `mem` and set up a fresh stub library. */
static int load(uint8_t *mem, j4_sandbox *sb, j4_seglist *seg, stub_lib *lib, char *err, unsigned el)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/bin/j5t.exe", apps_dir());
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, el, "cannot open %s", path); return 1; }
    static uint8_t buf[1 << 16];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    j4_sandbox_init(sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    if (j4_load_hunks(sb, buf, len, /*skip_reloc=*/0, seg, err, el)) return 1;
    if (stublib_init(lib, sb, LIBBASE, HEAP_BASE, HEAP_END)) { snprintf(err, el, "stublib_init"); return 1; }
    return 0;
}

/* Run from entry through either the JIT (interp==0) or the interpreter (interp==1). */
static int runit(int interp, uint8_t *mem, struct j5d_m68k_state *st, uint32_t *d0,
                 stub_lib *lib, char *err, unsigned el)
{
    j4_sandbox sb; j4_seglist seg;
    if (load(mem, &sb, &seg, lib, err, el)) return -1;
    struct bctx c = { lib, &sb };
    j5d_sandbox j5sb = { sb.host_mem, sb.sandbox_origin, sb.size };
    memset(st, 0, sizeof *st);
    return interp
        ? j5d_interp_run(&j5sb, seg.entry, LIBBASE, st, d0, bridge, &c, err, el)
        : j5d_run(&j5sb, seg.entry, LIBBASE, st, d0, bridge, &c, err, el);
}

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}
/* [J5t] compare the FP register file (FP0..FP7 binary64 bits) + the FPSR cc byte + FPCR.
 * The quotient byte (23..16) of the FPSR is not asserted (the oracle doesn't model it; same
 * carve-out as [J5p]/[J5s]). */
static int eq_fp(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) {
        uint64_t ba, bb; memcpy(&ba, &a->fp[i], 8); memcpy(&bb, &b->fp[i], 8);
        if (ba != bb) return 0;
    }
    if ((a->fpsr & ~0x00FF0000u) != (b->fpsr & ~0x00FF0000u)) return 0;
    if (a->fpcr != b->fpcr) return 0;
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(40);

    printf("[J5t] FP CAPSTONE: a vbcc-COMPILED 68k program doing REAL HARDWARE floating-point\n");
    printf("      work (Newton's-method sqrt, a Taylor exp series, vector mean/variance/stddev,\n");
    printf("      a sin table) run through the JIT, byte-exact vs an independent interpreter.\n");
    printf("      The C `double` arithmetic was lowered by vbcc -fpu=68881 to LINE-F FP opcodes\n");
    printf("      (FMOVE/FADD/FSUB/FMUL/FDIV/FCMP/FBcc/FMOVEM/fintrz) + the hardware\n");
    printf("      transcendentals FSQRT/FSIN/FETOX; results integer-ized for the PutChar output.\n\n");

    uint8_t *mem_jit = malloc(SANDBOX_SIZE);
    uint8_t *mem_int = malloc(SANDBOX_SIZE);
    if (!mem_jit || !mem_int) { printf("[J5t] FAIL: oom\n"); return 1; }

    /* ---- (1) the JIT run ---- */
    struct j5d_m68k_state jit; uint32_t jd0 = 0; stub_lib jlib; char je[256] = {0};
    int jrc = runit(0, mem_jit, &jit, &jd0, &jlib, je, sizeof je);
    j5d_stats s; j5d_get_stats(&s);

    /* ---- (2) the independent interpreter run ---- */
    struct j5d_m68k_state ref; uint32_t rd0 = 0; stub_lib rlib; char re[256] = {0};
    int rrc = runit(1, mem_int, &ref, &rd0, &rlib, re, sizeof re);

    int regs_ok = (jrc == 0) && (rrc == 0) && eq_regs(&jit, &ref);
    int fp_ok   = (jrc == 0) && (rrc == 0) && eq_fp(&jit, &ref);
    int mem_ok  = (jrc == 0) && (rrc == 0) && (memcmp(mem_jit, mem_int, SANDBOX_SIZE) == 0);
    int out_ok  = (jrc == 0) && (rrc == 0) && (jlib.outlen == rlib.outlen) &&
                  (memcmp(jlib.out, rlib.out, jlib.outlen) == 0);
    int d0_ok   = (jrc == 0) && (rrc == 0) && (jd0 == rd0);

    printf("    toolchain: vbcc (m68020 + 68881 hardware FP, dtgen cross=y) -> vasm -m68882 -> "
           "vlink, all from source; program apps68k/j5t.c -> bin/j5t.exe\n");
    printf("    through the JIT: %u blocks (real Emu68 EMIT_FPU decoders), %u executed, %u m68k "
           "insns, %u FP-mem ops, %u (An)/EA mem accesses, %u PutChar calls bridged, %u AArch64 words\n",
           s.blocks_translated, s.blocks_executed, s.insns_decoded, s.fp_movem_ops,
           s.mem_accesses, s.lib_calls, s.arm_words_emitted);
    if (jrc) printf("    JIT run error: %s\n", je);
    if (rrc) printf("    interp run error: %s\n", re);
    printf("    [JIT]    exit d0=%u (0x%X), %d bytes of output\n", jd0, jd0, jlib.outlen);
    printf("    [interp] exit d0=%u (0x%X), %d bytes of output\n", rd0, rd0, rlib.outlen);
    printf("    --- the program's output (printed by the JITed FP code via PutChar) ---\n");
    printf("%.*s", jlib.outlen, jlib.out);
    printf("    ----------------------------------------------------------------------\n");
    printf("    byte-exact JIT vs interpreter: int-regs=%s fp-regs=%s memory=%s output=%s exit-d0=%s\n",
           regs_ok ? "YES" : "NO", fp_ok ? "YES" : "NO", mem_ok ? "YES" : "NO",
           out_ok ? "YES" : "NO", d0_ok ? "YES" : "NO");

    int pass1 = regs_ok && fp_ok && mem_ok && out_ok && d0_ok;

    /* ---- (3) NEGATIVE CONTROL: corrupt one code byte; the runs MUST diverge. ---- */
    int neg_bites = 0;
    {
        uint8_t *mj = malloc(SANDBOX_SIZE), *mi = malloc(SANDBOX_SIZE);
        struct j5d_m68k_state nj, ni; uint32_t nd0j = 0, nd0i = 0; stub_lib lj, li;
        char e1[256] = {0}, e2[256] = {0};
        j4_sandbox sbj, sbi; j4_seglist sgj, sgi;
        char path[1024]; snprintf(path, sizeof path, "%s/bin/j5t.exe", apps_dir());
        FILE *f = fopen(path, "rb"); static uint8_t buf[1<<16];
        size_t len = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
        if (mj && mi && len) {
            j4_sandbox_init(&sbj, mj, SANDBOX_ORIGIN, SANDBOX_SIZE);
            j4_sandbox_init(&sbi, mi, SANDBOX_ORIGIN, SANDBOX_SIZE);
            if (!j4_load_hunks(&sbj, buf, len, 0, &sgj, e1, sizeof e1) &&
                !j4_load_hunks(&sbi, buf, len, 0, &sgi, e2, sizeof e2)) {
                /* flip a byte well inside _main's compiled FP body of BOTH sandboxes identically.
                 * _main is at entry+0x410 (jsr _main from _start); +0x416 lands inside its
                 * fmovem.x/movem.l prologue + first FP setup — corrupting it changes the run. */
                uint8_t *cj = (uint8_t*)j4_sandbox_host(&sbj, sgj.entry + 0x416);
                uint8_t *ci = (uint8_t*)j4_sandbox_host(&sbi, sgi.entry + 0x416);
                if (cj && ci) { *cj ^= 0xFF; *ci ^= 0xFF; }
                stublib_init(&lj, &sbj, LIBBASE, HEAP_BASE, HEAP_END);
                stublib_init(&li, &sbi, LIBBASE, HEAP_BASE, HEAP_END);
                struct bctx bj = { &lj, &sbj }, bi = { &li, &sbi };
                j5d_sandbox jsbj = { sbj.host_mem, sbj.sandbox_origin, sbj.size };
                j5d_sandbox jsbi = { sbi.host_mem, sbi.sandbox_origin, sbi.size };
                memset(&nj, 0, sizeof nj); memset(&ni, 0, sizeof ni);
                int rj = j5d_run(&jsbj, sgj.entry, LIBBASE, &nj, &nd0j, bridge, &bj, e1, sizeof e1);
                int ri = j5d_interp_run(&jsbi, sgi.entry, LIBBASE, &ni, &nd0i, bridge, &bi, e2, sizeof e2);
                int diverged = (nd0j != nd0i) || (rj != ri) ||
                               (lj.outlen != li.outlen) ||
                               (memcmp(lj.out, li.out, lj.outlen < li.outlen ? lj.outlen : li.outlen) != 0) ||
                               !eq_regs(&nj, &ni) || !eq_fp(&nj, &ni);
                neg_bites = diverged;
            }
        }
        printf("    negative control (one code byte corrupted in both runs): %s\n",
               neg_bites ? "JIT and interpreter DIVERGE as required (assert bites)"
                         : "did NOT diverge (assert would be vacuous!)");
        free(mj); free(mi);
    }

    j5d_run_free(); j3_free_all_thunks();
    free(mem_jit); free(mem_int);

    if (pass1 && neg_bites) {
        printf("\n[J5t] PASS: the vbcc-compiled HARDWARE-FP 68k hunk executable ran THROUGH THE "
               "JIT (real Emu68 EMIT_FPU decoder + our FP shim + the [J3] PutChar bridge) and is "
               "BYTE-EXACT vs the independent interpreter on the integer registers, the FP register "
               "file (FP0..FP7 + FPSR/FPCR), the whole sandbox memory, the full %d-byte PutChar "
               "output stream, and exit d0=%u; the negative control diverges. COMPILER-generated "
               "Amiga FLOATING-POINT code runs on Apple silicon via the 68k JIT.\n",
               jlib.outlen, jd0);
        return 0;
    }
    printf("\n[J5t] FAIL: %s%s%s%s%s%s\n",
           pass1 ? "" : "byte-exact mismatch; ",
           regs_ok ? "" : "int-regs differ; ",
           fp_ok ? "" : "fp-regs differ; ",
           mem_ok ? "" : "memory differs; ",
           out_ok ? "" : "output differs; ",
           neg_bites ? "" : "negative control did not bite; ");
    return 1;
}
