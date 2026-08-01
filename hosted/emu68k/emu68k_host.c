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
#include <time.h>
#include <ctype.h>
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
/* The arena runs to 32 MiB with the hardware ranges PUNCHED OUT as PROT_NONE
 * holes, rather than stopping short of them. Stopping short was simpler but it
 * capped the guest at ~9.9 MiB, and real software wants more than that: PPMore
 * faulted on a perfectly ordinary access at guest $100C000, just past the old
 * ceiling. Holes give both - megabytes of guest memory AND a hardware access
 * that still faults into the classifier. */
#define GUEST_TOP       0x02000000u
#define SANDBOX_SIZE    (GUEST_TOP - SANDBOX_ORIGIN)

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
#define GUEST_RESERVE   GUEST_TOP       /* the whole low guest space reserved   */

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
/* The library bases live INSIDE the arena, and must: a program does not only
 * call through a base, it READS FIELDS from it (ExecBase->AttnFlags, ThisTask,
 * a library's version...). A base outside the mapped arena faults on the first
 * such read, which is how this first failed on real software. */
#define EXEC_BASE       0x00220000u     /* guest exec.library base (in-arena)   */
#define LIBBASE_FIRST   0x00221000u     /* opened libraries get bases from here */
#define LIBBASE_STRIDE  0x00001000u
#define LIBBASE_MAX     16   /* real programs open more than a handful */

/* A minimal guest `struct Process`. Startup code universally does
 * FindTask(NULL) and then reads pr_CLI to decide whether it was launched from
 * the Shell or from Workbench; with no Process to read, it reads zero, concludes
 * "Workbench", and goes looking for a startup message that will never come (both
 * LhA and PPMore stopped at exactly that call). Offsets are the classic 68k
 * layout: struct Task is 92 bytes, MsgPort 34, and pr_CLI lands at 172. */
#define GUEST_PROCESS   0x00222000u
#define GUEST_CLI       0x00222400u
#define PR_TASK_LN_TYPE 8            /* tc_Node.ln_Type: NT_PROCESS = 13       */
#define PR_CLI_OFFSET   172
#define NT_PROCESS      13

/* A 68k program is a CLASSIC AmigaOS program, and there a BPTR is the address
 * DIVIDED BY FOUR: BADDR(x) is x<<2. Native AROS on this target is built with
 * AROS_FAST_BPTR, where a BPTR is just a pointer, so it is easy to plant a raw
 * address in a guest structure and be wrong in a way nothing complains about -
 * the program shifts it left by two and reads somewhere else entirely. That is
 * exactly what happened: pr_CLI was planted as $222400, the program computed
 * BADDR and got $889000, and the faults landed at addresses derived from it.
 * Every BPTR-typed field handed to the guest goes through this. */
#define GUEST_MKBADDR(a)  ((a) >> 2)
/* Guest-visible file handles. A dos handle is a BPTR, and a program is free to
 * dereference it (to read fh_Type, say), so a handle cannot be an opaque tag -
 * BADDR of a tag lands nowhere. Each handle is backed by a real, zeroed guest
 * structure here, and what crosses is MKBADDR of its address. The embedder maps
 * that slot back to the native BPTR it stands for.
 * These three constants are mirrored in emu68k_intern.h on the AROS side and
 * MUST match: that is the whole contract between the two halves. */
#define GUEST_FH_BASE   0x00223000u
#define GUEST_FH_SLOT   64u
#define GUEST_FH_MAX    32u
#define GUEST_BADDR(b)    ((b) << 2)

/* exec LVOs a program uses to get going (negative offset / 6). */
#define LVO_OPENLIBRARY   92    /* -552 */
#define LVO_CLOSELIBRARY  69    /* -414 */
#define LVO_ALLOCMEM      33    /* -198 */
#define LVO_FREEMEM       35    /* -210 */
#define LVO_FORBID        22    /* -132 */
#define LVO_PERMIT        23    /* -138 */
#define LVO_DISABLE       27    /* -162 */
#define LVO_ENABLE        28    /* -168 */
#define LVO_FINDTASK      49    /* -294 */
#define LVO_SETSIGNAL     51    /* -306 */
#define LVO_OLDOPENLIB    68    /* -408: what pre-2.0 programs still call      */
#define LVO_AVAILMEM      36    /* -216 */
#define LVO_ALLOCVEC     114    /* -684: the most-wanted call in the corpus    */
#define LVO_FREEVEC      115    /* -690 */
#define LVO_ALLOCENTRY    37    /* -222 */
#define EXECBASE_THISTASK 276   /* ExecBase->ThisTask, the classic offset      */
/* struct Library: ln(14) lib_Flags(14) lib_pad(15) lib_NegSize(16)
 * lib_PosSize(18) lib_Version(20) lib_Revision(22). Programs written for
 * AmigaOS 2.0 and later routinely check lib_Version before doing anything and
 * quit silently if it is too low - which a zeroed base always is. */
#define LIB_VERSION_OFF   20
#define LIB_REVISION_OFF  22
#define GUEST_LIB_VERSION 39    /* what an OS 3.0 program expects to find      */
#define GUEST_LIB_REV     106
#define LVO_ALLOCATE      31    /* -186: allocate from a specific MemHeader     */
#define LVO_CREATEMSGPORT 111   /* -666 */
#define LVO_DELETEMSGPORT 112   /* -672 */
#define LVO_INSERT        39    /* -234 */
#define LVO_ADDHEAD       40    /* -240 */
#define LVO_ADDTAIL       41    /* -246 */
#define LVO_REMOVE        42    /* -252 */

static const char *g_crash_dir = NULL;

struct emu68k_run {
    void                 *reserve;       /* the PROT_NONE guest-space reservation */
    uint8_t              *arena;         /* the RW window inside it               */
    unsigned long         arena_size;    /* page-aligned DOWN (see the mapping)   */
    unsigned long         hole_mask;
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
    double                deadline;      /* wall-clock limit, 0 = none          */
    uint32_t              last_ioerr;    /* what the guest's IoErr() reports    */
    char                  name[64];      /* for ledger/bundle attribution       */
    /* [T3] opened libraries: guest base -> name, for the OS-call callback */
    struct { uint32_t base; char name[32]; } openlib[LIBBASE_MAX];
    int                   nlib;
    stub_lib             *run_lib;       /* the corpus stub's small heap        */
    uint32_t              exec_heap;     /* exec AllocMem cursor (real programs) */
    uint32_t              exec_heap_end;
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

/* Guest memory accessors: 68k memory is big-endian, so a pointer written for
 * the guest must be written as big-endian regardless of the host. */
static uint32_t gread32(j4_sandbox *sb, uint32_t a)
{
    const uint8_t *p;
    if (a < sb->sandbox_origin || a + 4 > sb->sandbox_origin + sb->size) return 0;
    p = j4_sandbox_host(sb, a);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void gwrite32(j4_sandbox *sb, uint32_t a, uint32_t v)
{
    uint8_t *p;
    if (a < sb->sandbox_origin || a + 4 > sb->sandbox_origin + sb->size) return;
    p = j4_sandbox_host(sb, a);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* =========================== [T3] ReadArgs ==================================
 * Every modern AmigaDOS CLI tool parses its arguments with ReadArgs, so this
 * one call decides whether ordinary command-line software does anything useful.
 *
 * It is implemented HERE, over guest memory, rather than by calling the native
 * ReadArgs - because everything it produces is a POINTER THE PROGRAM
 * DEREFERENCES: the argument strings, the LONG a /N writes, the NULL-terminated
 * array a /M builds. Those have to live in the guest's own address space, so
 * the parse happens there and nothing native is involved.
 *
 * Template syntax covered (the set real tools use):
 *   NAME        positional
 *   /S          switch: present -> DOSTRUE
 *   /K          keyword: must be named
 *   /N          number: array slot points to a LONG
 *   /A          required
 *   /M          multiple: array slot points to a NULL-terminated string vector
 *   /F          rest of the line, taken whole
 *   =ALIAS      alternate name
 * Unsupported combinations fail the AmigaDOS way (NULL + IoErr), never silently.
 */
#define RDA_MAX_ITEMS 32
#define RDA_MAX_TOK   64
#define ERROR_REQUIRED_ARG_MISSING_ 116
#define ERROR_BAD_TEMPLATE_         114
#define ERROR_TOO_MANY_ARGS_        115

struct rda_item {
    char name[32], alias[32];
    int  sw, key, num, req, mult, rest;
};

/* bump-allocate zeroed guest memory for results the program will dereference */
static uint32_t guest_alloc(struct emu68k_run *r, uint32_t size)
{
    uint32_t a;
    size = (size + 7u) & ~7u;
    if (!size || r->exec_heap + size > r->exec_heap_end) return 0;
    a = r->exec_heap; r->exec_heap += size;
    memset(j4_sandbox_host(&r->sb, a), 0, size);
    return a;
}

static uint32_t guest_strdup(struct emu68k_run *r, const char *s, size_t n)
{
    uint32_t a = guest_alloc(r, (uint32_t)n + 1);
    if (!a) return 0;
    memcpy(j4_sandbox_host(&r->sb, a), s, n);
    ((char *)j4_sandbox_host(&r->sb, a))[n] = 0;
    return a;
}

static int rda_parse_template(const char *t, struct rda_item *it, int max)
{
    int n = 0;
    while (*t && n < max) {
        const char *e = strchr(t, ',');
        size_t len = e ? (size_t)(e - t) : strlen(t);
        char buf[96];
        if (len >= sizeof buf) return -1;
        memcpy(buf, t, len); buf[len] = 0;
        memset(&it[n], 0, sizeof it[n]);
        {
            char *slash = strchr(buf, '/');
            char *eq;
            while (slash) {
                char m = (char)toupper((unsigned char)slash[1]);
                switch (m) {
                case 'S': it[n].sw = 1;   break;
                case 'K': it[n].key = 1;  break;
                case 'N': it[n].num = 1;  break;
                case 'A': it[n].req = 1;  break;
                case 'M': it[n].mult = 1; break;
                case 'F': it[n].rest = 1; break;
                case 'T': it[n].sw = 1;   break;   /* toggle: treated as switch */
                default: break;                    /* unknown: ignored, not fatal */
                }
                *slash = 0;
                slash = strchr(slash + 1, '/');
            }
            eq = strchr(buf, '=');
            if (eq) { *eq = 0; snprintf(it[n].alias, sizeof it[n].alias, "%s", eq + 1); }
            snprintf(it[n].name, sizeof it[n].name, "%s", buf);
        }
        n++;
        if (!e) break;
        t = e + 1;
    }
    return n;
}

static int rda_eqname(const char *tok, const struct rda_item *it)
{
    return (it->name[0] && !strcasecmp(tok, it->name)) ||
           (it->alias[0] && !strcasecmp(tok, it->alias));
}

/* ReadArgs(template D1, array D2, rdargs D3) -> RDArgs* in D0 */
static int rda_readargs(struct emu68k_run *r, j4_sandbox *sb,
                        struct j5d_m68k_state *st, char *e, unsigned el)
{
    const char *tmpl = guest_cstr(sb, st->d[1]);
    uint32_t    arr  = st->d[2];
    struct rda_item items[RDA_MAX_ITEMS];
    char  line[1024];
    char *tok[RDA_MAX_TOK];
    int   ntok = 0, nit, i;
    uint32_t used[RDA_MAX_TOK];

    if (!tmpl) { snprintf(e, el, "ReadArgs: bad template pointer"); return 1; }
    nit = rda_parse_template(tmpl, items, RDA_MAX_ITEMS);
    if (nit < 0) { r->last_ioerr = ERROR_BAD_TEMPLATE_; st->d[0] = 0; return 0; }

    /* the command line the program was started with, minus the trailing newline */
    {
        const char *src = (const char *)j4_sandbox_host(sb, ARGS_BASE);
        size_t n = 0;
        while (n < sizeof line - 1 && src[n] && src[n] != '\n') { line[n] = src[n]; n++; }
        line[n] = 0;
    }

    /* The AmigaDOS convention: a command line of just "?" means "tell me your
     * arguments". ReadArgs prints the template and (on a real system) reads the
     * real arguments from the next input line. Printing it is the part that
     * matters - it is how a tool documents itself, and it is how you find out
     * what to give a program you have never run. */
    {
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (q[0] == '?' && (q[1] == 0 || q[1] == ' ')) {
            if (r->sink) {
                r->sink(tmpl, (long)strlen(tmpl), r->sink_user);
                r->sink(": \n", 3, r->sink_user);
            }
            r->last_ioerr = 0;
            st->d[0] = 0;            /* no arguments parsed: the program exits  */
            return 0;
        }
    }

    /* split into tokens, honouring "quoted strings" */
    {
        char *p = line;
        while (*p && ntok < RDA_MAX_TOK) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (*p == '"') {
                tok[ntok++] = ++p;
                while (*p && *p != '"') p++;
                if (*p) *p++ = 0;
            } else {
                tok[ntok++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = 0;
            }
        }
    }
    for (i = 0; i < ntok; i++) used[i] = 0;

    /* pass 1: keywords by name (any position) */
    for (i = 0; i < ntok; i++) {
        int k;
        if (used[i]) continue;
        for (k = 0; k < nit; k++) {
            if (!rda_eqname(tok[i], &items[k])) continue;
            used[i] = 1;
            if (items[k].sw) {
                gwrite32(sb, arr + 4u * (uint32_t)k, 1u);          /* DOSTRUE-ish */
            } else if (i + 1 < ntok) {
                used[i + 1] = 1;
                if (items[k].num) {
                    uint32_t cell = guest_alloc(r, 4);
                    gwrite32(sb, cell, (uint32_t)strtol(tok[i + 1], NULL, 10));
                    gwrite32(sb, arr + 4u * (uint32_t)k, cell);
                } else {
                    gwrite32(sb, arr + 4u * (uint32_t)k,
                             guest_strdup(r, tok[i + 1], strlen(tok[i + 1])));
                }
            }
            break;
        }
    }

    /* pass 2: positionals, in template order */
    {
        int t = 0;
        for (i = 0; i < nit; i++) {
            if (items[i].sw || items[i].key) continue;
            if (gread32(sb, arr + 4u * (uint32_t)i)) continue;     /* already set */
            while (t < ntok && used[t]) t++;
            if (t >= ntok) continue;
            if (items[i].rest) {                    /* /F: the rest, joined      */
                char joined[1024]; size_t n = 0;
                for (; t < ntok; t++) {
                    size_t l = strlen(tok[t]);
                    if (n && n + 1 < sizeof joined) joined[n++] = ' ';
                    if (n + l >= sizeof joined) break;
                    memcpy(joined + n, tok[t], l); n += l; used[t] = 1;
                }
                joined[n] = 0;
                gwrite32(sb, arr + 4u * (uint32_t)i, guest_strdup(r, joined, n));
            } else if (items[i].mult) {             /* /M: a string vector       */
                uint32_t vec, cnt = 0, j2;
                for (j2 = (uint32_t)t; j2 < (uint32_t)ntok; j2++) if (!used[j2]) cnt++;
                vec = guest_alloc(r, (cnt + 1) * 4);
                cnt = 0;
                for (j2 = (uint32_t)t; j2 < (uint32_t)ntok; j2++) {
                    if (used[j2]) continue;
                    gwrite32(sb, vec + 4 * cnt,
                             guest_strdup(r, tok[j2], strlen(tok[j2])));
                    used[j2] = 1; cnt++;
                }
                gwrite32(sb, vec + 4 * cnt, 0);     /* NULL terminator           */
                gwrite32(sb, arr + 4u * (uint32_t)i, vec);
            } else if (items[i].num) {
                uint32_t cell = guest_alloc(r, 4);
                gwrite32(sb, cell, (uint32_t)strtol(tok[t], NULL, 10));
                gwrite32(sb, arr + 4u * (uint32_t)i, cell);
                used[t] = 1;
            } else {
                gwrite32(sb, arr + 4u * (uint32_t)i,
                         guest_strdup(r, tok[t], strlen(tok[t])));
                used[t] = 1;
            }
        }
    }

    /* required arguments must have been satisfied */
    for (i = 0; i < nit; i++)
        if (items[i].req && !gread32(sb, arr + 4u * (uint32_t)i)) {
            r->last_ioerr = ERROR_REQUIRED_ARG_MISSING_;
            st->d[0] = 0;                            /* the AmigaDOS failure     */
            return 0;
        }

    /* a guest RDArgs the program can hold and hand to FreeArgs */
    st->d[0] = guest_alloc(r, 64);
    r->last_ioerr = 0;
    return 0;
}

/* [T3] exec.library, served here: this is the bootstrap every AmigaOS program
 * performs before it can do anything else. OpenLibrary hands back a guest base
 * that the engine then recognises, so calls through it arrive at the bridge
 * with A6 naming the library. */
static int exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case LVO_OLDOPENLIB:      /* same thing, older entry point: A1 = name     */
    case LVO_OPENLIBRARY: {
        const char *nm = guest_cstr(sb, st->a[1]);      /* A1 = name, D0 = ver  */
        if (!nm) {
            snprintf(e, el, "OpenLibrary: name pointer A1=%08x is outside the "
                     "guest arena %08x..%08x", st->a[1], sb->sandbox_origin,
                     sb->sandbox_origin + sb->size);
            return 1;
        }
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
        {   /* give the handed-out base a version, for the same reason */
            uint8_t *lb = j4_sandbox_host(sb, base);
            memset(lb, 0, 64);
            lb[LIB_VERSION_OFF]      = (uint8_t)(GUEST_LIB_VERSION >> 8);
            lb[LIB_VERSION_OFF + 1]  = (uint8_t)GUEST_LIB_VERSION;
            lb[LIB_REVISION_OFF]     = (uint8_t)(GUEST_LIB_REV >> 8);
            lb[LIB_REVISION_OFF + 1] = (uint8_t)GUEST_LIB_REV;
        }
        st->d[0] = base;
        return 0;
    }
    case LVO_CLOSELIBRARY:
        st->d[0] = 0;
        return 0;                                        /* bases stay valid    */
    case LVO_AVAILMEM:
        st->d[0] = (r->exec_heap_end > r->exec_heap)
                 ? (r->exec_heap_end - r->exec_heap) : 0;
        return 0;
    case LVO_FREEVEC:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;
    case LVO_ALLOCATE:       /* Allocate(MemHeader A0, size D0): the header is
                              * the guest's idea of where memory comes from; in
                              * this arena there is one place, so serve it. */
    case LVO_ALLOCVEC:       /* the single most-called allocation in real code */
    case LVO_ALLOCMEM: {
        /* Memory a 68k program allocates must live in the GUEST arena: the
         * program dereferences the pointer itself, so it has to be an address
         * the program can reach. Real software asks for real amounts (DMS wants
         * hundreds of KB before it will even start), so this comes from a large
         * region sized to the arena, not from the corpus stub's small heap. */
        uint32_t size = (st->d[0] + 7u) & ~7u;
        if (size == 0 || r->exec_heap + size > r->exec_heap_end) {
            st->d[0] = 0;                                /* AmigaOS: NULL       */
            return 0;
        }
        st->d[0] = r->exec_heap;
        r->exec_heap += size;
        memset(j4_sandbox_host(sb, st->d[0]), 0, size);  /* MEMF_CLEAR-safe     */
        return 0;
    }
    case LVO_FREEMEM:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;

    /* Bookkeeping calls a single-threaded guest can be told the truth about:
     * there is no other task in its arena to arbitrate against. */
    case LVO_FORBID: case LVO_PERMIT:
    case LVO_DISABLE: case LVO_ENABLE:
        return 0;
    case LVO_FINDTASK:
        /* FindTask(NULL) = "me": the guest Process, which exists so that reading
         * pr_CLI says "launched from the Shell". */
        st->d[0] = (st->a[1] == 0) ? GUEST_PROCESS : 0;
        return 0;
    /* Exec list handling operates on GUEST structures, so it is performed in
     * guest memory here rather than handed to the native AROS AddHead, which
     * would manipulate host pointers in a list the program cannot address.
     * (struct Node: ln_Succ at 0, ln_Pred at 4; struct List: lh_Head 0,
     * lh_Tail 4, lh_TailPred 8.) */
    case LVO_ADDHEAD: {
        uint32_t list = st->a[0], node = st->a[1];
        uint32_t head = gread32(sb, list);
        gwrite32(sb, node, head);              /* node->ln_Succ = list->lh_Head */
        gwrite32(sb, node + 4, list);          /* node->ln_Pred = &lh_Head      */
        gwrite32(sb, head + 4, node);          /* head->ln_Pred = node          */
        gwrite32(sb, list, node);              /* list->lh_Head = node          */
        return 0;
    }
    case LVO_ADDTAIL: {
        uint32_t list = st->a[0], node = st->a[1];
        uint32_t tailpred = gread32(sb, list + 8);
        gwrite32(sb, node, list + 4);          /* node->ln_Succ = &lh_Tail      */
        gwrite32(sb, node + 4, tailpred);      /* node->ln_Pred = lh_TailPred   */
        gwrite32(sb, tailpred, node);          /* tailpred->ln_Succ = node      */
        gwrite32(sb, list + 8, node);          /* list->lh_TailPred = node      */
        return 0;
    }
    case LVO_REMOVE: {
        uint32_t node = st->a[1];
        uint32_t succ = gread32(sb, node), pred = gread32(sb, node + 4);
        gwrite32(sb, pred, succ);
        gwrite32(sb, succ + 4, pred);
        return 0;
    }
    case LVO_CREATEMSGPORT: {
        /* A guest MsgPort: the program holds it and may put it in structures,
         * so it is built in guest memory with a properly initialised (empty)
         * message list. Nothing signals a guest task, so it stays empty.
         * Layout: mp_Node(14) mp_Flags(1) mp_SigBit(1) mp_SigTask(4)
         *         mp_MsgList at 20 { lh_Head 20, lh_Tail 24, lh_TailPred 28 } */
        uint32_t port = guest_alloc(r, 34);
        if (!port) { st->d[0] = 0; return 0; }
        j4_sandbox_host(sb, port)[8] = 4;            /* ln_Type = NT_MSGPORT   */
        gwrite32(sb, port + 20, port + 24);          /* lh_Head = &lh_Tail     */
        gwrite32(sb, port + 24, 0);                  /* lh_Tail = NULL         */
        gwrite32(sb, port + 28, port + 20);          /* lh_TailPred = &lh_Head */
        st->d[0] = port;
        return 0;
    }
    /* Ports and semaphores. A guest has one thread of control in its own
     * arena, so arbitration is a no-op; what matters is that the STRUCTURES it
     * initialises look right afterwards, because the program walks them.
     * struct SignalSemaphore: ss_Link(0,14) ss_NestCount(14) ss_WaitQueue(16,
     * MinList: head 16, tail 20, tailpred 24) ss_Owner(36) ss_QueueCount(40) */
    case 93: {               /* InitSemaphore(sigSem A0)                        */
        uint32_t ss = st->a[0];
        if (ss) {
            memset(j4_sandbox_host(sb, ss), 0, 44);
            gwrite32(sb, ss + 16, ss + 20);      /* mlh_Head = &mlh_Tail        */
            gwrite32(sb, ss + 20, 0);            /* mlh_Tail = NULL             */
            gwrite32(sb, ss + 24, ss + 16);      /* mlh_TailPred = &mlh_Head    */
            gwrite32(sb, ss + 36, 0);            /* ss_Owner = NULL             */
            j4_sandbox_host(sb, ss)[40] = 0xFF;  /* ss_QueueCount = -1 (WORD)   */
            j4_sandbox_host(sb, ss)[41] = 0xFF;
        }
        return 0;
    }
    case 94: case 95: case 97: case 98: case 113:
        return 0;            /* Obtain/Release[List]/Shared: nothing to contend */
    case 96:                 /* AttemptSemaphore: always succeeds               */
        st->d[0] = 1;
        return 0;
    case 59: case 60:        /* AddPort / RemPort: no public port list here     */
        return 0;
    case 65:                 /* FindPort(name A1): a guest has no public ports,
                              * and NULL is the answer callers are written for */
        st->d[0] = 0;
        return 0;
    case LVO_DELETEMSGPORT:
        return 0;                                    /* bump heap: nothing to do */
    case LVO_SETSIGNAL:
        /* SetSignal(newSignals D0, signalMask D1) -> old signals. Nothing
         * signals a guest task yet, so the honest answer is a clean zero. */
        st->d[0] = 0;
        return 0;
    default:
        if (getenv("EMU68K_DEBUG_EXEC"))
            fprintf(stderr, "[exec_call] unhandled lvo=%d (ADDHEAD=%d)\n",
                    lvo, LVO_ADDHEAD);
        return 1;                                        /* not served here     */
    }
}

/* EMU68K_TRACE_CALLS: log every library call a program makes. A program that
 * produces no output and reports no gap has given up somewhere, and the call
 * sequence is the only thing that says where. */
static int g_trace = -1;
static void trace_call(struct emu68k_run *r, const char *lib, int lvo,
                       struct j5d_m68k_state *st)
{
    if (g_trace < 0) g_trace = getenv("EMU68K_TRACE_CALLS") ? 1 : 0;
    if (!g_trace) return;
    fprintf(stderr, "[68k] %s LVO %d (%d)  d0=%08x d1=%08x a0=%08x a1=%08x\n",
            lib, lvo, -6 * lvo, st->d[0], st->d[1], st->a[0], st->a[1]);
    (void)r;
}

static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    struct emu68k_run *r = c->run;
    uint32_t a6 = st->a[6];

    /* which library did the program call through? */
    if (r && a6 == EXEC_BASE) {
        trace_call(r, "exec.library", lvo, st);
        if (el) e[0] = 0;
        if (exec_call(r, c->sb, lvo, st, e, el) == 0) return 0;
        if (e[0]) {          /* exec_call said something specific: do not bury it
                              * under a generic "capability gap" message */
            ledger_record(lvo, r->name[0] ? r->name : NULL);
            return 1;
        }
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
            trace_call(r, r->openlib[i].name, lvo, st);
            /* [T3] dos calls whose RESULTS are guest pointers are served in the
             * guest: handing back native pointers would give the program
             * addresses it cannot dereference. */
            if (!strcmp(r->openlib[i].name, "dos.library")) {
                if (lvo == 133) return rda_readargs(r, c->sb, st, e, el);
                if (lvo == 134) { st->d[0] = 0; return 0; }   /* FreeArgs        */
                if (lvo == 22) {                              /* IoErr           */
                    st->d[0] = r->last_ioerr; return 0;
                }
                if (lvo == 38) {          /* AllocDosObject(type D1, tags D2)   */
                    /* The program walks and fills these itself (a FileInfoBlock
                     * is 260 bytes and the largest of the family), so hand back
                     * zeroed GUEST memory rather than a native structure. */
                    st->d[0] = guest_alloc(r, 512);
                    return 0;
                }
                if (lvo == 39) { st->d[0] = 0; return 0; }   /* FreeDosObject   */
            }
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
    if (r->deadline > 0.0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if ((double)ts.tv_sec + ts.tv_nsec / 1e9 > r->deadline) {
            r->kill_req = 1;
            return J5D_POLL_KILL;
        }
    }
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
    {   /* Open the arena, then punch the hardware windows back out. Page
         * alignment matters in BOTH directions here: mprotect rounds a length
         * up, so a hole must be aligned DOWN at its start and UP at its end or
         * the registers it is meant to exclude stay mapped. */
        long pg = sysconf(_SC_PAGESIZE);
        unsigned long psz = (pg > 0) ? (unsigned long)pg : 0x4000u;
        unsigned long mask = psz - 1u;
        struct { uint32_t lo, hi; } hole[2] = {
            { HW_CIA_LO,    HW_CIA_HI    },
            { HW_CUSTOM_LO, HW_CUSTOM_HI },
        };
        int i;
        r->arena_size = SANDBOX_SIZE & ~mask;
        if (mprotect(r->arena, r->arena_size, PROT_READ | PROT_WRITE) != 0) {
            snprintf(err, errlen, "cannot map the guest arena");
            goto fail;
        }
        r->hole_mask = mask;      /* punched after the arena is zeroed, below */
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
    {   /* NOW punch the hardware windows out. It has to happen after the arena
         * is initialised, because initialising it zeroes the whole range - and
         * memset walking into a PROT_NONE hole faults in OUR code, where there
         * is no 68k program to blame and no recovery target registered. */
        long pg = sysconf(_SC_PAGESIZE);
        unsigned long psz = (pg > 0) ? (unsigned long)pg : 0x4000u;
        unsigned long mask = psz - 1u;
        struct { uint32_t lo, hi; } hole[2] = {
            { HW_CIA_LO, HW_CIA_HI }, { HW_CUSTOM_LO, HW_CUSTOM_HI },
        };
        int i;
        for (i = 0; i < 2; i++) {
            unsigned long lo = hole[i].lo & ~mask;
            unsigned long hi = (hole[i].hi + psz) & ~mask;
            mprotect((uint8_t *)r->reserve + lo, hi - lo, PROT_NONE);
        }
    }
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

    /* plant the guest Process: NT_PROCESS, and a non-NULL pr_CLI so startup
     * code takes the Shell path instead of waiting for a Workbench message */
    {
        uint8_t *pr = j4_sandbox_host(&r->sb, GUEST_PROCESS);
        memset(pr, 0, 256);
        pr[PR_TASK_LN_TYPE] = NT_PROCESS;
        /* Startup code usually reads ExecBase->ThisTask rather than calling
         * FindTask; with that zero it walks into address 0, reads pr_CLI as
         * zero, decides "launched from Workbench" and waits for a message that
         * never arrives. Pointing it at the guest Process is what puts real
         * programs on the Shell path. */
        {
            uint8_t *eb = j4_sandbox_host(&r->sb, EXEC_BASE);
            memset(eb, 0, 512);
            eb[LIB_VERSION_OFF]      = (uint8_t)(GUEST_LIB_VERSION >> 8);
            eb[LIB_VERSION_OFF + 1]  = (uint8_t)GUEST_LIB_VERSION;
            eb[LIB_REVISION_OFF]     = (uint8_t)(GUEST_LIB_REV >> 8);
            eb[LIB_REVISION_OFF + 1] = (uint8_t)GUEST_LIB_REV;
            eb[EXECBASE_THISTASK + 0] = (uint8_t)(GUEST_PROCESS >> 24);
            eb[EXECBASE_THISTASK + 1] = (uint8_t)(GUEST_PROCESS >> 16);
            eb[EXECBASE_THISTASK + 2] = (uint8_t)(GUEST_PROCESS >> 8);
            eb[EXECBASE_THISTASK + 3] = (uint8_t)(GUEST_PROCESS);
        }
        {   /* pr_CLI is a BPTR: the program will BADDR it */
            uint32_t cli_b = GUEST_MKBADDR(GUEST_CLI);
            pr[PR_CLI_OFFSET + 0] = (uint8_t)(cli_b >> 24);
            pr[PR_CLI_OFFSET + 1] = (uint8_t)(cli_b >> 16);
            pr[PR_CLI_OFFSET + 2] = (uint8_t)(cli_b >> 8);
            pr[PR_CLI_OFFSET + 3] = (uint8_t)(cli_b);
        }
        memset(j4_sandbox_host(&r->sb, GUEST_CLI), 0, 128);
    }
    /* EMU68K_MAX_SECONDS: a wall-clock limit per run. Off by default; a sweep
     * over unknown software sets it so one program that waits forever cannot
     * stall the batch. Enforced at quantum boundaries, so it lands even inside
     * a chained loop. */
    {
        const char *lim = getenv("EMU68K_MAX_SECONDS");
        double secs = lim && *lim ? atof(lim) : 0.0;
        if (secs > 0.0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            r->deadline = (double)ts.tv_sec + ts.tv_nsec / 1e9 + secs;
        }
    }
    memset(j4_sandbox_host(&r->sb, GUEST_FH_BASE), 0,
           GUEST_FH_SLOT * GUEST_FH_MAX);        /* the guest handle structures */
    r->run_lib   = &r->lib;
    /* the exec heap: everything between the loaded program and the stack, which
     * on a ~10 MiB arena is megabytes - what real Amiga software expects. */
    r->exec_heap     = (r->sb.next_alloc + 0xFFFFu) & ~0xFFFFu;
    if (r->exec_heap < PROG_ORIGIN) r->exec_heap = PROG_ORIGIN;
    /* allocate below the first hardware hole, so a big allocation never spans it */
    r->exec_heap_end = HW_CIA_LO - 0x00010000u;
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
