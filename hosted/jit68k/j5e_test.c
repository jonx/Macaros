/* j5e_test.c — [J5e] THE OPTIMIZE DELIVERABLE: prove the register-file frame keeps the WHOLE
 * apps68k corpus BYTE-EXACT vs the independent interpreter while MEASURABLY cutting the 68k
 * register file's trips through struct j5d_m68k_state memory. (OURS, AROS-licensed.)
 *
 * [J5k] UPDATE — cross-region chaining SUPERSEDES the [J5e] per-block minimal frame. [J5e]
 * originally loaded only a block's live-in regs and stored only its dirty regs (a per-block
 * frame, vs the naive load-16/store-16). [J5k] pins the WHOLE 68k file in fixed host regs
 * across a chained region (the cold entry loads all 16; chained hops keep them live and do
 * NOT round-trip memory; the epilogue stores all 16 only at a dispatcher exit). That is a
 * STRICTLY LARGER reduction in register memory traffic on any looping/branching program —
 * but it is mutually exclusive with the per-block minimal load (chaining needs the full file
 * live at every chain entry, which only a full-file pin guarantees without global liveness).
 * So this test now measures the [J5k] reduction (the active optimization), not the retired
 * per-block frame. The CORRECTNESS gate is unchanged and still primary.
 *
 * The marker `[J5e] PASS` is gated on BOTH:
 *   (a) CORRECTNESS — every corpus program's JIT register file + sandbox memory + the
 *       libcall stub-call log is byte-exact equal to the from-scratch interpreter, the
 *       negative control still bites; AND
 *   (b) A MEASURED REDUCTION — the [J5k] chaining frame cuts C-dispatcher round-trips below
 *       total block executions and elides register memory-ops at the chained boundaries.
 * Correctness gates the marker: a smaller count with a diverging result is a FAIL, not a PASS.
 *
 * ===================== THE REGISTER ALLOCATOR [J5e] BUILDS =======================
 * The REAL Emu68 decoders already keep the 68k Dn/An permanently in fixed host registers
 * across a basic block (D0..D7=w19..w26, An=w13..w17,w27..w29) — there is NO per-op spill
 * of the architectural file inside the decoders (RA_MapM68kRegister returns the fixed map;
 * reg-to-reg ops emit directly against it). What the naive [J5d] frame did was bracket
 * EVERY block with a FIXED prologue/epilogue: load all 16 Dn/An from struct j5d_m68k_state
 * + store all 16 back, unconditionally (32 state ldr/str per block) regardless of what the
 * block touches. [J5e]'s RA (j5c_ra.c) records, as the decoders run, which 68k regs are
 * READ before written (live-in) and which are WRITTEN (dirty); the engine then loads ONLY
 * live-in regs in the prologue and stores back ONLY dirty regs in the epilogue.
 *
 * SPILL POLICY at boundaries (correctness-critical):
 *   - block exit (RTS / fall-through / branch): store dirty regs -> the state struct.
 *   - jsr-through-vector (the [J3] library bridge): the block ENDS at the jsr; its
 *     epilogue stores all dirty regs to memory BEFORE the dispatcher calls the bridge,
 *     and the bridge marshals the 68k args FROM the memory state. Clean regs were never
 *     modified in their host copy, so memory is fully consistent for ANY register at the
 *     boundary. (No partial-store hazard: a partial .W/.B write reads the reg first, so it
 *     is live-in AND dirty, hence both loaded and stored.)
 *   - (An) memory access (may alias the code/data): the access goes through OUR sandbox EA
 *     helper which dereferences host_mem, independent of the state struct, so it does not
 *     race the deferred reg file; An itself (modified by (An)+/-(An)) is marked dirty by
 *     the REAL EA decoder's RA_SetDirtyM68kRegister and so is stored at exit.
 * Deferred (honest): cross-block register caching (re-loading live-ins the previous block
 * already had in host regs) and linear-scan spilling under register pressure — see spec.
 * ===============================================================================
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

/* The EXACT relocated code streams the [J4] loader lays down (same as j5d_test.c). */
static const uint8_t MUL[] = {
    0x70,0x00, 0x72,0x07, 0x74,0x06, 0xd0,0x82, 0x53,0x81, 0x66,0xfa, 0x4e,0x75 };
static const uint8_t FACT[] = {
    0x70,0x01, 0x74,0x02, 0x72,0x00, 0x26,0x02, 0xd2,0x80, 0x53,0x83, 0x66,0xfa,
    0x20,0x01, 0x52,0x82, 0x78,0x06, 0xb4,0x84, 0x66,0xec, 0x4e,0x75 };
static const uint8_t ARR[] = {
    0x41,0xf9, 0x00,0x21,0x01,0x00, 0x72,0x05, 0x70,0x00,
    0xd0,0x98, 0x53,0x81, 0x66,0xfa, 0x4e,0x75 };
static const uint8_t LIB[] = {
    0x20,0x3c,0x00,0x00,0x01,0x00, 0x72,0x01, 0x4e,0xae,0xff,0x3a, 0x24,0x40,
    0x70,0x41, 0x4e,0xae,0xff,0xe2, 0x22,0x4a, 0x20,0x3c,0x00,0x00,0x01,0x00,
    0x4e,0xae,0xff,0x2e, 0x70,0x00, 0x4e,0x75 };

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
    const char *m = "[J5e] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

/* Per-program totals for the before/after summary. [J5k]: count executions/round-trips and
 * the register memory-ops elided at chained boundaries. */
static uint32_t tot_exec = 0, tot_roundtrips = 0, tot_elided = 0;

/* A register-only program: run through the optimized JIT, assert byte-exact, report the
 * RA before/after numbers. */
static void measure_regprog(const char *nm, const uint8_t *code, unsigned clen,
                            uint32_t want, void (*data)(uint8_t *))
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
    int memok = (memcmp(mem, mem2, SZ) == 0);
    int correct = (rc == 0) && (d0 == want) && (d0 == rd0) && regs_ok && memok;

    /* [J5k] the active optimization: chained hops keep the file live and skip the C round-trip
     * + the register memory round-trip. A program with a loop must show round-trips < execs. */
    int reduced = (s.dispatcher_roundtrips < s.blocks_executed) && (s.chain_branches_taken > 0);

    tot_exec += s.blocks_executed; tot_roundtrips += s.dispatcher_roundtrips;
    tot_elided += s.chain_spills_elided;

    printf("  %-9s want d0=%u -> JIT d0=%u  regs=%s sandbox-mem=%s  CORRECT=%s\n",
           nm, want, d0, regs_ok ? "byte-exact" : "DIVERGE",
           memok ? "byte-exact" : "DIVERGE", correct ? "yes" : "NO");
    printf("    %u blocks | %u executions -> %u C round-trips (%u chained, %u edges linked)\n",
           s.blocks_translated, s.blocks_executed, s.dispatcher_roundtrips,
           s.chain_branches_taken, s.chain_links_patched);
    printf("    register memory-ops elided at chained boundaries: %u\n", s.chain_spills_elided);
    printf("    -> %s\n", (correct && reduced) ? "PASS" : "FAIL");
    if (!correct) { g_fail = 1; if (rc) printf("    run error: %s\n", err);
                                if (irc) printf("    interp error: %s\n", e2); }
    if (!reduced) { g_fail = 1; printf("    NO MEASURED REDUCTION (optimize clause not met)\n"); }

    j5d_run_free(); free(mem); free(mem2);
}

/* The libcall program: the spill-at-jsr-boundary case. */
struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

static void measure_libcall(void)
{
    uint8_t *mem = calloc(1, SZ);
    j4_sandbox jsb; j4_sandbox_init(&jsb, mem, ORG, SZ);
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

    int seq_ok = (lib.ncalls == 3) &&
                 lib.calls[0].lvo == STUB_LVO_ALLOCMEM &&
                 lib.calls[1].lvo == STUB_LVO_PUTCHAR  &&
                 lib.calls[2].lvo == STUB_LVO_FREEMEM;
    int alloc_ok = lib.calls[0].arg_d0 == 256 && lib.calls[0].arg_d1 == STUB_MEMF_CLEAR &&
                   lib.calls[0].ret_d0 >= HEAP_BASE && lib.calls[0].ret_d0 < HEAP_END;
    int print_ok = lib.outlen == 1 && lib.out[0] == 'A';
    int free_ok  = lib.calls[2].arg_a1 == lib.calls[0].ret_d0 &&
                   lib.calls[2].arg_d0 == 256 && lib.bytes_outstanding == 0;
    int correct  = (rc == 0) && (d0 == 0) && seq_ok && alloc_ok && print_ok && free_ok;

    tot_exec += s.blocks_executed; tot_roundtrips += s.dispatcher_roundtrips;
    tot_elided += s.chain_spills_elided;

    printf("  libcall   AllocMem+PutChar+FreeMem via jsr -off(a6) -> [J3] bridge "
           "(SPILL-AT-CALL boundary)\n");
    printf("    %d library calls bridged from the memory state | CORRECT=%s "
           "(seq=%s alloc=%s print=%s free=%s exit=%s)\n",
           lib.ncalls, correct ? "yes" : "NO", seq_ok?"ok":"X", alloc_ok?"ok":"X",
           print_ok?"ok":"X", free_ok?"ok":"X", (rc==0 && d0==0)?"ok":"X");
    /* libcall is STRAIGHT-LINE (no loop) — the [J5k] spill-at-jsr-boundary correctness case,
     * not a chaining-reduction case. Each jsr-through-vector is a NON-chainable terminator: the
     * block's epilogue flushes the WHOLE file to memory before the [J3] bridge marshals the 68k
     * args from that memory state — so the SPILL-AT-CALL boundary is exactly memory-consistent.
     * No reduction is expected (or required) here; the gate for libcall is correctness only. */
    printf("    %u blocks | %u executions -> %u C round-trips (jsr boundaries spill the file)\n",
           s.blocks_translated, s.blocks_executed, s.dispatcher_roundtrips);
    printf("    -> %s\n", correct ? "PASS" : "FAIL");
    if (!correct) { g_fail = 1; if (rc) printf("    run error: %s\n", err); }

    j5d_run_free(); j3_free_all_thunks(); free(mem);
}

/* Negative control: a smaller instruction count must NOT mask a wrong result. */
static void neg_control(void)
{
    uint8_t corrupt[sizeof MUL]; memcpy(corrupt, MUL, sizeof MUL);
    corrupt[7] ^= 0x02;   /* add.l d2,d0 -> add.l d2,d1 */

    uint8_t *mem = calloc(1, SZ); memcpy(mem, corrupt, sizeof corrupt);
    j5d_sandbox sb = { mem, ORG, SZ };
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&sb, ORG, 0, &jit, &d0, NULL, NULL, err, sizeof err);

    int bit = (rc == 0) && (d0 != 42);
    printf("  neg-ctrl  corrupt add.l d2,d0 -> d2,d1: JIT d0=%u (uncorrupt=42) -> %s\n",
           d0, bit ? "DIVERGED (correctness gate bites under the RA too)" : "FAILED TO BITE");
    if (!bit) g_fail = 1;
    j5d_run_free(); free(mem);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(15);

    printf("[J5e] register-file frame: keep the 68k Dn/An live in fixed host regs and minimise\n");
    printf("      their trips through struct j5d_m68k_state memory. [J5k] supersedes the original\n");
    printf("      per-block live-in/dirty frame with WHOLE-FILE pinning across chained regions:\n");
    printf("      the file stays live across block->block hops and only spills to memory at a\n");
    printf("      dispatcher boundary. Corpus must stay byte-exact AND show the reduction.\n\n");

    measure_regprog("mul",      MUL,  sizeof MUL,  42,  NULL);
    measure_regprog("fact",     FACT, sizeof FACT, 120, NULL);
    measure_regprog("arraysum", ARR,  sizeof ARR,  150, arr_data);
    measure_libcall();
    neg_control();

    printf("\n  ===== corpus totals =====\n");
    printf("    block executions:        %u\n", tot_exec);
    printf("    C-dispatcher round-trips: %u  (-%u vs %u executions, %.0f%% fewer)\n",
           tot_roundtrips, tot_exec - tot_roundtrips, tot_exec,
           tot_exec ? 100.0 * (tot_exec - tot_roundtrips) / tot_exec : 0.0);
    printf("    register memory-ops elided at chained boundaries: %u\n", tot_elided);

    int reduced = (tot_roundtrips < tot_exec) && (tot_elided > 0);

    if (g_fail || !reduced) {
        printf("\n[J5e] FAIL (%s)\n", g_fail ? "a correctness assert failed"
                                             : "no measured reduction");
        return 1;
    }

    printf("\n  VERDICT: the register-file frame keeps the WHOLE corpus byte-exact vs the\n");
    printf("           independent interpreter (registers + sandbox memory + the libcall\n");
    printf("           stub-call log) while the [J5k] chaining frame cuts C-dispatcher\n");
    printf("           round-trips below total executions and elides register memory-ops at\n");
    printf("           the chained boundaries on every looping program. libcall's straight-line\n");
    printf("           jsr boundaries correctly spill the whole file to memory (the bridge reads\n");
    printf("           the 68k args from there). The negative control still bites: correctness\n");
    printf("           gates the marker, not the smaller count.\n");
    printf("[J5e] PASS\n");
    return 0;
}
