/* j5k_test.c — [J5k] CROSS-REGION BLOCK CHAINING: chain cached blocks with direct AArch64
 * branches PAST the C dispatcher, keeping the 68k register file live across the hop. The
 * deliverable proves BOTH:
 *   (a) CORRECTNESS (gates the marker) — the chain-heavy Mandelbrot runs byte-exact vs the
 *       independent from-scratch interpreter (PutChar output stream + final register file +
 *       full sandbox memory), and the negative control bites; AND
 *   (b) THE MEASURED WIN — the C-dispatcher round-trips collapse (was ~42k, all of them) and
 *       direct block->block branches replace them, eliding the register-file memory traffic
 *       at every chained boundary.
 * Correctness gates the marker: a faster-but-wrong result is a FAIL. (OURS, AROS-licensed.)
 *
 * ===================== WHAT [J5k] ADDS OVER [J5j] (the optimization) ============
 * Through [J5j] every block exited via RET to the C MainLoop, which re-read the m68k PC and
 * re-dispatched — Emu68's own model. The Mandelbrot's ~42k block executions were ~42k C
 * round-trips, each storing the block's dirty 68k regs to struct j5d_m68k_state and the next
 * block reloading its live-ins. [J5k]:
 *   - DIRECT CHAINING. A block whose terminator is a STATIC-TARGET transfer (fall-through /
 *     BRA / Bcc / jmp abs.l) emits a backpatchable tail branch. The first time the edge is
 *     taken and the target block exists, the dispatcher LAZILY LINKS the slot to a `b` into
 *     the target's CHAIN ENTRY; thereafter the JIT'd code branches block->block, past C.
 *   - CROSS-BLOCK REGISTER CACHING. The whole 68k file (D0..D7, A0..A7) is pinned in fixed
 *     host regs across a chained region: the cold entry loads all 16, chained hops keep them
 *     live (no store-then-reload through memory), and the file spills to memory ONLY at a
 *     dispatcher boundary (the universal epilogue: rts / jsr-LVO bridge / exception / computed
 *     jump / an unresolved link). The CCR is synced through memory at every block boundary
 *     (one byte), so the inline Bcc condition + the C fallback always agree.
 *   - SPILL POLICY (correctness-critical). A chained hop A->B skips A's epilogue AND B's
 *     prologue, so the file is memory-INCONSISTENT mid-chain — fine, because nothing mid-chain
 *     reads memory for the file. At the ONE epilogue a chain reaches, the WHOLE file is stored
 *     (not just that block's dirty set), because any block in the chain may have written any
 *     register and they are all live in host regs. The (An) sandbox EA dereferences host_mem
 *     directly (independent of the file memory image), and An updates are in the live host
 *     regs, so memory aliasing is unaffected; the rts/library/exception boundaries all reach
 *     the full-file-spill epilogue, so they see a consistent memory state.
 *   - INVALIDATION. No mid-run cache eviction exists (a full cache is a clean error, not an
 *     eviction), so a linked branch never dangles within a run; j5d_run_free() drops all
 *     regions + links between runs. SMC / dirty-code-page invalidation stays DEFERRED — we do
 *     not chain across writable-code regions that rewrite themselves (no corpus program does).
 * ===============================================================================
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
    if (!f) { fprintf(stderr, "[J5k] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static void on_alarm(int sig){ (void)sig;
    const char *m = "[J5k] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
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

/* Run mandel.exe through the JIT (chained) AND the oracle; assert byte-exact + measure the
 * round-trip reduction. Returns the JIT stats via *out_s. */
static int run_mandel(j5d_stats *out_s)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    j4_sandbox sb, sb2; uint32_t entry = 0, entry2 = 0;
    if (load_one("bin/mandel.exe", mem, &sb, &entry) ||
        load_one("bin/mandel.exe", mem2, &sb2, &entry2)) { g_fail = 1; goto out; }
    j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
    j5d_sandbox b = { sb2.host_mem, sb2.sandbox_origin, sb2.size };

    stub_lib jlib; struct bctx jc = { &jlib, &sb };
    stublib_init(&jlib, &sb, LIBBASE, HEAP_BASE, HEAP_END);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&a, entry, LIBBASE, &jit, &d0, bridge, &jc, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s); *out_s = s;

    stub_lib rlib; struct bctx rc_ = { &rlib, &sb2 };
    stublib_init(&rlib, &sb2, LIBBASE, HEAP_BASE, HEAP_END);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, entry2, LIBBASE, &ref, &rd0, bridge, &rc_, e2, sizeof e2);

    printf("    ---- Mandelbrot (rendered by the CHAINED JIT'd 68k code, %d chars) ----\n", jlib.outlen);
    fwrite(jlib.out, 1, (size_t)jlib.outlen, stdout);
    printf("    -----------------------------------------------------------------------\n");

    int out_ok = (jlib.outlen == rlib.outlen) &&
                 (memcmp(jlib.out, rlib.out, (size_t)jlib.outlen) == 0);
    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int mem_ok  = (memcmp(mem, mem2, SZ) == 0);
    int d0_ok   = (rc == 0) && (d0 == 0) && (d0 == rd0);
    int shape_ok = (jlib.outlen == 26 * (64 + 1));
    int correct = out_ok && regs_ok && mem_ok && d0_ok && shape_ok;

    /* the chaining win on this chain-heavy program. */
    int chain_ok = (s.dispatcher_roundtrips < s.blocks_executed / 4u) &&
                   (s.chain_branches_taken > 0) && (s.chain_links_patched > 0);

    printf("    JIT d0=%u ORACLE d0=%u | output %d/%d bytes | byte-exact: stream=%s regs=%s mem=%s\n",
           d0, rd0, jlib.outlen, rlib.outlen,
           out_ok ? "yes" : "NO", regs_ok ? "yes" : "NO", mem_ok ? "yes" : "NO");
    printf("    [J5k] %u block executions -> %u C-dispatcher round-trips (%.1f%% fewer)\n",
           s.blocks_executed, s.dispatcher_roundtrips,
           s.blocks_executed ? 100.0*(1.0-(double)s.dispatcher_roundtrips/(double)s.blocks_executed) : 0.0);
    printf("    [J5k] %u direct block->block chain branches, %u edges lazily linked, %u register\n"
           "          memory-ops elided at chained boundaries\n",
           s.chain_branches_taken, s.chain_links_patched, s.chain_spills_elided);
    printf("    -> %s%s\n", correct ? "CORRECT" : "WRONG", chain_ok ? " + MEASURED WIN" : " (no win)");
    if (!correct) { g_fail = 1; if (rc) printf("    JIT err: %s\n", err); if (irc) printf("    oracle err: %s\n", e2); }
    if (!chain_ok) g_fail = 1;
    if (rc) printf("    JIT run error: %s\n", err);

    j5d_run_free();

    /* NEGATIVE CONTROL: corrupt the escape compare in the JIT copy so the chained streams MUST
     * diverge — proving the byte-exact assert (under chaining) is not a tautology. */
    {
        uint8_t *m3 = calloc(1, SZ), *m4 = calloc(1, SZ);
        j4_sandbox s3, s4; uint32_t e3 = 0, e4 = 0;
        if (!load_one("bin/mandel.exe", m3, &s3, &e3) && !load_one("bin/mandel.exe", m4, &s4, &e4)) {
            uint8_t *code = s3.host_mem; int patched = 0;
            for (uint32_t i = 0; i + 2 <= 0x200; i += 2)
                if (code[i] == 0xB0 && code[i+1] == 0xBC) { code[i] = 0xB2; patched = 1; break; }
            stub_lib jl2, rl2; stublib_init(&jl2, &s3, LIBBASE, HEAP_BASE, HEAP_END);
            stublib_init(&rl2, &s4, LIBBASE, HEAP_BASE, HEAP_END);
            struct bctx jc2 = { &jl2, &s3 }, rc2 = { &rl2, &s4 };
            j5d_sandbox a2 = { s3.host_mem, s3.sandbox_origin, s3.size };
            j5d_sandbox b2 = { s4.host_mem, s4.sandbox_origin, s4.size };
            struct j5d_m68k_state j2, r2; memset(&j2,0,sizeof j2); memset(&r2,0,sizeof r2);
            uint32_t jd=0, rdv=0; char x1[200]={0}, x2[200]={0};
            j5d_run(&a2, e3, LIBBASE, &j2, &jd, bridge, &jc2, x1, sizeof x1);
            j5d_interp_run(&b2, e4, LIBBASE, &r2, &rdv, bridge, &rc2, x2, sizeof x2);
            int diverged = patched && ((jl2.outlen != rl2.outlen) ||
                            memcmp(jl2.out, rl2.out, (size_t)jl2.outlen) != 0 || jd != rdv);
            printf("    neg-ctrl: corrupt escape compare in JIT copy -> %s\n",
                   diverged ? "DIVERGED (byte-exact assert bites under chaining)" : "FAILED TO BITE");
            if (!diverged) g_fail = 1;
        }
        free(m3); free(m4);
    }
out:
    free(mem); free(mem2);
    j5d_run_free(); j3_free_all_thunks();
    return g_fail;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(20);

    printf("[J5k] CROSS-REGION BLOCK CHAINING: chain cached blocks with direct AArch64 branches\n");
    printf("      past the C dispatcher, keeping the 68k register file live across the hop. The\n");
    printf("      chain-heavy Mandelbrot's PutChar stream + final registers + full sandbox memory\n");
    printf("      are asserted byte-exact vs an independent interpreter (correctness GATES the\n");
    printf("      marker), and the C-dispatcher round-trips are measured before vs after.\n\n");

    j5d_stats s;
    run_mandel(&s);

    if (g_fail) { printf("\n[J5k] FAIL\n"); return 1; }
    printf("\n  VERDICT: cross-region chaining + cross-block register caching run the chain-heavy\n");
    printf("           Mandelbrot byte-exact vs the independent interpreter while collapsing the\n");
    printf("           C-dispatcher round-trips from %u (every execution) to %u (%.1f%% fewer) and\n",
           s.blocks_executed, s.dispatcher_roundtrips,
           s.blocks_executed ? 100.0*(1.0-(double)s.dispatcher_roundtrips/(double)s.blocks_executed) : 0.0);
    printf("           eliding %u register memory-ops at the chained boundaries; the negative\n", s.chain_spills_elided);
    printf("           control bites. The frozen seam (jit_region API, struct M68KState layout,\n");
    printf("           the [J3] LVO marshalling contract, the [J5i] exception/SR model) is\n");
    printf("           UNCHANGED — chaining is entirely below the dispatcher's control flow.\n");
    printf("[J5k] PASS\n");
    return 0;
}
