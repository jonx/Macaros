/* t0p3_engine.c — [T0-P3] re-entrant engine, safe points, fault containment.
 * (OURS, AROS-licensed. Contains NO Emu68 source; links the engine via libjit68k.)
 *
 * THE EXIT BAR (docs/features/68k-transparent-exec/plan.md, [T0-P3], host harness):
 *   A. two engine instances INTERLEAVED on one thread (cooperative quanta via the
 *      poll callback + J5D_RC_YIELD/resume), both programs byte-exact;
 *   B. an infinite-loop 68k guest — fully CHAINED, i.e. spinning inside JIT code
 *      with no C dispatcher roundtrips — interrupted via j5d_request_stop() from a
 *      signal handler (J5D_RC_KILLED);
 *   C. a forced translated-code HOST fault (a store through a 68k address far
 *      outside the sandbox) unwound cleanly back out of j5d_run with the process
 *      alive, and a fresh instance runs normally afterwards;
 *   D. two engine runs SEQUENTIALLY IN ONE PROCESS on separate instances (the exact
 *      sequence that crashed with the global single-run engine — see the [T0-P1]
 *      NOTES.md finding).
 * Marker: [T0P3] PASS / FAIL. */

#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "j5n_diag.h"
#include "j5n_symbols.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x01000000u
#define LIBBASE         0x00230000u
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u
#define PROG_ORIGIN     0x00250000u

#define CHECK(cond, why) do { if (!(cond)) { \
    fprintf(stderr, "[T0P3] FAIL: %s (line %d)\n", why, __LINE__); exit(1); } } while (0)

/* ---- one runnable 68k program: its own arena, stub OS and 68k state ---- */
struct bctx {
    stub_lib *lib;
    j4_sandbox *sb;
    uint32_t preserve_base;
    unsigned preserve_calls;
};
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    if (c->preserve_base && st->a[6] == c->preserve_base &&
        (lvo == 1 || lvo == 2)) {
        c->preserve_calls++;
        return 0;
    }
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

typedef struct prog {
    j4_sandbox            sb;
    j4_seglist            seg;
    stub_lib              lib;        /* 1 MiB out-buffer: heap-allocate progs   */
    struct j5d_m68k_state st;
    struct bctx           c;
    j5d_engine           *eng;
    uint32_t              resume_pc;
    uint32_t              d0;
    int                   done;
    int                   yields;
} prog;

static prog *prog_new(const uint8_t *buf, size_t len)
{
    prog *p = calloc(1, sizeof *p);
    CHECK(p != NULL, "prog alloc");
    uint8_t *mem = calloc(1, SANDBOX_SIZE);
    CHECK(mem != NULL, "arena alloc");
    char err[256] = {0};
    j4_sandbox_init(&p->sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    p->sb.next_alloc = PROG_ORIGIN;
    CHECK(j4_load_hunks(&p->sb, buf, len, 0, &p->seg, err, sizeof err) == 0, err);
    CHECK(stublib_init(&p->lib, &p->sb, LIBBASE, HEAP_BASE, HEAP_END) == 0, "stublib init");
    p->c.lib = &p->lib; p->c.sb = &p->sb;
    p->eng = j5d_engine_new();
    CHECK(p->eng != NULL, "engine alloc");
    p->resume_pc = p->seg.entry;
    return p;
}

static void prog_free(prog *p)
{
    j5d_engine_free(p->eng);
    free(p->sb.host_mem);
    free(p);
}

/* Run one quantum (or to completion) of p on ITS engine. Returns the rc. */
static int prog_run(prog *p, char *err, unsigned errlen)
{
    j5d_engine_activate(p->eng);
    j5d_sandbox sb = { p->sb.host_mem, p->sb.sandbox_origin, p->sb.size };
    int rc = j5d_run(&sb, p->resume_pc, LIBBASE, &p->st, &p->d0,
                     bridge, &p->c, err, errlen);
    p->resume_pc = p->st.pc;
    if (rc == 0) p->done = 1;
    if (rc == J5D_RC_YIELD) p->yields++;
    return rc;
}

static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "test binary missing (build apps68k)");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    CHECK(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz, "read test binary");
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

/* ---- craft a minimal 1-code-hunk executable from longwords (big-endian) ---- */
static size_t mk_hunk(uint8_t *out, const uint32_t *code, unsigned ncode)
{
    uint32_t w[64]; unsigned n = 0;
    w[n++] = 0x3F3; w[n++] = 0;              /* HUNK_HEADER, no resident names   */
    w[n++] = 1; w[n++] = 0; w[n++] = 0;      /* 1 hunk, first 0, last 0          */
    w[n++] = ncode;                          /* hunk 0 size in longwords         */
    w[n++] = 0x3E9; w[n++] = ncode;          /* HUNK_CODE, ncode longwords       */
    for (unsigned i = 0; i < ncode; i++) w[n++] = code[i];
    w[n++] = 0x3F2;                          /* HUNK_END                          */
    for (unsigned i = 0; i < n; i++) {
        out[i*4+0] = (uint8_t)(w[i] >> 24); out[i*4+1] = (uint8_t)(w[i] >> 16);
        out[i*4+2] = (uint8_t)(w[i] >> 8);  out[i*4+3] = (uint8_t)w[i];
    }
    return n * 4;
}

/* ---- B: the SIGALRM -> stop-request path (async-signal-safe by design) ---- */
static volatile sig_atomic_t g_alarm_count = 0;
static void on_alarm(int sig)
{
    (void)sig;
    if (++g_alarm_count == 1) {
        j5d_request_stop();          /* one volatile store: signal-safe          */
        alarm(8);                    /* backstop: if the kill fails, hard-fail   */
    } else {
        const char msg[] = "[T0P3] FAIL: kill did not take; hard abort\n";
        write(2, msg, sizeof msg - 1);
        _exit(1);
    }
}

static j5d_poll_action poll_yield(void *u) { (void)u; return J5D_POLL_YIELD; }

int main(void)
{
    char err[256] = {0};

    /* ================= A. two instances interleaved on one thread ================= */
    size_t mlen, jlen;
    uint8_t *mbuf = slurp("hosted/jit68k/apps68k/bin/mandel.exe", &mlen);
    uint8_t *jbuf = slurp("hosted/jit68k/apps68k/bin/j5t.exe",   &jlen);
    prog *A = prog_new(mbuf, mlen);
    prog *B = prog_new(jbuf, jlen);

    j5d_engine_activate(A->eng); j5d_set_poll(poll_yield, NULL, 4);
    j5d_engine_activate(B->eng); j5d_set_poll(poll_yield, NULL, 4);

    while (!A->done || !B->done) {
        if (!A->done) {
            int rc = prog_run(A, err, sizeof err);
            CHECK(rc == 0 || rc == J5D_RC_YIELD, err);
        }
        if (!B->done) {
            int rc = prog_run(B, err, sizeof err);
            CHECK(rc == 0 || rc == J5D_RC_YIELD, err);
        }
    }
    CHECK(A->yields > 0 && B->yields > 0,
          "both programs actually yielded (the interleave was real)");
    CHECK(A->d0 == 0, "interleaved mandel exit D0 == 0");
    CHECK(A->lib.outlen > 0 && memchr(A->lib.out, '+', (size_t)A->lib.outlen) != NULL,
          "interleaved mandel rendered its output");
    CHECK(B->d0 == 10857, "interleaved j5t exit D0 == 10857 (the [J5t] byte-exact value)");
    CHECK(B->lib.outlen == 717, "interleaved j5t output is the 717-byte byte-exact stream");
    fprintf(stderr, "[T0P3] A ok: interleaved on one thread (mandel %d yields, j5t %d yields)\n",
            A->yields, B->yields);
    prog_free(A); prog_free(B);

    /* PhotoDemo found the nastier resume case: one base load followed by two
     * adjacent library calls.  Force a yield between those calls and prove A6
     * remains the guest's live alternate base rather than being reseeded to
     * the run's initial LIBBASE on re-entry. */
    static const uint32_t preserve_code[] = {
        0x2C7C0024u, 0x00004EAEu, 0xFFFA4EAEu, 0xFFF44E75u
    };
    uint8_t pb[256]; size_t plen = mk_hunk(pb, preserve_code, 4);
    prog *P = prog_new(pb, plen);
    P->c.preserve_base = 0x00240000u;
    j5d_engine_activate(P->eng);
    j5d_register_libbase(P->c.preserve_base);
    j5d_set_poll(poll_yield, NULL, 2);
    while (!P->done) {
        int prc = prog_run(P, err, sizeof err);
        CHECK(prc == 0 || prc == J5D_RC_YIELD, err);
    }
    CHECK(P->yields > 0, "adjacent library calls actually yielded between quanta");
    CHECK(P->c.preserve_calls == 2,
          "both adjacent calls kept the alternate A6 library base across resume");
    CHECK(P->st.a[6] == P->c.preserve_base,
          "live A6 remains the alternate library base after resumed completion");
    fprintf(stderr, "[T0P3] A6 ok: yield/resume preserved %08x across two adjacent calls\n",
            P->st.a[6]);
    j5d_clear_libbases();
    prog_free(P);

    /* ================= D. two sequential runs, one process, two instances ========= */
    /* The exact sequence that crashed the global single-run engine ([T0-P1] NOTES.md
     * finding: stale chained blocks in freed JIT memory). No forks here, on purpose. */
    prog *D1 = prog_new(mbuf, mlen);
    CHECK(prog_run(D1, err, sizeof err) == 0, err);
    CHECK(D1->d0 == 0 && D1->lib.outlen > 0, "sequential run 1 (mandel) byte-exact");
    prog_free(D1);                                   /* frees its MAP_JIT regions     */
    prog *D2 = prog_new(jbuf, jlen);
    CHECK(prog_run(D2, err, sizeof err) == 0, err);
    CHECK(D2->d0 == 10857 && D2->lib.outlen == 717, "sequential run 2 (j5t FP) byte-exact");
    prog_free(D2);
    fprintf(stderr, "[T0P3] D ok: two sequential runs in one process on fresh instances\n");

    /* ================= B. kill a fully-chained infinite loop ====================== */
    /* `here: bra.s here` self-chains after the first backpatch: the loop then spins
     * INSIDE translated code with zero dispatcher roundtrips — the exact case where a
     * translated process would freeze hosted AROS without the emitted safe point. */
    static const uint32_t loop_code[] = { 0x60FE4E75u };   /* bra.s -2 ; rts (dead)  */
    uint8_t hb[256]; size_t hlen = mk_hunk(hb, loop_code, 1);
    prog *L = prog_new(hb, hlen);

    signal(SIGALRM, on_alarm);
    alarm(1);
    int rc = prog_run(L, err, sizeof err);
    alarm(0);
    CHECK(rc == J5D_RC_KILLED, "infinite chained loop killed via j5d_request_stop");
    CHECK(strstr(err, "killed at safe point") != NULL, "kill rc carries the safe-point message");
    if (L->st.pc != L->seg.entry) {
        fprintf(stderr, "[T0P3] B: parked pc=%08x entry=%08x\n", L->st.pc, L->seg.entry);
        CHECK(0, "killed loop parked at its own entry PC");
    }
    {
        j5d_stats s; j5d_get_stats(&s);
        CHECK(s.chain_branches_taken > 1000,
              "the loop really was chained (it spun in JIT, not in the C dispatcher)");
    }
    prog_free(L);
    fprintf(stderr, "[T0P3] B ok: chained infinite loop killed from a signal handler\n");

    /* ================= C. forced translated-code host fault, contained ============ */
    /* movea.l #$F0000000,a0 ; move.l d0,(a0) ; rts — the (An) store resolves to a host
     * address ~3.7 GiB past the arena: a genuine SIGSEGV inside MAP_JIT code. With the
     * [J5n] signal net installed the engine must return an error (bundle written), the
     * process must live, and a fresh instance must run normally afterwards. */
    static const uint32_t fault_code[] = { 0x207CF000u, 0x00002080u, 0x4E754E75u };
    uint8_t fb[256]; size_t flen = mk_hunk(fb, fault_code, 3);
    prog *F = prog_new(fb, flen);

    static j5n_symtab symtab;
    j5n_symbols_parse(fb, flen, &F->seg, &symtab);
    j5d_sandbox j5sb = { F->sb.host_mem, F->sb.sandbox_origin, F->sb.size };
    static j5n_diag diag;
    j5n_diag_init(&diag, fb, flen, &j5sb, F->seg.entry, LIBBASE, &symtab);
    diag.quiet_banner = 1;
    const char *cd = getenv("TMPDIR"); if (cd) diag.crash_dir = cd;
    j5d_engine_activate(F->eng);
    j5d_set_diag(&diag);
    j5n_signal_install(&diag);

    rc = prog_run(F, err, sizeof err);
    j5n_signal_remove();
    j5d_set_diag(NULL);
    CHECK(rc != 0 && rc != J5D_RC_YIELD && rc != J5D_RC_KILLED,
          "host fault surfaced as a clean engine error, not a process death");
    CHECK(diag.bundles_written > 0, "the fault produced a crash bundle");
    prog_free(F);

    prog *R = prog_new(mbuf, mlen);        /* the process is alive and usable */
    CHECK(prog_run(R, err, sizeof err) == 0, err);
    CHECK(R->d0 == 0 && R->lib.outlen > 0, "post-fault fresh instance runs byte-exact");
    prog_free(R);
    fprintf(stderr, "[T0P3] C ok: translated-code host fault contained; engine reusable\n");

    free(mbuf); free(jbuf);

    printf("[T0P3] PASS: engine instances + safe points + fault containment proven — two "
           "instances interleaved on one thread via poll/yield/resume (both byte-exact), "
           "two sequential same-process runs on fresh instances (the old single-run "
           "crasher), a fully-chained infinite 68k loop killed from a signal handler via "
           "the emitted chain-entry safe point (J5D_RC_KILLED, parked at its entry PC), "
           "and a genuine SIGSEGV in translated code unwound to a clean error with a "
           "crash bundle and a live, reusable process.\n");
    return 0;
}
