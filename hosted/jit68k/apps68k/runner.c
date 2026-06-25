/* runner.c — the apps68k runner: load REAL vasm-assembled 68k hunk programs into the
 * [J4] sandbox (with real HUNK_RELOC32 relocation) and run EACH ONE THROUGH THE JIT via
 * the [J5d] engine — Emu68's REAL per-opcode decoders for every ALU/move/memory opcode
 * + OUR re-hosted dispatcher for control flow + the (An) sandbox-memory EA + the
 * jsr-through-vector -> [J3] library bridge. Each result is value-asserted AND compared
 * byte-exact against an INDEPENDENT from-scratch interpreter over the SAME sandbox.
 * (OURS, AROS-licensed.)
 *
 * This is the broadened [J5d] state: where the earlier runner ran only mul through the
 * JIT and the other three on the reference (NEEDS [J5c]), all FOUR now go through the
 * real-decoder JIT:
 *   mul       -> d0 = 42   (moveq/add.l/subq.l/bne.s/rts)
 *   fact      -> d0 = 120  (+ reg-to-reg move.l + cmp.l + nested loops)
 *   arraysum  -> d0 = 150  (+ lea-relocated DATA, add.l (a0)+ via the REAL EA decoder)
 *   libcall   -> d0 = 0    (+ AllocMem/PutChar/FreeMem via jsr -off(a6) -> [J3] bridge)
 *
 * Watchdog: SIGALRM hard-kills the process so the runner can never hang.
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
    const char *m = "[apps68k] FAIL: watchdog timeout\n"; write(2, m, strlen(m)); _exit(2); }

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00040000u     /* 256 KiB: code + data + lib table + heap */
#define LIBBASE         0x00230000u     /* stub library base (vectors grow downward) */
#define HEAP_BASE       0x00231000u     /* AllocMem heap                            */
#define HEAP_END        0x00238000u

static int g_fail = 0;

static const char *apps_dir(void)
{
    const char *d = getenv("APPS68K_DIR");
    return (d && *d) ? d : ".";
}

static uint8_t *read_file(const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", apps_dir(), rel);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    static uint8_t buf[1 << 16];
    *len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

/* Load a program's CODE+DATA hunks (relocated) into a caller-provided sandbox memory. */
static int load_program(const char *path, uint8_t *mem, j4_sandbox *sb, j4_seglist *seg)
{
    size_t len; uint8_t *buf = read_file(path, &len);
    if (!buf) return 1;
    j4_sandbox_init(sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    char err[200] = {0};
    if (j4_load_hunks(sb, buf, len, /*skip_reloc=*/0, seg, err, sizeof err)) {
        printf("    load error: %s\n", err); return 1;
    }
    return 0;
}

/* Bridge a loaded [J4] sandbox to the [J5d] engine's view of it. */
static j5d_sandbox to_j5d(const j4_sandbox *sb)
{
    j5d_sandbox r = { sb->host_mem, sb->sandbox_origin, sb->size };
    return r;
}

/* ---- The libcall bridge: marshal 68k regs into the native stub via the [J3] thunk. */
struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

/* ====================== a register/memory program (no library) ================= */
static void run_regprog(const char *nm, const char *path, uint32_t want, const char *note,
                        int expect_mem)
{
    printf("[apps68k] %s — %s\n", nm, note);
    uint8_t *mem  = malloc(SANDBOX_SIZE);
    uint8_t *mem2 = malloc(SANDBOX_SIZE);
    j4_sandbox sb, sb2; j4_seglist seg, seg2;
    if (load_program(path, mem, &sb, &seg) || load_program(path, mem2, &sb2, &seg2)) {
        g_fail = 1; free(mem); free(mem2); return;
    }

    /* JIT through the [J5d] engine: real Emu68 decoders + our dispatcher. */
    j5d_sandbox j5sb = to_j5d(&sb);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&j5sb, seg.entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    /* Independent reference over a SEPARATELY loaded sandbox. */
    j5d_sandbox refsb = to_j5d(&sb2);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&refsb, seg2.entry, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);

    /* arraysum: prove the loader RELOCATED the lea target into the DATA hunk. */
    int reloc_ok = 1;
    if (expect_mem) {
        const uint8_t *code = j4_sandbox_host(&sb, seg.entry);
        uint32_t lea_target = ((uint32_t)code[2]<<24)|((uint32_t)code[3]<<16)|
                              ((uint32_t)code[4]<<8)|code[5];
        reloc_ok = (seg.numhunks >= 2) && (lea_target == seg.hunk_base[1]);
        printf("    loader relocation: lea target=0x%08X DATA hunk base=0x%08X -> %s\n",
               lea_target, seg.numhunks >= 2 ? seg.hunk_base[1] : 0,
               reloc_ok ? "RELOCATED CORRECTLY" : "MISMATCH");
    }

    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int memok = (memcmp(mem, mem2, SANDBOX_SIZE) == 0);
    int memstat_ok = expect_mem ? (s.mem_accesses > 0) : 1;
    int ok = (rc == 0) && (d0 == want) && (d0 == rd0) && regs_ok && memok && reloc_ok && memstat_ok;

    printf("    [JIT] d0=%u  REF d0=%u (want %u)  regs=%s  sandbox-mem=%s\n",
           d0, rd0, want, regs_ok ? "byte-exact" : "DIVERGE", memok ? "byte-exact" : "DIVERGE");
    printf("    through the JIT: %u blocks (real Emu68 decoders), %u executed, %u m68k insns, "
           "%u (An) mem accesses, %u AArch64 words\n",
           s.blocks_translated, s.blocks_executed, s.insns_decoded, s.mem_accesses, s.arm_words_emitted);
    if (rc)  printf("    run error: %s\n", err);
    if (irc) printf("    interp error: %s\n", e2);
    printf("    -> %s (expect %u)\n", ok ? "PASS" : "FAIL", want);
    if (!ok) g_fail = 1;
    j5d_run_free();
    free(mem); free(mem2);
}

/* ====================== a subroutine program ([J5f]: nested bsr/jsr/rts) ========
 * Like run_regprog but reports the [J5f] return-stack + block-cache telemetry and
 * asserts the call nesting / SP balance / computed-jump count, on top of the byte-exact
 * register + sandbox-memory (incl. the return stack) comparison vs the reference. */
static void run_subprog(const char *nm, const char *path, uint32_t want, const char *note)
{
    printf("[apps68k] %s — %s\n", nm, note);
    uint8_t *mem  = malloc(SANDBOX_SIZE);
    uint8_t *mem2 = malloc(SANDBOX_SIZE);
    j4_sandbox sb, sb2; j4_seglist seg, seg2;
    if (load_program(path, mem, &sb, &seg) || load_program(path, mem2, &sb2, &seg2)) {
        g_fail = 1; free(mem); free(mem2); return;
    }

    j5d_sandbox j5sb = to_j5d(&sb);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&j5sb, seg.entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    j5d_sandbox refsb = to_j5d(&sb2);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&refsb, seg2.entry, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);

    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int memok = (memcmp(mem, mem2, SANDBOX_SIZE) == 0);   /* incl. the return stack */
    int stack_ok = (s.calls_pushed == s.returns_popped) && (s.calls_pushed > 0) &&
                   (s.max_call_depth >= 2) && (s.computed_jumps >= 1) &&
                   (jit.a[7] == ref.a[7]);                /* a7 balanced JIT==REF   */
    int cache_ok = (s.block_cache_hits > 0) && (s.blocks_executed > s.blocks_translated);
    int ok = (rc == 0) && (d0 == want) && (d0 == rd0) && regs_ok && memok && stack_ok && cache_ok;

    printf("    [JIT] d0=%u  REF d0=%u (want %u)  regs=%s  sandbox-mem(incl. stack)=%s\n",
           d0, rd0, want, regs_ok ? "byte-exact" : "DIVERGE", memok ? "byte-exact" : "DIVERGE");
    printf("    return stack: a7 JIT=0x%08X REF=0x%08X  pushed=%u popped=%u  max depth=%u  "
           "computed jumps=%u\n", jit.a[7], ref.a[7], s.calls_pushed, s.returns_popped,
           s.max_call_depth, s.computed_jumps);
    printf("    block cache: %u translated (misses), %u executed, %u hits -> %u re-translations avoided\n",
           s.blocks_translated, s.blocks_executed, s.block_cache_hits, s.block_cache_hits);
    if (rc)  printf("    run error: %s\n", err);
    if (irc) printf("    interp error: %s\n", e2);
    printf("    -> %s (expect %u)\n", ok ? "PASS" : "FAIL", want);
    if (!ok) g_fail = 1;
    j5d_run_free();
    free(mem); free(mem2);
}

/* ============================== libcall (library) ============================== */
static void run_libcall(void)
{
    printf("[apps68k] libcall.exe — AllocMem + PutChar('A') + FreeMem via jsr -off(a6) "
           "(jsr-vector -> [J3] bridge, decoded from the stream)\n");
    uint8_t *mem = malloc(SANDBOX_SIZE);
    j4_sandbox sb; j4_seglist seg;
    if (load_program("bin/libcall.exe", mem, &sb, &seg)) { g_fail = 1; free(mem); return; }

    stub_lib lib; char err[200] = {0};
    if (stublib_init(&lib, &sb, LIBBASE, HEAP_BASE, HEAP_END)) {
        printf("    stublib_init failed\n"); g_fail = 1; free(mem); return;
    }
    struct bctx c = { &lib, &sb };

    j5d_sandbox j5sb = to_j5d(&sb);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0;
    int rc = j5d_run(&j5sb, seg.entry, LIBBASE, &jit, &d0, bridge, &c, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    printf("    through the JIT: %u blocks (real Emu68 decoders), %u library calls bridged, "
           "%u AArch64 words\n", s.blocks_translated, s.lib_calls, s.arm_words_emitted);
    printf("    observed %d library call(s) via the [J3] marshaller:\n", lib.ncalls);
    for (int i = 0; i < lib.ncalls; i++) {
        stub_call_rec *r = &lib.calls[i];
        const char *n = r->lvo == STUB_LVO_ALLOCMEM ? "AllocMem" :
                        r->lvo == STUB_LVO_FREEMEM  ? "FreeMem"  :
                        r->lvo == STUB_LVO_PUTCHAR  ? "PutChar"  : "?";
        printf("      #%d %-9s d0=0x%08X d1=0x%08X a1=0x%08X -> ret d0=0x%08X\n",
               i, n, r->arg_d0, r->arg_d1, r->arg_a1, r->ret_d0);
    }
    int seq_ok = (lib.ncalls == 3) &&
                 lib.calls[0].lvo == STUB_LVO_ALLOCMEM &&
                 lib.calls[1].lvo == STUB_LVO_PUTCHAR  &&
                 lib.calls[2].lvo == STUB_LVO_FREEMEM;
    int alloc_ok = lib.calls[0].arg_d0 == 256 && lib.calls[0].arg_d1 == STUB_MEMF_CLEAR &&
                   lib.calls[0].ret_d0 >= HEAP_BASE && lib.calls[0].ret_d0 < HEAP_END;
    int print_ok = lib.outlen == 1 && lib.out[0] == 'A';
    int free_ok  = lib.calls[2].arg_a1 == lib.calls[0].ret_d0 &&
                   lib.calls[2].arg_d0 == 256 && lib.bytes_outstanding == 0;
    int exit_ok  = (rc == 0) && (d0 == 0);
    int ok = seq_ok && alloc_ok && print_ok && free_ok && exit_ok;
    printf("    output=\"%.*s\"  bytes_outstanding=%u  exit d0=%u\n",
           lib.outlen, lib.out, lib.bytes_outstanding, d0);
    printf("    asserts: seq=%s alloc=%s print=%s free=%s exit=%s -> %s (expect 0)\n",
           seq_ok?"ok":"X", alloc_ok?"ok":"X", print_ok?"ok":"X", free_ok?"ok":"X",
           exit_ok?"ok":"X", ok ? "PASS" : "FAIL");
    if (rc) printf("    run error: %s\n", err);
    if (!ok) g_fail = 1;
    j5d_run_free(); j3_free_all_thunks();
    free(mem);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(20);

    printf("[apps68k] runner: REAL vasm-assembled 68k hunk programs, ALL SIX through the "
           "JIT (Emu68's REAL decoders LINE0/4/5/8/9/B/C/D/E + MOVE/MULDIV + our PC-driven "
           "dispatcher with the [J5f] real return stack + the (An)/(d16,An)/(d8,An,Xn) "
           "sandbox EA + the [J3] library bridge), each byte-exact vs an independent "
           "reference.\n\n");

    run_regprog("mul.exe", "bin/mul.exe", 42,
                "7*6 by repeated addition (moveq/add.l/subq.l/bne.s/rts)", 0);
    printf("\n");
    run_regprog("fact.exe", "bin/fact.exe", 120,
                "5! via nested additive loops (move.l Dn,Dm + cmp.l + bne)", 0);
    printf("\n");
    run_regprog("arraysum.exe", "bin/arraysum.exe", 150,
                "sum {10,20,30,40,50} via add.l (a0)+,d0 (REAL EA + REV; relocated DATA)", 1);
    printf("\n");
    run_subprog("sumsq.exe", "bin/sumsq.exe", 55,
                "sum of squares 1..5 via nested bsr/jsr/rts + computed jsr(a0) ([J5f] return stack)");
    printf("\n");
    run_regprog("bubsort.exe", "bin/bubsort.exe", 0x00F5B9F5,
                "[J5g] bubble sort via (d8,An,Xn.L) indexed mem + a shift/rotate/immediate "
                "checksum (LINE0/LINE4/LINEE decoders)", 1);
    printf("\n");
    run_libcall();
    printf("\n");

    if (g_fail) {
        printf("[apps68k] FAIL: one or more programs did not produce the expected result.\n");
        return 1;
    }
    printf("[apps68k] PASS: all six REAL 68k programs (mul=42, fact=120, arraysum=150, "
           "sumsq=55, bubsort=0x00F5B9F5, libcall=0) ran THROUGH THE JIT via Emu68's REAL "
           "per-opcode decoders + our re-hosted PC-driven dispatcher (with the [J5f] real "
           "return stack for sumsq's nested bsr/jsr/rts + computed jsr, and the [J5g] "
           "LINE0/LINE4/LINEE decoders + (d8,An,Xn)/(d16,An) sandbox EA for bubsort's "
           "indexed bubble sort + shift/rotate/immediate checksum); each register file + "
           "sandbox memory (incl. the sorted array + the return stack) is byte-exact vs an "
           "independent from-scratch interpreter, and libcall's AllocMem/PutChar/FreeMem "
           "were observed via the [J3] bridge with the right args/returns.\n");
    return 0;
}
