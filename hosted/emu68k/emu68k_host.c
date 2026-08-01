/* emu68k_host.c — the host-side 68k execution service (libemu68k.dylib).
 * (OURS, AROS-licensed. Links the engine via libjit68k; contains NO Emu68 source.)
 *
 * Composes the proven pieces exactly as run68k does — the [J4] loader, the stub
 * OS, the [J5d] engine with [T0-P3] instances/safe-points and [J5n] diagnostics
 * with fault containment — behind the small quantum-run API of emu68k_host.h,
 * so hosted AROS (emu68k.library) can drive real 68k programs from inside the
 * OS: bounded quanta, streaming output, async kill, contained faults. */

#include "emu68k_host.h"

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

/* the run68k sandbox runtime layout (see run68k.c for the map) */
#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x01000000u
#define LIBBASE         0x00230000u
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u
#define PROG_ORIGIN     0x00250000u
#define ARGS_BASE       0x00238000u
#define ARGS_REGION_END 0x00240000u

static const char *g_crash_dir = NULL;

struct emu68k_run {
    uint8_t              *arena;
    j4_sandbox            sb;
    j4_seglist            seg;
    stub_lib              lib;
    struct j5d_m68k_state st;
    j5d_engine           *eng;
    j5n_diag              diag;
    j5n_symtab            symtab;
    uint8_t              *image;         /* copy kept for the [J5n] bundle      */
    unsigned long         imagelen;
    emu68k_sink_fn        sink;
    void                 *sink_user;
    long                  flushed;       /* stub output bytes already sunk      */
    uint32_t              resume_pc;
    int                   started;
    int                   done;
    volatile int          kill_req;      /* async kill flag (one store)         */
};

struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

/* the per-quantum poll: yield when the roundtrip budget is spent, kill when a
 * kill request landed. Registered on the run's own engine instance. */
static j5d_poll_action quantum_poll(void *user)
{
    struct emu68k_run *r = user;
    if (r->kill_req) return J5D_POLL_KILL;
    return J5D_POLL_YIELD;                      /* interval expiry = quantum end */
}

static void flush_output(struct emu68k_run *r)
{
    if (!r->sink) return;
    long n = r->lib.outlen - r->flushed;
    if (n > 0) {
        r->sink(r->lib.out + r->flushed, n, r->sink_user);
        r->flushed = r->lib.outlen;
    }
}

emu68k_run *emu68k_run_new(const void *image, unsigned long imagelen,
                           const char *args, unsigned long argslen,
                           emu68k_sink_fn sink, void *sink_user,
                           char *err, unsigned errlen)
{
    struct emu68k_run *r = calloc(1, sizeof *r);
    if (!r) { snprintf(err, errlen, "out of memory (run)"); return NULL; }

    r->arena = calloc(1, SANDBOX_SIZE);
    r->image = malloc(imagelen ? imagelen : 1);
    if (!r->arena || !r->image) {
        snprintf(err, errlen, "out of memory (arena)");
        goto fail;
    }
    memcpy(r->image, image, imagelen);
    r->imagelen = imagelen;

    j4_sandbox_init(&r->sb, r->arena, SANDBOX_ORIGIN, SANDBOX_SIZE);
    r->sb.next_alloc = PROG_ORIGIN;
    if (j4_load_hunks(&r->sb, r->image, imagelen, 0, &r->seg, err, errlen))
        goto fail;

    if (stublib_init(&r->lib, &r->sb, LIBBASE, HEAP_BASE, HEAP_END)) {
        snprintf(err, errlen, "stub library init failed");
        goto fail;
    }

    /* the AmigaDOS argument string: "<args>\n" at ARGS_BASE, A0/D0 seeded */
    {
        unsigned long n = argslen;
        if ((uint64_t)ARGS_BASE + n + 2 > (uint64_t)ARGS_REGION_END) {
            snprintf(err, errlen, "argument string too long");
            goto fail;
        }
        uint8_t *dst = j4_sandbox_host(&r->sb, ARGS_BASE);
        if (n) memcpy(dst, args, n);
        dst[n]     = '\n';
        dst[n + 1] = '\0';
        r->st.a[0] = ARGS_BASE;
        r->st.d[0] = (uint32_t)(n + 1);
    }

    r->eng = j5d_engine_new();
    if (!r->eng) { snprintf(err, errlen, "engine instance alloc failed"); goto fail; }

    j5n_symbols_parse(r->image, imagelen, &r->seg, &r->symtab);
    {
        j5d_sandbox j5sb = { r->sb.host_mem, r->sb.sandbox_origin, r->sb.size };
        j5n_diag_init(&r->diag, r->image, imagelen, &j5sb, r->seg.entry, LIBBASE,
                      &r->symtab);
    }
    r->diag.quiet_banner = 1;
    if (g_crash_dir) r->diag.crash_dir = g_crash_dir;

    r->sink      = sink;
    r->sink_user = sink_user;
    r->resume_pc = r->seg.entry;
    return r;

fail:
    free(r->arena); free(r->image); free(r);
    return NULL;
}

int emu68k_run_quantum(emu68k_run *r, unsigned long max_roundtrips,
                       unsigned int *exit_d0, char *err, unsigned errlen)
{
    if (!r || r->done) { snprintf(err, errlen, "run finished"); return EMU68K_RC_ERROR; }

    j5d_engine_activate(r->eng);
    /* Clamp the quantum: each j5d_run call must stay far below the dispatcher's
     * runaway step cap (the cap resets per call, so bounded quanta make it moot
     * for long programs — the OS-side loop and kill are the real guards now). */
    {
        uint32_t q = max_roundtrips ? (uint32_t)max_roundtrips : 4096u;
        if (q > 32768u) q = 32768u;
        j5d_set_poll(quantum_poll, r, q);
    }
    /* The [J5n] signal net WITHOUT the engine-side diag registration: faults are
     * contained + bundled, while block CHAINING stays ON (the hot path — the
     * per-block flight recorder is the price; the bundle still carries state). */
    j5n_signal_install(&r->diag);

    struct bctx c = { &r->lib, &r->sb };
    j5d_sandbox j5sb = { r->sb.host_mem, r->sb.sandbox_origin, r->sb.size };
    uint32_t d0 = 0;
    char lerr[256] = {0};

    int rc = j5d_run(&j5sb, r->resume_pc, LIBBASE, &r->st, &d0,
                     bridge, &c, lerr, sizeof lerr);
    r->resume_pc = r->st.pc;

    j5n_signal_remove();
    j5d_engine_activate(NULL);

    flush_output(r);

    if (rc == 0) {
        r->done = 1;
        if (exit_d0) *exit_d0 = d0;
        return EMU68K_RC_DONE;
    }
    if (rc == J5D_RC_YIELD)
        return EMU68K_RC_YIELD;
    if (rc == J5D_RC_KILLED) {
        r->done = 1;
        snprintf(err, errlen, "%s", lerr[0] ? lerr : "killed");
        return EMU68K_RC_KILLED;
    }
    r->done = 1;
    if (r->diag.bundles_written > 0)
        snprintf(err, errlen, "68k program fault (crash bundle: %s)",
                 r->diag.last_bundle[0] ? r->diag.last_bundle : "written");
    else
        snprintf(err, errlen, "%s", lerr[0] ? lerr : "68k program failed");
    return EMU68K_RC_ERROR;
}

void emu68k_run_kill(emu68k_run *r)
{
    if (!r) return;
    r->kill_req = 1;
    /* also raise the engine-side flag so a fully-chained loop parks even if
     * the poll interval is far away; harmless if another run is active — a
     * spurious poll continues. */
    j5d_request_stop();
}

void emu68k_run_free(emu68k_run *r)
{
    if (!r) return;
    j5d_engine_free(r->eng);
    free(r->arena);
    free(r->image);
    free(r);
}

void emu68k_set_crash_dir(const char *dir) { g_crash_dir = dir; }

const char *emu68k_version(void)
{
    return "emu68k host service 1.0 ([T1]: quantum runs, streaming sink, async kill, "
           "contained faults)";
}
