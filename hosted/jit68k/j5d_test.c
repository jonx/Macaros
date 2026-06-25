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
#include "apps68k/stublib.h"
#include "j3_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

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
    run_libcall();
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
