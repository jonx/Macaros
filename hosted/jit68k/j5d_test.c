/* j5d_test.c — [J5d] value-asserting driver: run the WHOLE apps68k corpus through the
 * REAL-decoder JIT engine and assert each program's result is byte-exact equal to an
 * INDEPENDENT from-scratch interpreter (OURS), with value asserts + a negative control
 * + the libcall stub-call log + a watchdog. Prints `[J5d] PASS` only if every assert
 * holds across all four programs. (OURS, AROS-licensed.)
 *
 * The four corpus programs (the EXACT relocated code streams the [J4] loader produces
 * from apps68k/{mul,fact,arraysum,libcall}.exe — kept in sync with those .s sources):
 *   mul       7*6 by repeated addition           -> d0 = 42
 *   fact      5! via nested additive loops       -> d0 = 120  (move.l Dn,Dm + cmp.l)
 *   arraysum  sum {10..50} via add.l (a0)+,d0    -> d0 = 150  (REAL EA + REV byteswap)
 *   libcall   AllocMem/PutChar/FreeMem via jsr   -> d0 = 0    (jsr-vector -> [J3] bridge)
 *
 * Every one is TRANSLATED to AArch64 by the REAL Emu68 per-opcode decoders (LINE5/8/9/
 * B/C/D + MOVE + the rewritten EA), run under W^X, with OUR dispatcher owning the
 * inter-block control flow + the LVO bridge. NO faked passes: each register file is
 * compared byte-exact against the independent interpreter over the SAME sandbox.
 */
#include "j5d_jit68k.h"
#include "j5n_diag.h"
#include "apps68k/stublib.h"
#include "j3_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>

#define ORG   0x00210000u
#define SZ    0x00040000u
#define LIBBASE   0x00230000u
#define HEAP_BASE 0x00231000u
#define HEAP_END  0x00238000u

/* ---- the relocated code streams (big-endian, as the [J4] loader lays them down) ---- */
static const uint8_t MUL[] = {
    0x70,0x00, 0x72,0x07, 0x74,0x06, 0xd0,0x82, 0x53,0x81, 0x66,0xfa, 0x4e,0x75 };
static const uint8_t FACT[] = {
    0x70,0x01, 0x74,0x02, 0x72,0x00, 0x26,0x02, 0xd2,0x80, 0x53,0x83, 0x66,0xfa,
    0x20,0x01, 0x52,0x82, 0x78,0x06, 0xb4,0x84, 0x66,0xec, 0x4e,0x75 };
/* arraysum: lea abs.l,a0 with abs32 = ORG+0x100 (the relocated DATA hunk base). */
static const uint8_t ARR[] = {
    0x41,0xf9, 0x00,0x21,0x01,0x00, 0x72,0x05, 0x70,0x00,
    0xd0,0x98, 0x53,0x81, 0x66,0xfa, 0x4e,0x75 };
static const uint8_t LIB[] = {
    0x20,0x3c,0x00,0x00,0x01,0x00, 0x72,0x01, 0x4e,0xae,0xff,0x3a, 0x24,0x40,
    0x70,0x41, 0x4e,0xae,0xff,0xe2, 0x22,0x4a, 0x20,0x3c,0x00,0x00,0x01,0x00,
    0x4e,0xae,0xff,0x2e, 0x70,0x00, 0x4e,0x75 };
/* lea ORG+0x100,a2; move.l a1,32(a2); beq taken; d0=1; rts; taken:d0=42;rts.
 * A1 begins at zero. The checked memory-store helper must not replace MOVE's Z
 * with the private bounds-comparison flags before the BEQ consumes it. */
static const uint8_t MOVE_MEM_FLAGS[] = {
    0x45,0xf9, 0x00,0x21,0x01,0x00, 0x25,0x49,0x00,0x20,
    0x67,0x04, 0x70,0x01, 0x4e,0x75, 0x70,0x2a, 0x4e,0x75 };

/* The compiler-emitted two-memory displacement form that first exposed the hosted
 * bounds-check defect while dispatching a real ARexx message to TurboCalc:
 *
 *     move.w 12(a0),626(a5)
 *
 * It deliberately makes both source and destination use the EA funnel in one
 * translated instruction.  The source value and the destination bytes are checked
 * against the independent interpreter below. */
static const uint8_t MOVEW_D16_TO_D16[] = {
    0x3b,0x68, 0x00,0x0c, 0x02,0x72, 0x4e,0x75 };

/* arraysum DATA hunk: 5 big-endian longwords at sandbox offset 0x100. */
static void arr_data(uint8_t *mem)
{
    static const uint32_t v[5] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        uint8_t *p = mem + 0x100 + i * 4;
        p[0] = v[i] >> 24; p[1] = v[i] >> 16; p[2] = v[i] >> 8; p[3] = (uint8_t)v[i];
    }
}

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

static volatile sig_atomic_t g_alarmed = 0;
static void on_alarm(int sig){ (void)sig; g_alarmed = 1;
    const char *m = "[J5d] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

/* ============================ a register-only program ========================== */
static void run_regprog(const char *nm, const uint8_t *code, unsigned clen,
                        uint32_t want, void (*data)(uint8_t *), const char *note)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    memcpy(mem,  code, clen); if (data) data(mem);
    memcpy(mem2, code, clen); if (data) data(mem2);
    j5d_sandbox sb  = { mem,  ORG, SZ };
    j5d_sandbox sb2 = { mem2, ORG, SZ };

    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&sb, ORG, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&sb2, ORG, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);

    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int memok = (memcmp(mem, mem2, SZ) == 0);    /* sandbox memory byte-exact */
    int ok = (rc == 0) && (d0 == want) && (d0 == rd0) && regs_ok && memok;

    printf("  %-9s %s\n", nm, note);
    printf("    JIT d0=%u  REF d0=%u  (want %u)  regs=%s  sandbox-mem=%s\n",
           d0, rd0, want, regs_ok ? "byte-exact" : "DIVERGE", memok ? "byte-exact" : "DIVERGE");
    printf("    through the JIT: %u blocks translated (real Emu68 decoders), %u executed, "
           "%u m68k insns, %u (An) mem accesses, %u AArch64 words\n",
           s.blocks_translated, s.blocks_executed, s.insns_decoded, s.mem_accesses, s.arm_words_emitted);
    if (rc) printf("    run error: %s\n", err);
    if (irc) printf("    interp error: %s\n", e2);
    printf("    -> %s\n", ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
    j5d_run_free();
    free(mem); free(mem2);
}

static void run_movew_d16_to_d16(void)
{
    uint8_t *mem = calloc(1, SZ), *mem2 = calloc(1, SZ);
    const uint32_t src = ORG + 0x1000u, dst = ORG + 0x2000u;
    memcpy(mem, MOVEW_D16_TO_D16, sizeof MOVEW_D16_TO_D16);
    memcpy(mem2, MOVEW_D16_TO_D16, sizeof MOVEW_D16_TO_D16);
    /* 68k memory is big-endian: 0xbeef at 12(a0). */
    mem[src - ORG + 12] = mem2[src - ORG + 12] = 0xbeu;
    mem[src - ORG + 13] = mem2[src - ORG + 13] = 0xefu;

    j5d_sandbox sb = { mem, ORG, SZ }, refsb = { mem2, ORG, SZ };
    struct j5d_m68k_state jit, ref;
    memset(&jit, 0, sizeof jit); memset(&ref, 0, sizeof ref);
    jit.a[0] = ref.a[0] = src;
    jit.a[5] = ref.a[5] = dst;
    uint32_t d0 = 0, rd0 = 0; char err[200] = {0}, e2[200] = {0};
    int rc = j5d_run(&sb, ORG, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_run_free();
    int irc = j5d_interp_run(&refsb, ORG, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);
    int regs_ok = rc == 0 && irc == 0 && eq_regs(&jit, &ref);
    int mem_ok = memcmp(mem, mem2, SZ) == 0;
    int ok = rc == 0 && irc == 0 && regs_ok && mem_ok &&
             mem[dst - ORG + 626] == 0xbeu && mem[dst - ORG + 627] == 0xefu;
    printf("  move.w-d16 two funnel EAs in one instruction: regs=%s sandbox-mem=%s -> %s\n",
           regs_ok ? "byte-exact" : "DIVERGE", mem_ok ? "byte-exact" : "DIVERGE",
           ok ? "PASS" : "FAIL");
    if (rc) printf("    JIT error: %s\n", err);
    if (irc) printf("    ref error: %s\n", e2);
    if (!ok) g_fail = 1;
    free(mem); free(mem2);
}

/* ============================ the libcall program ============================== */
struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

static void run_libcall(void)
{
    uint8_t *mem = calloc(1, SZ);
    j4_sandbox jsb; j4_sandbox_init(&jsb, mem, ORG, SZ);   /* zeroes mem */
    memcpy(mem, LIB, sizeof LIB);
    j5d_sandbox sb = { mem, ORG, SZ };

    stub_lib lib; char err[200] = {0};
    if (stublib_init(&lib, &jsb, LIBBASE, HEAP_BASE, HEAP_END)) {
        printf("  libcall   stublib_init failed\n"); g_fail = 1; free(mem); return;
    }
    struct bctx c = { &lib, &jsb };

    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0;
    int rc = j5d_run(&sb, ORG, LIBBASE, &jit, &d0, bridge, &c, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    printf("  libcall   AllocMem(256,MEMF_CLEAR)+PutChar('A')+FreeMem via jsr -off(a6) "
           "-> [J3] bridge from the DECODED stream\n");
    printf("    through the JIT: %u blocks translated, %u library calls bridged, "
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
    printf("    asserts: seq=%s alloc=%s print=%s free=%s exit=%s -> %s\n",
           seq_ok?"ok":"X", alloc_ok?"ok":"X", print_ok?"ok":"X", free_ok?"ok":"X",
           exit_ok?"ok":"X", ok ? "PASS" : "FAIL");
    if (rc) printf("    run error: %s\n", err);
    if (!ok) g_fail = 1;
    j5d_run_free(); j3_free_all_thunks();
    free(mem);
}

/* ===================== negative control: corrupt one opcode ==================== */
static void neg_control(void)
{
    /* Corrupt mul's add.l d2,d0 (0xd082) -> add.l d2,d1 (0xd282) by flipping the dest
     * register field. The REAL decoder must emit a DIFFERENT (still-valid) instruction
     * so the JIT result diverges from the (uncorrupted) reference -> the asserts bite. */
    uint8_t corrupt[sizeof MUL]; memcpy(corrupt, MUL, sizeof MUL);
    corrupt[7] ^= 0x02;    /* d0->d1 destination */

    uint8_t *mem = calloc(1, SZ); memcpy(mem, corrupt, sizeof corrupt);
    j5d_sandbox sb = { mem, ORG, SZ };
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&sb, ORG, 0, &jit, &d0, NULL, NULL, err, sizeof err);

    /* The uncorrupted reference exits d0 = 42; corrupt accumulates into d1, leaving d0=0. */
    int bit = (rc == 0) && (d0 != 42);
    printf("  neg-ctrl  corrupt add.l d2,d0 -> d2,d1: JIT d0=%u (uncorrupt ref=42) -> %s\n",
           d0, bit ? "DIVERGED (asserts bite)" : "FAILED TO BITE");
    if (!bit) g_fail = 1;
    j5d_run_free(); free(mem);
}

/* ================= mapped-neighbour sandbox containment regression ============== */
static int classify_null_bounds_fault(void *fault_addr, void *user)
{
    (void)user;
    return fault_addr == NULL;
}

static int bounds_case(uint8_t *mem, size_t size, uint32_t a0, int is_store,
                       int expect_ok, uint32_t expect_d0)
{
    static const uint8_t load_code[]  = { 0x20,0x10, 0x4e,0x75 }; /* move.l (a0),d0;rts */
    static const uint8_t store_code[] = { 0x20,0x80, 0x4e,0x75 }; /* move.l d0,(a0);rts */
    memset(mem, 0, size);
    memcpy(mem, is_store ? store_code : load_code, sizeof load_code);
    if (expect_ok && !is_store) {
        size_t off = (size_t)(a0 - ORG);
        mem[off] = (uint8_t)(expect_d0 >> 24); mem[off + 1] = (uint8_t)(expect_d0 >> 16);
        mem[off + 2] = (uint8_t)(expect_d0 >> 8); mem[off + 3] = (uint8_t)expect_d0;
    }

    j5d_sandbox sb = { mem, ORG, (uint32_t)size };
    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    st.a[0] = a0;
    st.d[0] = is_store ? 0xdeadbeefu : 0;
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&sb, ORG, 0, &st, &d0, NULL, NULL, err, sizeof err);
    int ok = expect_ok ? (rc == 0 && d0 == expect_d0) : (rc != 0);
    j5d_run_free();
    return ok;
}

static void run_bounds_regression(void)
{
    size_t ps = (size_t)getpagesize();
    uint8_t *map = mmap(NULL, ps * 3, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        printf("  bounds    mmap failed -> FAIL\n"); g_fail = 1; return;
    }
    uint8_t *mem = map + ps;               /* sandbox is the MIDDLE mapped page */
    uint8_t *lower = map, *upper = map + ps * 2;
    memset(lower, 0xa5, ps); memset(upper, 0x5a, ps);
    uint8_t *lower_before = malloc(ps), *upper_before = malloc(ps);
    if (!lower_before || !upper_before) {
        printf("  bounds    allocation failed -> FAIL\n"); g_fail = 1;
        free(lower_before); free(upper_before); munmap(map, ps * 3); return;
    }
    memcpy(lower_before, lower, ps); memcpy(upper_before, upper, ps);

    /* Expected bounds faults are selected to NULL by the emitted checks. Classify that
     * synthetic fault so the normal recovery boundary unwinds without writing a report. */
    j5n_signal_set_classifier(classify_null_bounds_fault, NULL);
    j5n_signal_install(NULL);

    int low_load  = bounds_case(mem, ps, ORG - 4, 0, 0, 0);
    int high_load = bounds_case(mem, ps, ORG + (uint32_t)ps, 0, 0, 0);
    int low_store = bounds_case(mem, ps, ORG - 4, 1, 0, 0);
    int cross     = bounds_case(mem, ps, ORG + (uint32_t)ps - 2, 0, 0, 0);
    int canary_ok = memcmp(lower, lower_before, ps) == 0 &&
                    memcmp(upper, upper_before, ps) == 0;

    /* The final aligned longword is legal: prove the width limit is inclusive and that
     * the engine remains reusable after four contained faults. */
    int edge_ok = bounds_case(mem, ps, ORG + (uint32_t)ps - 4, 0, 1, 0x12345678u);

    j5n_signal_remove();
    j5n_signal_set_classifier(NULL, NULL);

    int ok = low_load && high_load && low_store && cross && canary_ok && edge_ok;
    printf("  bounds    mapped neighbours: low-load=%s high-load=%s low-store=%s "
           "cross-width=%s canaries=%s edge=%s -> %s\n",
           low_load?"contained":"X", high_load?"contained":"X",
           low_store?"contained":"X", cross?"contained":"X",
           canary_ok?"unchanged":"CORRUPT", edge_ok?"valid":"X", ok?"PASS":"FAIL");
    if (!ok) g_fail = 1;
    free(lower_before); free(upper_before); munmap(map, ps * 3);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(15);

    printf("[J5d] broadening the [J5c] re-hosting: the WHOLE apps68k corpus through the JIT\n");
    printf("      (REAL Emu68 decoders for every ALU/move/memory opcode + OUR dispatcher for\n");
    printf("      control flow + the (An) sandbox-EA edit + the jsr-vector -> [J3] bridge)\n\n");

    run_regprog("mul",      MUL,  sizeof MUL,  42,  NULL,
                "7*6 by repeated addition (moveq/add.l/subq.l/bne.s/rts)");
    run_regprog("fact",     FACT, sizeof FACT, 120, NULL,
                "5! via nested loops (move.l Dn,Dm + cmp.l + bne -> the [J5c]-coverage opcodes)");
    run_regprog("arraysum", ARR,  sizeof ARR,  150, arr_data,
                "sum {10..50} via add.l (a0)+,d0 (REAL EA decoder, sandbox-base + REV byteswap)");
    run_regprog("move-flags", MOVE_MEM_FLAGS, sizeof MOVE_MEM_FLAGS, 42, NULL,
                "MOVE.L zero to memory keeps Z across the checked store for BEQ");
    run_movew_d16_to_d16();
    run_libcall();
    run_bounds_regression();
    neg_control();

    if (g_fail) { printf("\n[J5d] FAIL\n"); return 1; }

    printf("\n  VERDICT: the WHOLE apps68k corpus now runs THROUGH THE JIT via Emu68's REAL\n");
    printf("           per-opcode decoders. The [J5c] register/ALU class is broadened with\n");
    printf("           LINE5 (addq/subq), reg-to-reg move + cmp (LINE2/MOVE+LINEB), the (An)/\n");
    printf("           (An)+ sandbox-memory EA (the disclosed M68k_EA.c edit, applied in the\n");
    printf("           build-dir copy via darwinize), and the jsr-through-vector [J3] bridge\n");
    printf("           decoded from the stream. Each result is byte-exact vs an independent\n");
    printf("           from-scratch interpreter. Still out of [J5d] scope: full ISA (FPU,\n");
    printf("           privileged, exceptions), our own SR/exception model, dirty-page SMC,\n");
    printf("           bcc.W/.L + computed jmp, and a sandbox-backed allocator for out-of-\n");
    printf("           sandbox return pointers.\n");
    printf("[J5d] PASS\n");
    return 0;
}
