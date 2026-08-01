/* emu68k_host.c — the host-side 68k execution service (libemu68k.dylib).
 * (OURS, AROS-licensed. Links the engine via libjit68k; contains NO Emu68 source.)
 *
 * Composes the proven pieces exactly as run68k does — the [J4] loader, the stub
 * OS, the [J5d] engine with [T0-P3] instances/safe-points and [J5n] diagnostics
 * with fault containment — behind the small quantum-run API of emu68k_host.h,
 * so hosted AROS (emu68k.library) can drive real 68k programs from inside the
 * OS: bounded quanta, streaming output, async kill, contained faults. */

#include "emu68k_host.h"
#include "scan68k.h"

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
#include <sys/mman.h>
#include <unistd.h>

/* The guest memory map (the run68k layout; see run68k.c for the full map).
 *
 * [T2b] THE RUNTIME HARDWARE GUARD. The arena deliberately STOPS where the Amiga
 * hardware begins: it spans $210000..$BFD000, so the CIA range ($BFDxxx-$BFExxx)
 * and the custom chips ($DFFxxx) sit just ABOVE it and the exception-vector page
 * sits below it. A guest access to any of them therefore resolves to a host
 * address outside the mapping and faults, and the classifier below turns that
 * fault back into the guest address it came from - which is how "this program
 * wants the hardware" becomes a routing EVENT rather than a crash.
 *
 * This costs nothing on the hot path: no bounds compare is emitted, the hardware
 * simply is not there. It is also why the arena is ~9.9 MiB rather than the 16
 * MiB run68k uses standalone - a 16 MiB arena would swallow $DFF000 and let a
 * hardware write silently corrupt guest memory instead of announcing itself.
 *
 * "Not there" has to be literally true, so the whole low 16 MiB of GUEST space is
 * RESERVED as one PROT_NONE mapping and only the arena window inside it is made
 * readable/writable. A malloc'd arena would not do: reading a few KiB past the
 * end of a 10 MiB malloc block usually lands in the allocator's own pages and
 * quietly succeeds, which is exactly the silent-corruption case this prevents.
 * With the reservation, every guest address below the arena (the vector page)
 * and above it (CIAs, custom chips) resolves into PROT_NONE and faults. */
#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x009ED000u     /* ends at $BFD000, where the CIAs begin;
                                         * TRIMMED DOWN to a page below, because
                                         * mprotect rounds a length UP and a
                                         * 16 KiB page of overshoot would put the
                                         * CIA registers back inside the arena  */

/* the guest windows the arena deliberately excludes */
#define HW_CIA_LO       0x00BFD000u
#define HW_CIA_HI       0x00BFEFFFu
#define HW_CUSTOM_LO    0x00DFF000u
#define HW_CUSTOM_HI    0x00DFFFFFu
#define HW_VECTOR_HI    0x000003FFu
#define LIBBASE         0x00230000u
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u
#define PROG_ORIGIN     0x00250000u
#define ARGS_BASE       0x00238000u
#define ARGS_REGION_END 0x00240000u
#define GUEST_RESERVE   0x01000000u     /* guest $000000..$1000000 reserved     */

/* [T3] The AmigaOS environment a real program expects.
 *
 * A 68k program does not receive a library base: it reads SysBase from ABSOLUTE
 * ADDRESS 4 (`move.l 4.w,a6`), then calls exec's OpenLibrary through it. So the
 * guest needs a readable low page with a pointer at offset 4, an exec base, and
 * a base per opened library - each recognised by the engine so `jsr -N(A6)`
 * reaches the bridge.
 *
 * The low page is mapped READ-ONLY on purpose: reading SysBase at 4 is the
 * standard idiom and must work, while WRITING an exception vector is a program
 * taking over the machine and must still fault into the [T2b] hardware guard.
 * Read-only gives both behaviours with one mapping. */
#define GUEST_LOWPAGE   0x00000000u
#define EXEC_BASE       0x00200000u     /* guest exec.library base              */
#define LIBBASE_FIRST   0x00201000u     /* opened libraries get bases from here */
#define LIBBASE_STRIDE  0x00001000u
#define LIBBASE_MAX     8

/* exec LVOs a program uses to get going (negative offset / 6). */
#define LVO_OPENLIBRARY   92    /* -552 */
#define LVO_CLOSELIBRARY  69    /* -414 */
#define LVO_ALLOCMEM      33    /* -198 */
#define LVO_FREEMEM       35    /* -210 */

static const char *g_crash_dir = NULL;

struct emu68k_run {
    void                 *reserve;       /* the PROT_NONE guest-space reservation */
    uint8_t              *arena;         /* the RW window inside it               */
    unsigned long         arena_size;    /* page-aligned DOWN (see the mapping)   */
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
    char                  name[64];      /* for ledger/bundle attribution       */
    /* [T3] opened libraries: guest base -> name, for the OS-call callback */
    struct { uint32_t base; char name[32]; } openlib[LIBBASE_MAX];
    int                   nlib;
    stub_lib             *run_lib;       /* the guest heap, for exec AllocMem   */
};

/* [T3] The OS-call seam. The engine runs on the HOST; the real AROS libraries
 * live in AROS. So a library call the stub does not implement is handed OUT to
 * the embedder (emu68k.library), which marshals the guest registers into a real
 * native call and writes the result back. The host stays OS-agnostic: it knows
 * about guest memory and registers, nothing about AROS.
 *
 *   libname : which library the program called (its own OpenLibrary name)
 *   lvo     : the vector index (negative offset / 6)
 *   regs    : the live 68k register file, marshalled in and out by the callback
 *   base    : host pointer to guest address 0, for translating guest pointers
 * Returns 0 if the call was served, nonzero if not (-> capability gap). */
static emu68k_oscall_fn g_oscall = NULL;
static void            *g_oscall_user = NULL;
void emu68k_set_oscall(emu68k_oscall_fn fn, void *user)
{ g_oscall = fn; g_oscall_user = user; }

/* [T2b] the hardware-access classifier. The signal handler hands us the faulting
 * HOST address; subtracting the sandbox base-adjust recovers the guest address
 * the program actually asked for. If that lands in a hardware window, this was
 * not a crash: the program wants the Amiga hardware. */
static char g_hw_detail[96];

static int classify_hardware(void *fault_addr, void *user)
{
    struct emu68k_run *r = user;
    unsigned long long host = (unsigned long long)(uintptr_t)fault_addr;
    unsigned long long base = (unsigned long long)(uintptr_t)r->sb.host_mem;
    unsigned long long guest;

    if (host < base - 0x10000000ull || host > base + 0x10000000ull) return 0;
    guest = host - base + r->sb.sandbox_origin;

    if (guest >= HW_CUSTOM_LO && guest <= HW_CUSTOM_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail,
                 "custom chip register $%06llX", guest);
    else if (guest >= HW_CIA_LO && guest <= HW_CIA_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail, "CIA register $%06llX", guest);
    else if (guest <= HW_VECTOR_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail,
                 "exception vector page $%03llX", guest);
    else
        return 0;                                  /* a genuine wild access     */
    return 1;
}

void emu68k_run_set_name(struct emu68k_run *r, const char *name)
{
    if (r) snprintf(r->name, sizeof r->name, "%s", name ? name : "");
}

/* ---- [T1d] the capability-gap ledger: every library call the bridge cannot
 * marshal is RECORDED (lvo + count + last program) and the run aborts with a
 * classified message — the design's no-guessing rule, and the data that drives
 * which function gets marshalled next. Also mirrored to stderr, which hosted
 * AROS forwards to the host log, so the gap is visible without tooling. ---- */
#define EMU68K_LEDGER_MAX 64
static struct { int lvo; unsigned long count; char prog[64]; } g_ledger[EMU68K_LEDGER_MAX];
static int g_ledger_n = 0;

static void ledger_record(int lvo, const char *prog)
{
    int i;
    for (i = 0; i < g_ledger_n; i++)
        if (g_ledger[i].lvo == lvo) break;
    if (i == g_ledger_n && g_ledger_n < EMU68K_LEDGER_MAX) {
        g_ledger_n++;
        g_ledger[i].lvo = lvo;
        g_ledger[i].count = 0;
    }
    if (i < EMU68K_LEDGER_MAX) {
        g_ledger[i].count++;
        snprintf(g_ledger[i].prog, sizeof g_ledger[i].prog, "%s", prog ? prog : "");
    }
    fprintf(stderr, "[emu68k] capability gap: LVO %d (offset %d) prog=\"%s\" hits=%lu\n",
            lvo, -6 * lvo, prog ? prog : "", i < EMU68K_LEDGER_MAX ? g_ledger[i].count : 0);
}

int emu68k_ledger_get(int idx, int *lvo, unsigned long *count)
{
    if (idx < 0 || idx >= g_ledger_n) return 0;
    if (lvo) *lvo = g_ledger[idx].lvo;
    if (count) *count = g_ledger[idx].count;
    return 1;
}

struct bctx { stub_lib *lib; j4_sandbox *sb; struct emu68k_run *run; };

/* read a NUL-terminated guest string (bounded by the arena) */
static const char *guest_cstr(j4_sandbox *sb, uint32_t addr)
{
    if (addr < sb->sandbox_origin ||
        addr >= sb->sandbox_origin + sb->size) return NULL;
    const char *p = (const char *)j4_sandbox_host(sb, addr);
    unsigned long max = sb->sandbox_origin + sb->size - addr;
    if (!memchr(p, 0, max)) return NULL;
    return p;
}

/* [T3] exec.library, served here: this is the bootstrap every AmigaOS program
 * performs before it can do anything else. OpenLibrary hands back a guest base
 * that the engine then recognises, so calls through it arrive at the bridge
 * with A6 naming the library. */
static int exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case LVO_OPENLIBRARY: {
        const char *nm = guest_cstr(sb, st->a[1]);      /* A1 = name, D0 = ver  */
        if (!nm) { snprintf(e, el, "OpenLibrary: bad name pointer"); return 1; }
        for (int i = 0; i < r->nlib; i++)                 /* already open?       */
            if (!strcmp(r->openlib[i].name, nm)) { st->d[0] = r->openlib[i].base; return 0; }
        if (r->nlib >= LIBBASE_MAX) { snprintf(e, el, "too many open libraries"); return 1; }
        /* Only offer what the embedder can actually serve; anything else fails
         * the AmigaOS way (D0 = 0), which programs are written to handle. */
        if (!g_oscall) { st->d[0] = 0; return 0; }
        uint32_t base = LIBBASE_FIRST + (uint32_t)r->nlib * LIBBASE_STRIDE;
        snprintf(r->openlib[r->nlib].name, sizeof r->openlib[r->nlib].name, "%s", nm);
        r->openlib[r->nlib].base = base;
        r->nlib++;
        j5d_register_libbase(base);
        st->d[0] = base;
        return 0;
    }
    case LVO_CLOSELIBRARY:
        st->d[0] = 0;
        return 0;                                        /* bases stay valid    */
    case LVO_ALLOCMEM: {
        /* Memory a 68k program allocates must live in the GUEST arena: the
         * program will dereference the pointer itself. So this is served here
         * from the guest heap and never handed out to the embedder, whose
         * allocator would return an address the program cannot reach. */
        uint32_t size = (st->d[0] + 3u) & ~3u;
        stub_lib *L = r->run_lib;
        if (!L || size == 0 || L->heap_next + size > L->heap_end) {
            st->d[0] = 0;                                /* AmigaOS: NULL       */
            return 0;
        }
        st->d[0] = L->heap_next;
        L->heap_next += size;
        memset(j4_sandbox_host(sb, st->d[0]), 0, size);
        return 0;
    }
    case LVO_FREEMEM:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;
    default:
        return 1;                                        /* not served here     */
    }
}

static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    struct emu68k_run *r = c->run;
    uint32_t a6 = st->a[6];

    /* which library did the program call through? */
    if (r && a6 == EXEC_BASE) {
        if (exec_call(r, c->sb, lvo, st, e, el) == 0) return 0;
        /* fall through to the OS callback: the embedder may serve more of exec */
        if (g_oscall &&
            g_oscall("exec.library", lvo, st, r->reserve, g_oscall_user,
                     e, el) == 0)
            return 0;
        ledger_record(lvo, r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: exec.library function LVO %d (offset %d) "
                        "is not available yet", lvo, -6 * lvo);
        return 1;
    }
    if (r) {
        for (int i = 0; i < r->nlib; i++) {
            if (r->openlib[i].base != a6) continue;
            if (g_oscall &&
                g_oscall(r->openlib[i].name, lvo, st, r->reserve, g_oscall_user,
                         e, el) == 0)
                return 0;
            ledger_record(lvo, r->name[0] ? r->name : NULL);
            snprintf(e, el, "capability gap: %s function LVO %d (offset %d) "
                            "is not available yet", r->openlib[i].name, lvo, -6 * lvo);
            return 1;
        }
    }

    /* the built-in stub OS (the corpus path: PutChar/AllocMem/FreeMem) */
    int rc = stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
    if (rc) {
        ledger_record(lvo, r && r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: library function LVO %d (offset %d) "
                        "is not marshalled yet", lvo, -6 * lvo);
    }
    return rc;
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

    /* reserve the guest address range, then open ONLY the arena window in it */
    r->reserve = mmap(NULL, GUEST_RESERVE, PROT_NONE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r->reserve == MAP_FAILED) {
        r->reserve = NULL;
        snprintf(err, errlen, "cannot reserve the guest address range");
        goto fail;
    }
    r->arena = (uint8_t *)r->reserve + SANDBOX_ORIGIN;
    {   /* round the writable window DOWN to a whole page: mprotect extends a
         * partial page, which would hand the guest the very hardware addresses
         * the arena is sized to exclude. */
        long pg = sysconf(_SC_PAGESIZE);
        unsigned long mask = (pg > 0) ? (unsigned long)pg - 1u : 0x3FFFu;
        r->arena_size = SANDBOX_SIZE & ~mask;
        if (mprotect(r->arena, r->arena_size, PROT_READ | PROT_WRITE) != 0) {
            snprintf(err, errlen, "cannot map the guest arena");
            goto fail;
        }
    }
    /* [T3] the low page: readable so `move.l 4.w,a6` finds SysBase, NOT writable
     * so installing an exception vector still faults into the hardware guard. */
    {
        long pg = sysconf(_SC_PAGESIZE);
        unsigned long pgsz = (pg > 0) ? (unsigned long)pg : 0x4000u;
        uint8_t *low = (uint8_t *)r->reserve + GUEST_LOWPAGE;
        if (mprotect(low, pgsz, PROT_READ | PROT_WRITE) == 0) {
            low[4] = (uint8_t)(EXEC_BASE >> 24); low[5] = (uint8_t)(EXEC_BASE >> 16);
            low[6] = (uint8_t)(EXEC_BASE >> 8);  low[7] = (uint8_t)EXEC_BASE;
            mprotect(low, pgsz, PROT_READ);          /* freeze it read-only     */
        }
    }
    r->image = malloc(imagelen ? imagelen : 1);
    if (!r->image) {
        snprintf(err, errlen, "out of memory (image)");
        goto fail;
    }
    memcpy(r->image, image, imagelen);
    r->imagelen = imagelen;

    j4_sandbox_init(&r->sb, r->arena, SANDBOX_ORIGIN, r->arena_size);
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
    {   /* the exec base must be recognised on THIS run's engine instance */
        j5d_engine *prev = j5d_engine_active();
        j5d_engine_activate(r->eng);
        j5d_clear_libbases();
        j5d_register_libbase(EXEC_BASE);
        j5d_engine_activate(prev);
    }

    j5n_symbols_parse(r->image, imagelen, &r->seg, &r->symtab);
    {
        j5d_sandbox j5sb = { r->sb.host_mem, r->sb.sandbox_origin, r->sb.size };
        j5n_diag_init(&r->diag, r->image, imagelen, &j5sb, r->seg.entry, LIBBASE,
                      &r->symtab);
    }
    r->diag.quiet_banner = 1;
    if (g_crash_dir) r->diag.crash_dir = g_crash_dir;

    r->run_lib   = &r->lib;
    r->sink      = sink;
    r->sink_user = sink_user;
    r->resume_pc = r->seg.entry;
    return r;

fail:
    if (r->reserve) munmap(r->reserve, GUEST_RESERVE);
    free(r->image); free(r);
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
    j5n_signal_set_classifier(classify_hardware, r);

    struct bctx c = { &r->lib, &r->sb, r };
    j5d_sandbox j5sb = { r->sb.host_mem, r->sb.sandbox_origin, r->sb.size };
    uint32_t d0 = 0;
    char lerr[256] = {0};

    int rc = j5d_run(&j5sb, r->resume_pc, LIBBASE, &r->st, &d0,
                     bridge, &c, lerr, sizeof lerr);
    r->resume_pc = r->st.pc;

    j5n_signal_remove();
    j5n_signal_set_classifier(NULL, NULL);
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
    if (rc == J5D_RC_HARDWARE) {
        r->done = 1;
        snprintf(err, errlen, "needs the Amiga hardware (%s)",
                 g_hw_detail[0] ? g_hw_detail : "unmapped hardware window");
        return EMU68K_RC_HARDWARE;
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
    if (r->reserve) munmap(r->reserve, GUEST_RESERVE);
    free(r->image);
    free(r);
}

void emu68k_set_crash_dir(const char *dir) { g_crash_dir = dir; }

int emu68k_scan_image(const void *image, unsigned long imagelen,
                      char *detail, unsigned detaillen)
{
    scan68k_report rep;
    char err[128];
    if (scan68k_image(image, imagelen, &rep, err, sizeof err)) {
        snprintf(detail, detaillen, "%s", err);
        return 0;                       /* unscannable: let it try to run       */
    }
    if (rep.confidence == SCAN68K_BANGER) {
        snprintf(detail, detaillen, "%s",
                 rep.n_evidence ? rep.evidence[0].what : "hits the Amiga hardware");
        return 1;                       /* FULL                                  */
    }
    snprintf(detail, detaillen, "%s", scan68k_confidence_text(rep.confidence));
    return 0;                           /* JIT                                   */
}

const char *emu68k_version(void)
{
    return "emu68k host service 1.0 ([T1]: quantum runs, streaming sink, async kill, "
           "contained faults)";
}
