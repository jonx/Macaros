/* emu68k_host.c — the host-side 68k execution service (libemu68k.dylib).
 * (OURS, AROS-licensed. Links the engine via libjit68k; contains NO Emu68 source.)
 *
 * Composes the proven pieces exactly as run68k does — the [J4] loader, the stub
 * OS, the [J5d] engine with [T0-P3] instances/safe-points and [J5n] diagnostics
 * with fault containment — behind the small quantum-run API of emu68k_host.h,
 * so hosted AROS (emu68k.library) can drive real 68k programs from inside the
 * OS: bounded quanta, streaming output, async kill, contained faults. */

#include "emu68k_host.h"
#include "emu68k_genlibs.h"
#include "scan68k.h"
#include "guestlib68k.h"

#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "j5n_diag.h"
#include "emu68k_guest_offsets.h"
#include "j5n_symbols.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "nativelib/rawdofmt_blob.h"

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
#define LIBBASE         0x0021E000u

/* The base a run starts with in A6, which is also the one extra address the
 * engine will accept as a library base when it recognises a vector call by
 * where it lands. That heuristic exists because real code calls through a copy
 * of the base (`move.l a6,a0 ; jsr -42(a0)`), and it costs one false positive
 * for every ordinary indirect call that happens to land just below a base.
 *
 * So the stub OS's base is only offered when the stub OS is the OS. With the
 * real one bridged it is not a library at all, and advertising it turned an
 * ordinary indirect call in a real program into a call on a library that does
 * not exist. Exec's base is the honest answer there, and is what a 68k program
 * expects to find in A6 anyway. */
#define RUN_LIBBASE     (g_oscall ? EXEC_BASE : LIBBASE)
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u
#define PROG_ORIGIN     0x00250000u
#define ARGS_BASE       0x00238000u
#define ARGS_REGION_END 0x00240000u
/* [T3] In-guest OS code: 68k routines the bridge REDIRECTS to instead of
 * serving natively, because they take a callback into the program's own code.
 * Sits between the argument region and the program, which starts at 0x250000. */
#define OSCODE_BASE     0x00240000u
#define OSCODE_END      0x00250000u
#define OSCODE_RAWDOFMT OSCODE_BASE
#define OSCODE_RETURN   (OSCODE_END - 2u) /* permanent RTS for reclaim redirects */
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
#define GUEST_PROCESS   0x00210000u
#define GUEST_CLI       0x00211000u
#define GUEST_COMMAND   (GUEST_CLI + 64u)
#define PR_TASK_LN_TYPE 8            /* tc_Node.ln_Type: NT_PROCESS = 13       */
#define PR_CLI_OFFSET   172
#define CLI_COMMAND_OFF M68K_CommandLineInterface_cli_CommandName
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
#define GUEST_FH_BASE   0x00212000u
#define GUEST_FH_SLOT   64u
#define GUEST_FH_MAX    32u
#define GUEST_BADDR(b)    ((b) << 2)

/* exec LVOs a program uses to get going (negative offset / 6). */
#define LVO_OPENLIBRARY   92    /* -552 */
#define LVO_RAWDOFMT      87    /* -522: the printf engine, run in the guest */
#define LVO_CLOSELIBRARY  69    /* -414 */
#define LVO_ALLOCMEM      33    /* -198 */
#define LVO_FREEMEM       35    /* -210 */
#define LVO_FORBID        22    /* -132 */
#define LVO_PERMIT        23    /* -138 */
#define LVO_DISABLE       27    /* -162 */
#define LVO_ENABLE        28    /* -168 */
#define LVO_FINDTASK      49    /* -294 */
#define LVO_SETSIGNAL     51    /* -306 */
#define LVO_ALLOCSIGNAL   55    /* -330 */
#define LVO_FREESIGNAL    56    /* -336 */
#define LVO_OPENDEVICE    74    /* -444 */
#define LVO_WAIT          53    /* -318 */
#define LVO_SIGNAL        54    /* -324 */
#define LVO_ADDPORT       59    /* -354 */
#define LVO_REMPORT       60    /* -360 */
#define LVO_PUTMSG        61    /* -366 */
#define LVO_GETMSG        62    /* -372 */
#define LVO_REPLYMSG      63    /* -378 */
#define LVO_WAITPORT      64    /* -384 */
#define LVO_FINDPORT      65    /* -390 */
#define MP_SIGTASK   M68K_MsgPort_mp_SigTask
#define MP_SIGBIT    M68K_MsgPort_mp_SigBit
#define MP_MSGLIST   M68K_MsgPort_mp_MsgList_lh_Head
#define MN_REPLYPORT M68K_Message_mn_ReplyPort
#define TASK_SIGRECVD_OFF M68K_Task_tc_SigRecvd
#define LVO_OLDOPENLIB    68    /* -408: what pre-2.0 programs still call      */
#define LVO_AVAILMEM      36    /* -216 */
#define LVO_ALLOCVEC     114    /* -684: the most-wanted call in the corpus    */
#define LVO_FREEVEC      115    /* -690 */
#define LVO_CREATEPOOL   116    /* -696 */
#define LVO_DELETEPOOL   117    /* -702 */
#define LVO_ALLOCPOOLED  118    /* -708 */
#define LVO_FREEPOOLED   119    /* -714 */
#define LVO_ALLOCENTRY    37    /* -222 */
#define EXECBASE_THISTASK M68K_ExecBase_ThisTask
/* struct Library: ln(14) lib_Flags(14) lib_pad(15) lib_NegSize(16)
 * lib_PosSize(18) lib_Version(20) lib_Revision(22). Programs written for
 * AmigaOS 2.0 and later routinely check lib_Version before doing anything and
 * quit silently if it is too low - which a zeroed base always is. */
#define LIB_VERSION_OFF   M68K_Library_lib_Version
#define LIB_REVISION_OFF  M68K_Library_lib_Revision
#define GUEST_LIB_VERSION 39    /* what an OS 3.0 program expects to find      */
#define GUEST_LIB_REV     106
#define LVO_ALLOCATE      31    /* -186: allocate from a specific MemHeader     */
#define LVO_CREATEMSGPORT 111   /* -666 */
#define LVO_DELETEMSGPORT 112   /* -672 */
#define LVO_INSERT        39    /* -234 */
#define LVO_ADDHEAD       40    /* -240 */
#define LVO_ADDTAIL       41    /* -246 */
#define LVO_REMOVE        42    /* -252 */
#define LVO_COPYMEM      104    /* -624 */
#define LVO_COPYMEMQUICK 105    /* -630 */
#define LVO_CACHECLEARU  106    /* -636 */
#define LVO_CACHECLEARE  107    /* -642 */
#define LVO_SUPERVISOR     5    /* -30  */
#define LVO_REMHEAD       43    /* -258 */
#define LVO_REMTAIL       44    /* -264 */
#define LVO_ENQUEUE       45    /* -270 */
#define LVO_STACKSWAP    122    /* -732 */
#define TASK_SIGALLOC_OFF M68K_Task_tc_SigAlloc
#define TASK_SPREG_OFF    M68K_Task_tc_SPReg
#define TASK_SPLOWER_OFF  M68K_Task_tc_SPLower
#define TASK_SPUPPER_OFF  M68K_Task_tc_SPUpper

/* Private exec vectors used only by synthesized guest loader continuations.
 * They are inside the engine's vector-recognition window but beyond Exec's
 * public table. */
#define LVO_GL_INIT_DONE  650   /* -3900 */
#define LVO_GL_OPEN_DONE  651   /* -3906 */
#define LVO_GL_CLOSE_DONE 652   /* -3912 */
#define LVO_GL_RECLAIM    653   /* -3918 */

#define GUESTLIB_MAX 16
#define GUESTSEG_MAX 32
#define GUESTPOOL_MAX 32
/* Opens are tracked one entry per live open, not one per library. A library's
 * Open vector returns the base the CALLER must use, and it is free to return a
 * different one to every opener: that is how a library keeps per-opener state,
 * and it is ordinary AmigaOS, not an oddity. Duplicates are expected (a library
 * that hands back its own base every time appears N times) because each entry
 * is one reference, which is exactly what CloseLibrary decrements. */
#define GUESTLIB_OPENS_MAX 32
enum guestlib_state {
    GL_EMPTY = 0, GL_LOADING, GL_OPENING, GL_READY, GL_FAILED, GL_UNLOADED
};

struct guestlib_live {
    enum guestlib_state state;
    char                name[64];
    char                path[PATH_MAX];
    j4_seglist          seg;
    gl68_resident       resident;
    gl68_init           init;
    uint32_t            base;
    uint32_t            requested_version;
    uint32_t            init_trampoline;
    uint32_t            open_trampoline;
    uint32_t            close_trampoline;
    uint32_t            mem_start;
    uint32_t            mem_end;
    uint32_t            open_base[GUESTLIB_OPENS_MAX];  /* one per live open  */
    int                 open_count;
    uint32_t            closing_base;   /* which one CloseLibrary is unwinding */
    int                 parent;
    int                 reclaim_pending;
    uint32_t            saved_d[6];       /* ABI-preserved D2-D7 around exec */
    uint32_t            saved_a[5];       /* ABI-preserved A2-A6 around exec */
};

struct guestseg_live {
    int        live;
    char       name[128];
    j4_seglist seg;
    uint32_t   bptr;
    uint32_t   mem_start;
    uint32_t   mem_end;
};

struct guestpool_live {
    int      live;
    uint32_t guest;
    uint32_t requirements;
};

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
    char                  name[PATH_MAX];/* attribution + PROGDIR host fallback */
    /* [T3] native facades: guest base -> name, for the OS-call callback */
    struct { uint32_t base; char name[32]; } openlib[LIBBASE_MAX];
    int                   nlib;
    /* [T3e] disk-loaded libraries execute inside this run's guest arena. */
    struct guestlib_live  guestlib[GUESTLIB_MAX];
    struct guestseg_live  guestseg[GUESTSEG_MAX];
    struct guestpool_live guestpool[GUESTPOOL_MAX];
    int                   active_loader;
    stub_lib             *run_lib;       /* the corpus stub's small heap        */
    uint32_t              exec_heap;     /* exec AllocMem cursor (real programs) */
    uint32_t              exec_heap_end;
    uint32_t              stack_lower;
    uint32_t              stack_upper;
    uint32_t              callback_stack_top;
    uint32_t              poll_quantum;
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
static char g_hw_detail[224];

/* Where the access came from, in the program's own terms: the PC the running
 * block chain was entered at, and the 68k register that was carrying the
 * address. Without this an address alone says only THAT the program left the
 * arena, and the reader is back to guessing which pointer went bad. */
static char g_hw_origin[96];

static void describe_origin(unsigned long long guest)
{
    const struct j5d_m68k_state *st = j5n_signal_guest_state();
    char reg[16] = "";
    int i;

    g_hw_origin[0] = 0;
    if (!st)
        return;

    /* An exact register match names the pointer; a small positive delta names
     * the base it was derived from, which is what a struct access looks like. */
    for (i = 0; i < 8 && !reg[0]; i++) {
        if (st->a[i] == guest)                 snprintf(reg, sizeof reg, "A%d", i);
        else if (guest > st->a[i] && guest - st->a[i] <= 0x200)
            snprintf(reg, sizeof reg, "A%d+%llu", i, guest - st->a[i]);
    }
    for (i = 0; i < 8 && !reg[0]; i++)
        if (st->d[i] == guest) snprintf(reg, sizeof reg, "D%d", i);

    snprintf(g_hw_origin, sizeof g_hw_origin, ", from PC $%06X%s%s",
             st->pc, reg[0] ? " via " : "", reg);
}

/* The guest code at the fault PC, as raw bytes. A verdict tells you an address
 * went out of the arena; only the instructions tell you where the pointer was
 * built, so dump enough of the block to disassemble it. Opt-in: this is a
 * developer aid, not part of the verdict a user sees. */
static void dump_fault_code(struct emu68k_run *r)
{
    const struct j5d_m68k_state *st = j5n_signal_guest_state();
    const unsigned char *mem;
    unsigned long off;
    int i;

    if (!st || !getenv("EMU68K_TRACE_FAULT"))
        return;
    if (st->pc < r->sb.sandbox_origin ||
        st->pc + 96 >= r->sb.sandbox_origin + r->sb.size)
        return;
    mem = (const unsigned char *)r->sb.host_mem;
    off = st->pc - r->sb.sandbox_origin;

    fprintf(stderr, "[emu68k] code at PC $%06X:", st->pc);
    for (i = 0; i < 96; i++)
        fprintf(stderr, "%s%02X", (i % 16) ? "" : "\n  ", mem[off + i]);
    fprintf(stderr, "\n[emu68k] D0-D7");
    for (i = 0; i < 8; i++) fprintf(stderr, " %08X", st->d[i]);
    fprintf(stderr, "\n[emu68k] A0-A7");
    for (i = 0; i < 8; i++) fprintf(stderr, " %08X", st->a[i]);
    fprintf(stderr, "\n");
}

static int classify_hardware(void *fault_addr, void *user)
{
    struct emu68k_run *r = user;
    unsigned long long host = (unsigned long long)(uintptr_t)fault_addr;
    unsigned long long base = (unsigned long long)(uintptr_t)r->sb.host_mem;
    unsigned long long guest;

    if (host < base - 0x10000000ull || host > base + 0x10000000ull) {
        if (getenv("EMU68K_TRACE_FAULT"))
            fprintf(stderr, "[emu68k] fault host=%llx outside the window around "
                    "base=%llx: not decodable to a guest address\n", host, base);
        return 0;
    }
    guest = host - base + r->sb.sandbox_origin;
    describe_origin(guest);
    dump_fault_code(r);

    if (guest >= HW_CUSTOM_LO && guest <= HW_CUSTOM_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail,
                 "custom chip register $%06llX%s", guest, g_hw_origin);
    else if (guest >= HW_CIA_LO && guest <= HW_CIA_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail, "CIA register $%06llX%s", guest, g_hw_origin);
    else if (guest <= HW_VECTOR_HI)
        snprintf(g_hw_detail, sizeof g_hw_detail,
                 "exception vector page $%03llX%s", guest, g_hw_origin);
    else if (guest < r->sb.sandbox_origin ||
             guest >= (unsigned long long)r->sb.sandbox_origin + r->sb.size)
        /* Everything below the arena is reserved and unmapped on purpose: it is
         * machine address space this sandbox does not provide (ROM, chip RAM
         * the program did not allocate, the autoconfig area). A program reading
         * or writing there is ADDRESSING THE MACHINE, which is a routing
         * verdict - it needs a full emulator - and not a wild pointer.
         *
         * Only the vector page and the chip windows were recognised before, so
         * a memory tool that pokes anywhere else was reported as a crash. That
         * is the wrong answer twice over: it blames the program for a fault the
         * sandbox created deliberately, and it hides the one thing worth
         * knowing, which is that this program wants the real machine. */
        snprintf(g_hw_detail, sizeof g_hw_detail,
                 "machine address $%06llX%s, outside the memory this sandbox "
                 "provides ($%06X..$%06llX)", guest, g_hw_origin,
                 r->sb.sandbox_origin,
                 (unsigned long long)r->sb.sandbox_origin + r->sb.size);
    else {
        /* Not one of the regions with a meaning. Say WHICH address, because a
         * fault that cannot be named is a fault that gets diagnosed by
         * guesswork - which has been wrong every time on this port. */
        if (getenv("EMU68K_TRACE_FAULT"))
            fprintf(stderr, "[emu68k] unclassified fault at guest $%06llX%s "
                    "(arena $%06X..$%06llX)\n", guest, g_hw_origin,
                    r->sb.sandbox_origin,
                    (unsigned long long)r->sb.sandbox_origin + r->sb.size);
        return 0;                                  /* a genuine wild access     */
    }
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
static uint8_t gread8(j4_sandbox *sb, uint32_t a)
{
    return *(const uint8_t *)j4_sandbox_host(sb, a);
}

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
#define ERROR_NO_FREE_STORE_         103
#define ERROR_OBJECT_NOT_FOUND_      205
#define ERROR_BAD_HUNK_              235

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
    /* Runtime hunk loads and Exec allocations share one monotonic cursor. A
     * separate stale j4 cursor would let the next library overwrite memory the
     * program already received from AllocMem. */
    if (r->sb.next_alloc < r->exec_heap) r->sb.next_alloc = r->exec_heap;
    memset(j4_sandbox_host(&r->sb, a), 0, size);
    return a;
}

unsigned long emu68k_run_guest_alloc(emu68k_run *r, unsigned long size)
{
    if (!r || size > UINT32_MAX) return 0;
    return (unsigned long)guest_alloc(r, (uint32_t)size);
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

/* =========================== [T3e] guest libraries ========================= */

static void put_be16(j4_sandbox *sb, uint32_t at, uint16_t v)
{
    uint8_t *p = j4_sandbox_host(sb, at);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put_be32(j4_sandbox *sb, uint32_t at, uint32_t v)
{
    uint8_t *p = j4_sandbox_host(sb, at);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void emit_word(j4_sandbox *sb, uint32_t *pc, uint16_t v)
{
    put_be16(sb, *pc, v); *pc += 2u;
}

static void emit_long(j4_sandbox *sb, uint32_t *pc, uint32_t v)
{
    put_be32(sb, *pc, v); *pc += 4u;
}

static void emit_move_d1(j4_sandbox *sb, uint32_t *pc, uint32_t v)
{
    emit_word(sb, pc, 0x223cu); emit_long(sb, pc, v); /* move.l #v,d1 */
}

static void emit_move_a6(j4_sandbox *sb, uint32_t *pc, uint32_t v)
{
    emit_word(sb, pc, 0x2c7cu); emit_long(sb, pc, v); /* movea.l #v,a6 */
}

static void emit_jsr_a6(j4_sandbox *sb, uint32_t *pc, int lvo)
{
    emit_word(sb, pc, 0x4eaeu);
    emit_word(sb, pc, (uint16_t)(int16_t)(-6 * lvo));
}

static void emit_jmp_a6(j4_sandbox *sb, uint32_t *pc, int lvo)
{
    emit_word(sb, pc, 0x4eeeu);
    emit_word(sb, pc, (uint16_t)(int16_t)(-6 * lvo));
}

static uint32_t make_open_trampoline(struct emu68k_run *r, int idx,
                                     uint32_t initpc, int call_init)
{
    uint32_t start = guest_alloc(r, call_init ? 40u : 34u), pc = start;
    if (!start) return 0;
    if (call_init) {
        emit_word(&r->sb, &pc, 0x4eb9u);             /* jsr abs.l initpc */
        emit_long(&r->sb, &pc, initpc);
    }
    emit_move_d1(&r->sb, &pc, (uint32_t)idx);
    emit_move_a6(&r->sb, &pc, EXEC_BASE);
    emit_jsr_a6(&r->sb, &pc, LVO_GL_INIT_DONE);
    emit_move_d1(&r->sb, &pc, (uint32_t)idx);
    emit_move_a6(&r->sb, &pc, EXEC_BASE);
    emit_jsr_a6(&r->sb, &pc, LVO_GL_OPEN_DONE);
    emit_word(&r->sb, &pc, 0x4e75u);                 /* rts */
    return start;
}

/* A6 is NOT baked in: it is the base being closed, which the dispatcher sets,
 * because a library that hands a different base to each opener has to have each
 * of them closed on its own. One trampoline therefore serves every base the
 * library ever handed out. */
static uint32_t make_close_trampoline(struct emu68k_run *r, int idx)
{
    uint32_t start = guest_alloc(r, 32u), pc = start;
    if (!start) return 0;
    emit_jsr_a6(&r->sb, &pc, 2);                    /* library Close, -12 */
    emit_move_d1(&r->sb, &pc, (uint32_t)idx);
    emit_move_a6(&r->sb, &pc, EXEC_BASE);
    emit_jsr_a6(&r->sb, &pc, LVO_GL_CLOSE_DONE);
    /* Tail-call the reclaim safe point. If Close caused Expunge, the library
     * image and this trampoline are about to disappear, so an RTS here would
     * fetch its opcode from released/reused memory. The private vector clears
     * stale translated blocks and redirects to OSCODE_RETURN instead. */
    emit_jmp_a6(&r->sb, &pc, LVO_GL_RECLAIM);
    return start;
}

static uint8_t *read_host_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    fclose(f); *len = (size_t)n; return p;
}

/* A file at the right path is not the same as the right file. LIBS: on a booted
 * AROS holds NATIVE libraries, so a guest asking for one that AROS also has by
 * name finds an ELF object where a hunk file has to be. Taking it and failing
 * would end the search at the first wrong answer, so a candidate that is not a
 * 68k hunk file is discarded and the search carries on to the places a
 * program's own 68k libraries actually live. */
static uint8_t *take_if_hunk(uint8_t *image, size_t len)
{
    if (image && (len < 4u ||
                  ((uint32_t)image[0] << 24 | (uint32_t)image[1] << 16 |
                   (uint32_t)image[2] << 8 | image[3]) != J4_HUNK_HEADER)) {
        free(image);
        return NULL;
    }
    return image;
}

static uint8_t *try_library_at(const char *dir, size_t dirlen, const char *leaf,
                               char *found, size_t foundlen, size_t *imagelen)
{
    char path[PATH_MAX];
    int n;
    if (!dirlen) n = snprintf(path, sizeof path, "%s", leaf);
    else n = snprintf(path, sizeof path, "%.*s/%s", (int)dirlen, dir, leaf);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    uint8_t *p = take_if_hunk(read_host_file(path, imagelen), *imagelen);
    if (p) snprintf(found, foundlen, "%s", path);
    return p;
}

static int loader_dos_call(struct emu68k_run *r, int lvo,
                           struct j5d_m68k_state *st)
{
    char err[160] = {0};
    if (!g_oscall) return 1;
    return g_oscall("dos.library", lvo, st, r->reserve, g_oscall_user,
                    err, sizeof err);
}

/* Read through native AROS dos.library so LIBS: and PROGDIR: retain their real
 * assign/current-directory semantics. The existing OS bridge's BPTR token table
 * owns the native handle; bytes cross through one bounded guest scratch buffer. */
static uint8_t *read_aros_file(struct emu68k_run *r, const char *path,
                               size_t *imagelen)
{
    enum { DOS_OPEN = 5, DOS_CLOSE = 6, DOS_READ = 7, DOS_SEEK = 11 };
    struct j5d_m68k_state st;
    uint32_t guest_path, handle, scratch;
    uint8_t *image = NULL;
    uint32_t size, done = 0;
    size_t pathlen = strlen(path);

    guest_path = guest_strdup(r, path, pathlen);
    if (!guest_path) return NULL;
    memset(&st, 0, sizeof st);
    st.d[1] = guest_path; st.d[2] = 1005u;            /* MODE_OLDFILE */
    if (loader_dos_call(r, DOS_OPEN, &st) || !(handle = st.d[0])) return NULL;

    st.d[1] = handle; st.d[2] = 0; st.d[3] = 1;      /* OFFSET_END */
    if (loader_dos_call(r, DOS_SEEK, &st) || st.d[0] == 0xffffffffu) goto close;
    st.d[1] = handle; st.d[2] = 0; st.d[3] = 0;      /* OFFSET_CURRENT */
    if (loader_dos_call(r, DOS_SEEK, &st) || st.d[0] == 0xffffffffu) goto close;
    size = st.d[0];
    st.d[1] = handle; st.d[2] = 0; st.d[3] = 0xffffffffu; /* OFFSET_BEGINNING */
    if (!size || size > r->exec_heap_end - r->exec_heap ||
        loader_dos_call(r, DOS_SEEK, &st) || st.d[0] == 0xffffffffu)
        goto close;

    image = malloc(size);
    scratch = guest_alloc(r, size < 65536u ? size : 65536u);
    if (!image || !scratch) { free(image); image = NULL; goto close; }
    while (done < size) {
        uint32_t want = size - done;
        if (want > 65536u) want = 65536u;
        memset(&st, 0, sizeof st);
        st.d[1] = handle; st.d[2] = scratch; st.d[3] = want;
        if (loader_dos_call(r, DOS_READ, &st) || !st.d[0] || st.d[0] > want) {
            free(image); image = NULL; goto close;
        }
        memcpy(image + done, j4_sandbox_host(&r->sb, scratch), st.d[0]);
        done += st.d[0];
    }
    *imagelen = size;

close:
    memset(&st, 0, sizeof st); st.d[1] = handle;
    loader_dos_call(r, DOS_CLOSE, &st);
    return image;
}

/* Host-side equivalent of LIBS: plus PROGDIR fallback. The installed AROS seam
 * can provide its host-visible LIBS directories through EMU68K_LIBS_PATH; a
 * standalone run also searches beside the launched program and ./Libs. */
static uint8_t *resolve_guest_library(struct emu68k_run *r, const char *name,
                                      char *found, size_t foundlen, size_t *imagelen)
{
    const char *leaf = name, *paths, *slash;
    int progdir_only = 0;
    uint8_t *p;
    if (g_oscall) {
        char dospath[PATH_MAX];
        if (strchr(name, ':')) {
            if ((p = take_if_hunk(read_aros_file(r, name, imagelen), *imagelen))) {
                snprintf(found, foundlen, "%s", name); return p;
            }
        } else {
            snprintf(dospath, sizeof dospath, "LIBS:%s", name);
            if ((p = take_if_hunk(read_aros_file(r, dospath, imagelen), *imagelen))) {
                snprintf(found, foundlen, "%s", dospath); return p;
            }
            snprintf(dospath, sizeof dospath, "PROGDIR:%s", name);
            if ((p = take_if_hunk(read_aros_file(r, dospath, imagelen), *imagelen))) {
                snprintf(found, foundlen, "%s", dospath); return p;
            }
            /* A package that ships its own 68k libraries keeps them beside the
             * program, which is where its own startup script assigns LIBS: to
             * before running it. */
            snprintf(dospath, sizeof dospath, "PROGDIR:libs/%s", name);
            if ((p = take_if_hunk(read_aros_file(r, dospath, imagelen), *imagelen))) {
                snprintf(found, foundlen, "%s", dospath); return p;
            }
        }
    }
    if (!strncmp(leaf, "PROGDIR:", 8)) { leaf += 8; progdir_only = 1; }
    else if (!strncmp(leaf, "LIBS:", 5)) leaf += 5;
    if (!*leaf || strstr(leaf, "..") || strchr(leaf, '/') || strchr(leaf, ':'))
        return NULL;

    slash = strrchr(r->name, '/');
    if (progdir_only) {
        if (!slash) return NULL;
        return try_library_at(r->name, (size_t)(slash - r->name), leaf,
                              found, foundlen, imagelen);
    }

    paths = getenv("EMU68K_LIBS_PATH");
    while (paths && *paths) {
        const char *end = strchr(paths, ':');
        size_t n = end ? (size_t)(end - paths) : strlen(paths);
        if ((p = try_library_at(paths, n, leaf, found, foundlen, imagelen))) return p;
        paths = end ? end + 1 : NULL;
    }
    if (slash) {
        size_t n = (size_t)(slash - r->name);
        char dir[PATH_MAX];
        if (n + 6u < sizeof dir) {
            memcpy(dir, r->name, n); memcpy(dir + n, "/Libs", 6); n += 5u;
            if ((p = try_library_at(dir, n, leaf, found, foundlen, imagelen))) return p;
        }
        if ((p = try_library_at(r->name, (size_t)(slash - r->name), leaf,
                                found, foundlen, imagelen))) return p;
    }
    if ((p = try_library_at("Libs", 4, leaf, found, foundlen, imagelen))) return p;
    return try_library_at("", 0, leaf, found, foundlen, imagelen);
}

/* dos.LoadSeg as seen BY a 68k program.  Unlike the outer AROS loader's native
 * proxy, this value is dereferenced by guest code, so it is a classic BPTR chain
 * in the guest arena: BADDR(seg) is the link word and BADDR(seg)+4 is the hunk
 * payload.  The common J4 relocator supplies guest addresses throughout. */
static int dos_guest_loadseg(struct emu68k_run *r, j4_sandbox *sb,
                             struct j5d_m68k_state *st,
                             char *e, unsigned el)
{
    const char *guest_name = guest_cstr(sb, st->d[1]);
    char name[128], why[256] = {0};
    uint8_t *image;
    size_t imagelen = 0;
    uint32_t mark, bptr;
    int slot = -1;

    if (!guest_name || strlen(guest_name) >= sizeof name) {
        r->last_ioerr = ERROR_OBJECT_NOT_FOUND_;
        st->d[0] = 0;
        return 0;
    }
    snprintf(name, sizeof name, "%s", guest_name);
    for (int i = 0; i < GUESTSEG_MAX; i++)
        if (!r->guestseg[i].live) { slot = i; break; }
    if (slot < 0) {
        r->last_ioerr = ERROR_NO_FREE_STORE_;
        st->d[0] = 0;
        return 0;
    }

    image = read_aros_file(r, name, &imagelen);
    image = take_if_hunk(image, imagelen);
    if (!image) {
        if (getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] LoadSeg(\"%s\") -> not found/not a hunk\n", name);
        r->last_ioerr = ERROR_OBJECT_NOT_FOUND_;
        st->d[0] = 0;
        return 0;
    }

    mark = r->exec_heap > sb->next_alloc ? r->exec_heap : sb->next_alloc;
    r->exec_heap = sb->next_alloc = mark;
    memset(&r->guestseg[slot], 0, sizeof r->guestseg[slot]);
    if (j4_load_hunks_bptr(sb, image, imagelen, &r->guestseg[slot].seg,
                           &bptr, why, sizeof why)) {
        free(image);
        r->exec_heap = sb->next_alloc = mark;
        r->last_ioerr = ERROR_BAD_HUNK_;
        st->d[0] = 0;
        if (getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] LoadSeg(\"%s\") failed: %s\n", name, why);
        return 0;
    }
    free(image);

    r->exec_heap = sb->next_alloc;
    r->guestseg[slot].live = 1;
    r->guestseg[slot].bptr = bptr;
    r->guestseg[slot].mem_start = mark;
    r->guestseg[slot].mem_end = r->exec_heap;
    snprintf(r->guestseg[slot].name, sizeof r->guestseg[slot].name, "%s", name);
    r->last_ioerr = 0;
    st->d[0] = bptr;
    if (getenv("EMU68K_TRACE_CALLS"))
        fprintf(stderr, "[68k] LoadSeg(\"%s\") -> %08x entry=%08x hunks=%d\n",
                name, bptr, r->guestseg[slot].seg.entry,
                r->guestseg[slot].seg.numhunks);
    (void)e; (void)el;
    return 0;
}

static int dos_guest_unloadseg(struct emu68k_run *r,
                               struct j5d_m68k_state *st)
{
    uint32_t bptr = st->d[1];
    for (int i = 0; i < GUESTSEG_MAX; i++) {
        struct guestseg_live *g = &r->guestseg[i];
        if (!g->live || g->bptr != bptr) continue;

        /* Loaded code may have translated entries and incoming block chains.
         * UnLoadSeg is cold, so invalidate the whole per-run cache before any
         * byte can be reused; this is the same conservative rule as guest
         * library expunge and CacheClearE. */
        j5d_run_free();
        if (g->mem_end > g->mem_start)
            memset(j4_sandbox_host(&r->sb, g->mem_start), 0,
                   g->mem_end - g->mem_start);
        if (g->mem_end == r->exec_heap && r->sb.next_alloc == r->exec_heap)
            r->exec_heap = r->sb.next_alloc = g->mem_start;
        memset(g, 0, sizeof *g);
        r->last_ioerr = 0;
        st->d[0] = 0xffffffffu;                    /* DOSTRUE */
        return 0;
    }
    st->d[0] = 0;
    return 0;
}

static int find_guestlib_name(struct emu68k_run *r, const char *name)
{
    for (int i = 0; i < GUESTLIB_MAX; i++)
        if (r->guestlib[i].state != GL_EMPTY &&
            r->guestlib[i].state != GL_UNLOADED &&
            !strcmp(r->guestlib[i].name, name)) return i;
    return -1;
}

static int guestlib_add_open(struct guestlib_live *g, uint32_t base)
{
    if (g->open_count >= GUESTLIB_OPENS_MAX) return -1;
    g->open_base[g->open_count++] = base;
    return 0;
}

/* Drop ONE reference, not every entry with that base: two opens that were
 * handed the same base are two references, and closing one leaves the other. */
static void guestlib_drop_open(struct guestlib_live *g, uint32_t base)
{
    for (int i = 0; i < g->open_count; i++) {
        if (g->open_base[i] != base) continue;
        for (int j = i + 1; j < g->open_count; j++)
            g->open_base[j - 1] = g->open_base[j];
        g->open_count--;
        return;
    }
}

static int guestlib_owns_base(const struct guestlib_live *g, uint32_t base)
{
    if (base && base == g->base) return 1;
    for (int i = 0; i < g->open_count; i++)
        if (g->open_base[i] == base) return 1;
    return 0;
}

/* The base a program holds is whatever Open handed it, which is not necessarily
 * the library's own. Both answer to CloseLibrary. */
static int find_guestlib_base(struct emu68k_run *r, uint32_t base)
{
    for (int i = 0; i < GUESTLIB_MAX; i++)
        if (r->guestlib[i].state == GL_READY &&
            guestlib_owns_base(&r->guestlib[i], base))
            return i;
    return -1;
}

/* A library base has a vector area below it and a Library structure above it,
 * and both have to be inside the arena before anything dereferences either. */
static int plausible_libbase(const struct emu68k_run *r, uint32_t base)
{
    return base >= r->sb.sandbox_origin + 24u &&
           (uint64_t)base + 34u <= (uint64_t)r->sb.sandbox_origin + r->sb.size;
}

static void guestlib_save_preserved(struct guestlib_live *g,
                                    const struct j5d_m68k_state *st)
{
    memcpy(g->saved_d, &st->d[2], sizeof g->saved_d);
    memcpy(g->saved_a, &st->a[2], sizeof g->saved_a);
}

static void guestlib_restore_preserved(const struct guestlib_live *g,
                                       struct j5d_m68k_state *st)
{
    memcpy(&st->d[2], g->saved_d, sizeof g->saved_d);
    memcpy(&st->a[2], g->saved_a, sizeof g->saved_a);
}

static int alloc_guestlib_slot(struct emu68k_run *r)
{
    for (int i = 0; i < GUESTLIB_MAX; i++)
        if (r->guestlib[i].state == GL_EMPTY || r->guestlib[i].state == GL_UNLOADED)
            return i;
    return -1;
}

static void rollback_unexecuted(struct emu68k_run *r, uint32_t mark)
{
    uint32_t end = r->sb.next_alloc > r->exec_heap ? r->sb.next_alloc : r->exec_heap;
    if (end > mark) memset(j4_sandbox_host(&r->sb, mark), 0, end - mark);
    r->sb.next_alloc = mark; r->exec_heap = mark;
}

static int load_guestlib(struct emu68k_run *r, const char *name, uint32_t version,
                         int *idx_out, char *why, unsigned whylen)
{
    size_t imagelen = 0;
    char path[PATH_MAX] = {0};
    uint8_t *image = resolve_guest_library(r, name, path, sizeof path, &imagelen);
    const char *resident_name = name;
    const char *colon = strrchr(name, ':');
    const char *slash = strrchr(name, '/');
    int idx;
    uint32_t mark;
    if (colon && colon + 1 > resident_name) resident_name = colon + 1;
    if (slash && slash + 1 > resident_name) resident_name = slash + 1;
    if (!image) { snprintf(why, whylen, "%s not found in guest library paths", name); return 1; }
    idx = alloc_guestlib_slot(r);
    if (idx < 0) { free(image); snprintf(why, whylen, "too many guest libraries"); return 1; }

    struct guestlib_live *g = &r->guestlib[idx];
    memset(g, 0, sizeof *g);
    g->state = GL_LOADING; g->parent = r->active_loader;
    g->requested_version = version;
    snprintf(g->name, sizeof g->name, "%s", name);
    snprintf(g->path, sizeof g->path, "%s", path);

    mark = r->exec_heap > r->sb.next_alloc ? r->exec_heap : r->sb.next_alloc;
    mark = (mark + 7u) & ~7u;
    r->exec_heap = r->sb.next_alloc = mark;
    g->mem_start = mark;
    if (j4_load_hunks(&r->sb, image, imagelen, 0, &g->seg, why, whylen) ||
        gl68_find_resident(&r->sb, &g->seg, resident_name, &g->resident, why, whylen) ||
        g->resident.version < version ||
        gl68_prepare_init(&r->sb, &g->seg, &g->resident, &g->init, why, whylen)) {
        if (g->resident.version < version && !why[0])
            snprintf(why, whylen, "%s version %u is older than requested %u",
                     name, g->resident.version, version);
        free(image); rollback_unexecuted(r, mark); g->state = GL_UNLOADED;
        return 1;
    }
    free(image);
    r->exec_heap = r->sb.next_alloc;
    g->init_trampoline = make_open_trampoline(r, idx, g->init.init_pc,
                                               g->init.init_pc != 0);
    g->open_trampoline = make_open_trampoline(r, idx, 0, 0);
    if (!g->init_trampoline || !g->open_trampoline) {
        snprintf(why, whylen, "no guest memory for library continuation");
        rollback_unexecuted(r, mark); g->state = GL_UNLOADED; return 1;
    }
    g->mem_end = r->exec_heap;
    r->active_loader = idx;
    *idx_out = idx;
    return 0;
}

static int guestlib_init_done(struct emu68k_run *r, struct j5d_m68k_state *st,
                              char *e, unsigned el)
{
    int idx = (int)st->d[1];
    if (idx < 0 || idx >= GUESTLIB_MAX) { snprintf(e, el, "bad guest-library continuation"); return 1; }
    struct guestlib_live *g = &r->guestlib[idx];
    if (g->state == GL_LOADING) {
        uint32_t base = st->d[0];
        if (!base) {
            g->state = GL_FAILED; r->active_loader = g->parent; st->d[0] = 0;
            return 0;
        }
        if (base < r->sb.sandbox_origin + 24u ||
            (uint64_t)base + 34u > (uint64_t)r->sb.sandbox_origin + r->sb.size) {
            snprintf(e, el, "%s initializer returned invalid base %08x", g->name, base);
            g->state = GL_FAILED; r->active_loader = g->parent; return 1;
        }
        g->base = base; g->state = GL_OPENING;
        j5d_register_guest_libbase(base);
    } else if (g->state != GL_READY) {
        st->d[0] = 0; return 0;
    }
    st->a[6] = g->base;
    st->pc = g->base - 6u;                         /* library Open vector */
    return J5D_LVO_REDIRECT;
}

static int guestlib_open_done(struct emu68k_run *r, struct j5d_m68k_state *st,
                              char *e, unsigned el)
{
    int idx = (int)st->d[1];
    if (idx < 0 || idx >= GUESTLIB_MAX) return 1;
    struct guestlib_live *g = &r->guestlib[idx];
    if (!st->d[0]) {
        if (g->state == GL_OPENING) {
            j5d_unregister_libbase(g->base); g->base = 0; g->state = GL_FAILED;
            r->active_loader = g->parent;
        }
        guestlib_restore_preserved(g, st);
        return 0;
    }
    /* Open returns the base the CALLER must use. A library is entitled to make
     * a fresh one per opener, so an unfamiliar base is recorded rather than
     * refused: it only has to be a base, in this arena, and there has to be
     * room to remember it. It is registered as a GUEST base so a call through
     * it is run as the guest code it is, never mistaken for a native vector. */
    if (st->d[0] != g->base && !plausible_libbase(r, st->d[0])) {
        if (g->state == GL_OPENING) {
            j5d_unregister_libbase(g->base); g->base = 0; g->state = GL_FAILED;
            r->active_loader = g->parent;
        }
        snprintf(e, el, "%s Open returned invalid base %08x", g->name, st->d[0]);
        st->d[0] = 0;
        guestlib_restore_preserved(g, st);
        return 1;
    }
    if (guestlib_add_open(g, st->d[0]) < 0) {
        snprintf(e, el, "%s has more than %d opens live at once",
                 g->name, GUESTLIB_OPENS_MAX);
        st->d[0] = 0;
        guestlib_restore_preserved(g, st);
        return 1;
    }
    if (st->d[0] != g->base)
        j5d_register_guest_libbase(st->d[0]);
    if (g->state == GL_OPENING) {
        if (!g->close_trampoline)
            g->close_trampoline = make_close_trampoline(r, idx);
        if (!g->close_trampoline) {
            j5d_unregister_libbase(g->base); g->base = 0; g->state = GL_FAILED;
            r->active_loader = g->parent; st->d[0] = 0;
            snprintf(e, el, "no guest memory for Close continuation");
            return 1;
        }
        g->state = GL_READY;
        r->active_loader = g->parent;
    }
    guestlib_restore_preserved(g, st);
    return 0;
}

static int guestlib_close_done(struct emu68k_run *r, struct j5d_m68k_state *st,
                               char *e, unsigned el)
{
    int idx = (int)st->d[1];
    (void)e; (void)el;
    if (idx < 0 || idx >= GUESTLIB_MAX) return 1;
    struct guestlib_live *g = &r->guestlib[idx];
    uint32_t closed = g->closing_base;
    g->closing_base = 0;
    guestlib_drop_open(g, closed);
    /* A clone the library has just freed must stop being an address the engine
     * knows; the library's own base outlives every open and goes with expunge. */
    if (closed && closed != g->base && !guestlib_owns_base(g, closed))
        j5d_unregister_libbase(closed);
    if (st->d[0]) {                                 /* Close/Expunge returned seglist */
        for (int i = 0; i < g->open_count; i++)
            j5d_unregister_libbase(g->open_base[i]);
        g->open_count = 0;
        j5d_unregister_libbase(g->base);
        g->base = 0; g->state = GL_UNLOADED; g->reclaim_pending = 1;
    }
    st->d[0] = 0;                                   /* exec CloseLibrary is void */
    return 0;
}

static int guestlib_reclaim(struct emu68k_run *r, struct j5d_m68k_state *st,
                            char *e, unsigned el)
{
    int idx = (int)st->d[1];
    uint32_t start, end, close, close_end;
    (void)e; (void)el;
    if (idx < 0 || idx >= GUESTLIB_MAX) return 1;
    struct guestlib_live *g = &r->guestlib[idx];

    if (g->reclaim_pending) {
        start = g->mem_start; end = g->mem_end;
        close = g->close_trampoline; close_end = close ? close + 32u : 0;

        /* No translated block or backpatched chain may retain an address in
         * the image before those bytes can be reused. Expunge is cold-path;
         * dropping the per-run cache is deliberately stronger and safer than
         * trying to find only direct entries while missing incoming chains. */
        j5d_run_free();
        if (end > start)
            memset(j4_sandbox_host(&r->sb, start), 0, end - start);
        if (close)
            memset(j4_sandbox_host(&r->sb, close), 0, close_end - close);

        /* The loader is a bump allocator. Reuse the complete allocation when
         * no dependency or Exec allocation was interleaved; otherwise the
         * cleared ranges remain harmless holes until run teardown. */
        if (close == end && close_end == r->exec_heap &&
            r->sb.next_alloc == r->exec_heap) {
            r->exec_heap = r->sb.next_alloc = start;
        }
        g->reclaim_pending = 0;
    }

    st->d[0] = 0;
    guestlib_restore_preserved(g, st);
    st->pc = OSCODE_RETURN;
    return J5D_LVO_REDIRECT;
}

/* [T3] exec.library, served here: this is the bootstrap every AmigaOS program
 * performs before it can do anything else. OpenLibrary hands back a guest base
 * that the engine then recognises, so calls through it arrive at the bridge
 * with A6 naming the library. */
static int exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case LVO_GL_INIT_DONE:
        return guestlib_init_done(r, st, e, el);
    case LVO_GL_OPEN_DONE:
        return guestlib_open_done(r, st, e, el);
    case LVO_GL_CLOSE_DONE:
        return guestlib_close_done(r, st, e, el);
    case LVO_GL_RECLAIM:
        return guestlib_reclaim(r, st, e, el);

    case LVO_RAWDOFMT:
        /* Not served here: RawDoFmt calls the PROGRAM's PutChProc once per
         * character, so doing it natively would mean re-entering the JIT from
         * inside a native call, and its argument block cannot be converted
         * without parsing the format string first (a %s argument is a guest
         * pointer, a %d argument is not). Redirect into 68k code instead and
         * all of it stays inside the guest address space. */
        st->pc = OSCODE_RAWDOFMT;
        return J5D_LVO_REDIRECT;

    case LVO_OLDOPENLIB:      /* same thing, older entry point: A1 = name     */
    case LVO_OPENLIBRARY: {
        const char *nm = guest_cstr(sb, st->a[1]);      /* A1 = name, D0 = ver  */
        uint32_t requested = (lvo == LVO_OLDOPENLIB) ? 0u : st->d[0];
        int gi;
        if (!nm) {
            snprintf(e, el, "OpenLibrary: name pointer A1=%08x is outside the "
                     "guest arena %08x..%08x", st->a[1], sb->sandbox_origin,
                     sb->sandbox_origin + sb->size);
            return 1;
        }
        if (getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] OpenLibrary(\"%s\", %u)\n", nm, requested);
        for (int i = 0; i < r->nlib; i++)                 /* already open?       */
            if (!strcmp(r->openlib[i].name, nm)) { st->d[0] = r->openlib[i].base; return 0; }

        gi = find_guestlib_name(r, nm);
        if (gi >= 0) {
            struct guestlib_live *g = &r->guestlib[gi];
            if (g->state == GL_READY && g->resident.version >= requested) {
                guestlib_save_preserved(g, st);
                st->pc = g->open_trampoline;
                return J5D_LVO_REDIRECT;
            }
            /* LOADING/OPENING is an explicit dependency cycle. FAILED and an
             * unsatisfied version are ordinary OpenLibrary failure too. */
            st->d[0] = 0;
            return 0;
        }

        {
            /* Exactly the libraries the bridge generated crossings for. Kept
             * in step by generating it: offering a name with nothing behind it
             * turns the program's first call into a capability gap, and
             * withholding one we did generate sends it looking on disk for a
             * 68k library that is not there. */
#define EMU_SERVABLE_ROW(name) name,
            static const char *const servable[] = {
                EMU68K_SERVABLE_LIBS(EMU_SERVABLE_ROW)
            };
#undef EMU_SERVABLE_ROW
            /* Match on the FILE NAME, because "LIBS:diskfont.library" is an
             * ordinary way to ask for the system library and an exact compare
             * sends it to the guest loader to look for a 68k file that is not
             * there. The guest search above has already had its turn, so a
             * program that ships its own library still gets that one. */
            const char *leaf = nm;
            const char *sep;
            for (sep = nm; *sep; sep++)
                if (*sep == '/' || *sep == ':') leaf = sep + 1;
            unsigned k; int known = 0;
            for (k = 0; k < sizeof servable / sizeof servable[0]; k++)
                if (!strcmp(leaf, servable[k])) { known = 1; break; }
            if (known && g_oscall) {
                uint32_t base;
                if (r->nlib >= LIBBASE_MAX) {
                    snprintf(e, el, "too many native library facades"); return 1;
                }
                base = LIBBASE_FIRST + (uint32_t)r->nlib * LIBBASE_STRIDE;
                snprintf(r->openlib[r->nlib].name, sizeof r->openlib[r->nlib].name,
                         "%s", leaf);
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
        }

        {   /* Native-name-wins above; an unknown name is now a disk library. */
            char why[256] = {0};
            if (load_guestlib(r, nm, requested, &gi, why, sizeof why)) {
                if (getenv("EMU68K_TRACE_CALLS"))
                    fprintf(stderr, "[68k] OpenLibrary guest %s failed: %s\n", nm, why);
                st->d[0] = 0;
                return 0;
            }
            struct guestlib_live *g = &r->guestlib[gi];
            guestlib_save_preserved(g, st);
            st->d[0] = (g->resident.flags & GL68_RTF_AUTOINIT) ? g->init.base : 0;
            st->a[0] = g->init.seglist;
            st->a[4] = 0;
            st->a[6] = EXEC_BASE;
            st->pc = g->init_trampoline;
            return J5D_LVO_REDIRECT;
        }
    }
    case LVO_CLOSELIBRARY: {
        int gi = find_guestlib_base(r, st->a[1]);       /* A1 = library base */
        if (gi >= 0) {
            guestlib_save_preserved(&r->guestlib[gi], st);
            /* Close runs on the base the caller was given, not on the
             * library's own: that is the only way a per-opener base can free
             * the right instance. */
            r->guestlib[gi].closing_base = st->a[1];
            st->a[6] = st->a[1];
            st->pc = r->guestlib[gi].close_trampoline;
            return J5D_LVO_REDIRECT;
        }
        st->d[0] = 0;
        return 0;                                        /* native facade stays */
    }
    case LVO_AVAILMEM:
        st->d[0] = (r->exec_heap_end > r->exec_heap)
                 ? (r->exec_heap_end - r->exec_heap) : 0;
        return 0;
    case LVO_FREEVEC:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;
    case LVO_CREATEPOOL: {
        /* PoolHeader is opaque to callers. Give it stable guest identity so
         * accidental field reads remain in-range, while the allocations it
         * produces come from the same guest heap as AllocMem/AllocVec. */
        for (int i = 0; i < GUESTPOOL_MAX; i++) {
            if (r->guestpool[i].live) continue;
            uint32_t token = guest_alloc(r, 32u);
            if (!token) { st->d[0] = 0; return 0; }
            r->guestpool[i].live = 1;
            r->guestpool[i].guest = token;
            r->guestpool[i].requirements = st->d[0];
            st->d[0] = token;
            return 0;
        }
        st->d[0] = 0;
        return 0;
    }
    case LVO_DELETEPOOL:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                memset(&r->guestpool[i], 0, sizeof r->guestpool[i]);
                st->d[0] = 0;
                return 0;
            }
        snprintf(e, el, "DeletePool received unknown guest pool %08x", st->a[0]);
        return 1;
    case LVO_ALLOCPOOLED:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                st->d[0] = guest_alloc(r, st->d[0]);
                return 0;
            }
        snprintf(e, el, "AllocPooled received unknown guest pool %08x", st->a[0]);
        return 1;
    case LVO_FREEPOOLED:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                st->d[0] = 0;                           /* bump heap: no free */
                return 0;
            }
        snprintf(e, el, "FreePooled received unknown guest pool %08x", st->a[0]);
        return 1;
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
        if (r->sb.next_alloc < r->exec_heap) r->sb.next_alloc = r->exec_heap;
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
    /* RemHead/RemTail hand back a NODE, and the node is the guest's own: a
     * native pointer would be an address it cannot dereference, so like the
     * Add* pair these walk guest memory directly. An empty list is detected the
     * way exec does it, by the terminator's NULL link rather than by a count. */
    /* Both operands are GUEST addresses and the payload is plain bytes, so this
     * is a move inside the arena. Bridging it would hand dos.library two guest
     * pointers it cannot dereference. Overlap is allowed: CopyMemQuick promises
     * long-aligned non-overlapping, but a program that gets that wrong should
     * misbehave the way it does on a real Amiga, not corrupt the host. */
    case LVO_COPYMEM:
    case LVO_COPYMEMQUICK: {
        uint32_t src = st->a[0], dst = st->a[1], n = st->d[0];
        if (!n) return 0;
        if (src < sb->sandbox_origin || dst < sb->sandbox_origin ||
            (uint64_t)src + n > (uint64_t)sb->sandbox_origin + sb->size ||
            (uint64_t)dst + n > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "CopyMem %08x -> %08x (%u bytes) leaves the guest arena",
                     src, dst, n);
            return 1;
        }
        memmove(j4_sandbox_host(sb, dst), j4_sandbox_host(sb, src), n);
        return 0;
    }
    case LVO_SUPERVISOR:
        /* Supervisor(A5) runs the CALLER'S OWN routine with the S bit set. It
         * is not a request for anything the host has to provide: the code is
         * the guest's, in the guest's memory, and the only thing supervisor
         * mode buys it is the right to touch the privileged registers. So it
         * runs, in the guest, exactly where it is - and if it then reaches for
         * something this machine does not have, that is caught there, by name,
         * instead of the whole call being refused for what it might do.
         *
         * The routine returns with rte, so the dispatcher builds the frame. */
        if (!st->a[5]) {                             /* nothing to run          */
            st->d[0] = 0;
            return 0;
        }
        if (getenv("EMU68K_TRACE_CALLS")) {
            uint32_t target = st->a[5];
            fprintf(stderr, "[68k] Supervisor target=%08x", target);
            if (target >= sb->sandbox_origin &&
                (uint64_t)target + 16u <=
                    (uint64_t)sb->sandbox_origin + sb->size) {
                const uint8_t *p = j4_sandbox_host(sb, target);
                fputs(" bytes=", stderr);
                for (unsigned i = 0; i < 16; i++)
                    fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
            } else {
                fputs(" (outside guest sandbox)", stderr);
            }
            fputc('\n', stderr);
            if (st->pc >= sb->sandbox_origin + 16u &&
                (uint64_t)st->pc + 16u <=
                    (uint64_t)sb->sandbox_origin + sb->size) {
                const uint8_t *p = j4_sandbox_host(sb, st->pc - 16u);
                fprintf(stderr, "[68k] Supervisor caller=%08x bytes[-16..+15]=",
                        st->pc);
                for (unsigned i = 0; i < 32; i++)
                    fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
                fputc('\n', stderr);
            }
        }
        st->pc = st->a[5];
        return J5D_LVO_REDIRECT_RTE;
    case LVO_CACHECLEARU:
    case LVO_CACHECLEARE:
        /* A 68k program clears the caches when it has just written bytes that
         * are about to be executed. Under translation that is the notification
         * that a translated block may no longer match the code it came from,
         * which no amount of host cache maintenance would fix, so the whole
         * per-run translation cache goes. CacheClearE names a range; dropping
         * everything is a superset and a range-precise version would only be
         * faster, never more correct.
         *
         * The return cannot go back into a block that was just freed, so this
         * takes the same exit as the guest-library reclaim: the permanent RTS,
         * reached by redirect, which pops the caller's return address and
         * carries on through freshly translated code. */
        j5d_run_free();
        st->pc = OSCODE_RETURN;
        return J5D_LVO_REDIRECT;
    case LVO_REMHEAD: {
        uint32_t list = st->a[0];
        uint32_t node = gread32(sb, list);           /* lh_Head                */
        uint32_t succ = gread32(sb, node);           /* node->ln_Succ          */
        if (!succ) { st->d[0] = 0; return 0; }       /* the list was empty     */
        gwrite32(sb, list, succ);                    /* lh_Head = succ         */
        gwrite32(sb, succ + 4, list);                /* succ->ln_Pred = &lh_Head */
        st->d[0] = node;
        return 0;
    }
    case LVO_REMTAIL: {
        uint32_t list = st->a[0];
        uint32_t node = gread32(sb, list + 8);       /* lh_TailPred            */
        uint32_t pred = gread32(sb, node + 4);       /* node->ln_Pred          */
        if (!pred) { st->d[0] = 0; return 0; }
        gwrite32(sb, list + 8, pred);                /* lh_TailPred = pred     */
        gwrite32(sb, pred, list + 4);                /* pred->ln_Succ = &lh_Tail */
        st->d[0] = node;
        return 0;
    }
    case LVO_ENQUEUE: {
        /* Priority-sorted insert: walk to the first node of LOWER priority and
         * insert before it. ln_Pri is a SIGNED byte at offset 9. */
        uint32_t list = st->a[0], node = st->a[1];
        int pri = (int8_t)j4_sandbox_host(sb, node)[9];
        uint32_t next = gread32(sb, list);           /* lh_Head                */
        uint32_t succ;
        while ((succ = gread32(sb, next)) != 0) {    /* not yet the terminator */
            if ((int8_t)j4_sandbox_host(sb, next)[9] < pri) break;
            next = succ;
        }
        {   uint32_t pred = gread32(sb, next + 4);
            gwrite32(sb, node, next);                /* node->ln_Succ = next   */
            gwrite32(sb, node + 4, pred);            /* node->ln_Pred = pred   */
            gwrite32(sb, pred, node);
            gwrite32(sb, next + 4, node);
        }
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
    case LVO_ADDPORT: case LVO_REMPORT:  /* no public port list here          */
        return 0;
    case LVO_FINDPORT:       /* FindPort(name A1): a guest has no public ports,
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
    /* A signal BIT is bookkeeping: a program reserves one for a port it is
     * about to create, long before anything is ever sent to it. Refusing the
     * reservation stopped programs during startup, at their ARexx port, over a
     * bit nobody had signalled yet. What a guest still cannot do is WAIT on
     * one, and that stays a named gap rather than a hang. */
    /* ---- MESSAGE PORTS ------------------------------------------------------
     *
     * Every structure involved - the MsgPort, the Message, the list linking
     * them - is the GUEST's own memory, so these are guest-memory list
     * operations exactly like the Add/Rem pair above, plus a signal. Handing
     * them to the native exec would give it guest addresses it cannot
     * dereference, and would put the program's messages on a list it cannot
     * see.
     *
     * A port's list is initialised by the program with NewList, so these read
     * the same lh_Head/lh_Tail/lh_TailPred layout exec does, and an empty list
     * is detected the way exec detects it: by the terminator's NULL link. */
    case LVO_PUTMSG: {
        uint32_t port = st->a[0], msg = st->a[1];
        uint32_t list = port + MP_MSGLIST;
        uint32_t tailpred = gread32(sb, list + M68K_List_lh_TailPred);
        gwrite32(sb, msg, list + M68K_List_lh_Tail);
        gwrite32(sb, msg + 4, tailpred);
        gwrite32(sb, tailpred, msg);
        gwrite32(sb, list + M68K_List_lh_TailPred, msg);
        /* and tell the port's task, which is what makes a WaitPort return */
        {
            uint32_t task = gread32(sb, port + MP_SIGTASK);
            uint32_t bit  = gread8(sb, port + MP_SIGBIT);
            if (task && bit < 32)
                gwrite32(sb, task + TASK_SIGRECVD_OFF,
                         gread32(sb, task + TASK_SIGRECVD_OFF) | (1u << bit));
        }
        return 0;
    }
    case LVO_GETMSG: {
        uint32_t port = st->a[0];
        uint32_t list = port + MP_MSGLIST;
        uint32_t head = gread32(sb, list + M68K_List_lh_Head);
        uint32_t succ = head ? gread32(sb, head) : 0;
        if (!head || !succ) { st->d[0] = 0; return 0; }   /* empty */
        gwrite32(sb, list + M68K_List_lh_Head, succ);
        gwrite32(sb, succ + 4, list);
        st->d[0] = head;
        return 0;
    }
    case LVO_REPLYMSG: {
        uint32_t msg = st->a[1];
        uint32_t reply = gread32(sb, msg + MN_REPLYPORT);
        if (!reply) return 0;               /* a message with nowhere to go back */
        st->a[0] = reply;
        st->a[1] = msg;
        return exec_call(r, sb, LVO_PUTMSG, st, e, el);
    }
    case LVO_WAIT: {
        /* Wait(signalSet D0) -> the signals that arrived.
         *
         * A signal a program sent to ITSELF is already there, which is the
         * common shape for "wake me when I have queued my own work" and needs
         * nothing else to run. Anything else needs a second 68k context to do
         * the signalling, and until one exists a wait that cannot be satisfied
         * would be an unbreakable hang inside a translated program. Naming it
         * is the honest answer: a hang tells the reader nothing, and the run
         * would have to be killed from outside to find out why. */
        uint32_t want = st->d[0];
        uint32_t got = gread32(sb, GUEST_PROCESS + TASK_SIGRECVD_OFF) & want;
        if (got) {
            gwrite32(sb, GUEST_PROCESS + TASK_SIGRECVD_OFF,
                     gread32(sb, GUEST_PROCESS + TASK_SIGRECVD_OFF) & ~got);
            st->d[0] = got;
            return 0;
        }
        ledger_record(lvo, r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: Wait($%08lx) cannot be satisfied - "
                 "nothing else runs in this program yet to send it",
                 (unsigned long)want);
        return 1;
    }
    case LVO_SIGNAL: {
        uint32_t task = st->a[1], sigs = st->d[0];
        if (task)
            gwrite32(sb, task + TASK_SIGRECVD_OFF,
                     gread32(sb, task + TASK_SIGRECVD_OFF) | sigs);
        return 0;
    }
    case LVO_ALLOCSIGNAL: {
        uint32_t alloc = gread32(sb, GUEST_PROCESS + TASK_SIGALLOC_OFF);
        int want = (int32_t)st->d[0];
        int bit = -1;
        if (want >= 0 && want < 32) {
            if (!(alloc & (1u << want))) bit = want;
        } else {
            for (bit = 31; bit >= 16; bit--)
                if (!(alloc & (1u << bit))) break;
            if (bit < 16) bit = -1;
        }
        if (bit >= 0)
            gwrite32(sb, GUEST_PROCESS + TASK_SIGALLOC_OFF,
                     alloc | (1u << bit));
        st->d[0] = (uint32_t)(int32_t)bit;
        return 0;
    }
    case LVO_FREESIGNAL: {
        int bit = (int32_t)st->d[0];
        if (bit >= 0 && bit < 32) {
            uint32_t alloc = gread32(sb, GUEST_PROCESS + TASK_SIGALLOC_OFF);
            gwrite32(sb, GUEST_PROCESS + TASK_SIGALLOC_OFF,
                     alloc & ~(1u << bit));
        }
        return 0;
    }
    case LVO_STACKSWAP: {
        /* struct StackSwapStruct is three guest pointers: lower, upper and
         * switch-point SP. Swap those values here; a second call restores the
         * original stack, just like exec.library on a real 68k system. */
        uint32_t sss = st->a[0];
        uint32_t new_lower, new_upper, new_sp, old_sp = st->a[7];
        if (!sss || sss < sb->sandbox_origin ||
            (uint64_t)sss + 12u > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "StackSwap structure A0=%08x is outside guest memory", sss);
            return 1;
        }
        new_lower = gread32(sb, sss);
        new_upper = gread32(sb, sss + 4u);
        new_sp = gread32(sb, sss + 8u);
        if (new_lower < sb->sandbox_origin || new_upper <= new_lower ||
            new_sp < new_lower || new_sp > new_upper ||
            (uint64_t)new_upper > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "StackSwap requested invalid range %08x..%08x sp=%08x",
                     new_lower, new_upper, new_sp);
            return 1;
        }
        gwrite32(sb, sss, r->stack_lower);
        gwrite32(sb, sss + 4u, r->stack_upper);
        gwrite32(sb, sss + 8u, old_sp);
        r->stack_lower = new_lower;
        r->stack_upper = new_upper;
        st->a[7] = new_sp;
        gwrite32(sb, GUEST_PROCESS + TASK_SPREG_OFF, new_sp);
        gwrite32(sb, GUEST_PROCESS + TASK_SPLOWER_OFF, new_lower);
        gwrite32(sb, GUEST_PROCESS + TASK_SPUPPER_OFF, new_upper);
        return 0;
    }
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
    fprintf(stderr, "[68k] %s LVO %d (%d) pc=%08x  d0=%08x d1=%08x "
            "a0=%08x a1=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x\n",
            lib, lvo, -6 * lvo, st->pc,
            st->d[0], st->d[1], st->a[0], st->a[1], st->a[3],
            st->a[4], st->a[5], st->a[6], st->a[7]);
    (void)r;
}

/* utility.library tag walkers operate on structures the guest owns and may
 * return pointers into those structures. Running these over native pointers
 * would be an ABI error even in the installed AROS bridge, so keep the compact
 * pointer-sensitive core in guest memory. */
static uint32_t utility_next_tag(j4_sandbox *sb, uint32_t statep)
{
    uint32_t p;
    unsigned guard = 0;
    if (!statep || statep < sb->sandbox_origin ||
        (uint64_t)statep + 4u > (uint64_t)sb->sandbox_origin + sb->size)
        return 0;
    p = gread32(sb, statep);
    while (p && ++guard < 65536u) {
        uint32_t tag, data;
        if (p < sb->sandbox_origin ||
            (uint64_t)p + 8u > (uint64_t)sb->sandbox_origin + sb->size) {
            gwrite32(sb, statep, 0); return 0;
        }
        tag = gread32(sb, p); data = gread32(sb, p + 4u);
        switch (tag) {
        case 0:                                           /* TAG_DONE */
            gwrite32(sb, statep, 0); return 0;
        case 1:                                           /* TAG_IGNORE */
            p += 8u; break;
        case 2:                                           /* TAG_MORE */
            p = data; break;
        case 3:                                           /* TAG_SKIP */
            if (data > 0x1fffffffu) { gwrite32(sb, statep, 0); return 0; }
            p += 8u * (data + 1u); break;
        default:
            gwrite32(sb, statep, p + 8u); return p;
        }
    }
    gwrite32(sb, statep, 0);
    return 0;
}

static uint32_t utility_find_tag(j4_sandbox *sb, uint32_t wanted, uint32_t list)
{
    unsigned guard = 0;
    /* Same TAG control semantics as NextTagItem, without allocating a guest
     * state cell merely to walk a caller-owned list. */
    while (list && ++guard < 65536u) {
        uint32_t tag, data;
        if (list < sb->sandbox_origin ||
            (uint64_t)list + 8u > (uint64_t)sb->sandbox_origin + sb->size) return 0;
        tag = gread32(sb, list); data = gread32(sb, list + 4u);
        if (tag == 0) return 0;
        if (tag == 1) { list += 8u; continue; }
        if (tag == 2) { list = data; continue; }
        if (tag == 3) {
            if (data > 0x1fffffffu) return 0;
            list += 8u * (data + 1u); continue;
        }
        if (tag == wanted) return list;
        list += 8u;
    }
    return 0;
}

static int utility_guest_call(j4_sandbox *sb, int lvo,
                              struct j5d_m68k_state *st)
{
    switch (lvo) {
    case 5:                                             /* FindTagItem */
        st->d[0] = utility_find_tag(sb, st->d[0], st->a[0]); return 0;
    case 6: {                                           /* GetTagData */
        uint32_t tag = utility_find_tag(sb, st->d[0], st->a[0]);
        st->d[0] = tag ? gread32(sb, tag + 4u) : st->d[1]; return 0;
    }
    case 7: {                                           /* PackBoolTags */
        uint32_t flags = st->d[0], list = st->a[0];
        unsigned guard = 0;
        while (list && ++guard < 65536u) {
            uint32_t item, tag, data;
            if (list < sb->sandbox_origin ||
                (uint64_t)list + 8u > (uint64_t)sb->sandbox_origin + sb->size) break;
            tag = gread32(sb, list); data = gread32(sb, list + 4u);
            if (tag == 0) break;
            if (tag <= 3u) { /* Advance control tags with a temporary guest-free walk. */
                if (tag == 1) list += 8u;
                else if (tag == 2) list = data;
                else if (tag == 3) list += 8u * (data + 1u);
                continue;
            }
            item = utility_find_tag(sb, tag, st->a[1]);
            if (item) {
                uint32_t mask = gread32(sb, item + 4u);
                flags = data ? flags | mask : flags & ~mask;
            }
            list += 8u;
        }
        st->d[0] = flags; return 0;
    }
    case 8:                                             /* NextTagItem */
        st->d[0] = utility_next_tag(sb, st->a[0]); return 0;
    case 27: case 28: {                                 /* Stricmp / Strnicmp */
        const char *a = guest_cstr(sb, st->a[0]);
        const char *b = guest_cstr(sb, st->a[1]);
        uint32_t n = lvo == 28 ? st->d[0] : 0xffffffffu;
        int diff = 0;
        if (!a || !b) { st->d[0] = a ? 1u : b ? 0xffffffffu : 0u; return 0; }
        while (n--) {
            unsigned ca = (unsigned char)*a++, cb = (unsigned char)*b++;
            diff = toupper(ca) - toupper(cb);
            if (diff || !ca || !cb) break;
        }
        st->d[0] = (uint32_t)diff; return 0;
    }
    case 29: st->d[0] = (uint32_t)toupper((unsigned char)st->d[0]); return 0;
    case 30: st->d[0] = (uint32_t)tolower((unsigned char)st->d[0]); return 0;
    default: return 1;
    }
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
        {   /* a redirect is neither "served" nor "failed": the guest is about
             * to run 68k code for this vector, so pass it straight through. */
            int rc = exec_call(r, c->sb, lvo, st, e, el);
            if (rc == 0 || rc == J5D_LVO_REDIRECT ||
                rc == J5D_LVO_REDIRECT_RTE) return rc;
        }
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
        {
            /* Name what the program ASKED for where the vector says. An LVO
             * number tells the reader to go and look it up; "timer.device" or
             * "console.device" tells them what the program wanted and whether
             * it matters. OpenLibrary already does this. */
            const char *what = NULL;
            if (lvo == LVO_OPENDEVICE) what = guest_cstr(c->sb, st->a[0]);
            if (what && *what)
                snprintf(e, el, "capability gap: exec.library OpenDevice(\"%s\") "
                                "is not available yet", what);
            else
                snprintf(e, el, "capability gap: exec.library function LVO %d "
                                "(offset %d) is not available yet",
                         lvo, -6 * lvo);
        }
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
                if (lvo == 25) return dos_guest_loadseg(r, c->sb, st, e, el);
                if (lvo == 26) return dos_guest_unloadseg(r, st);
                if (lvo == 133) return rda_readargs(r, c->sb, st, e, el);
                /* FreeArgs is LVO 143 (-858). 134 is FindArg, and having it
                 * here meant FindArg was answered as if it were FreeArgs while
                 * FreeArgs itself fell through as a capability gap. Nothing to
                 * free on this side: the RDArgs and its results live in the
                 * guest, allocated by rda_readargs above. */
                if (lvo == 143) { st->d[0] = 0; return 0; }   /* FreeArgs        */
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
            if (!strcmp(r->openlib[i].name, "utility.library") &&
                utility_guest_call(c->sb, lvo, st) == 0)
                return 0;
            if (el) e[0] = 0;
            if (g_oscall &&
                g_oscall(r->openlib[i].name, lvo, st, r->reserve, g_oscall_user,
                         e, el) == 0)
                return 0;
            /* OpenCatalogA is deliberately allowed to fail: locale clients
             * are required to keep their built-in strings and use those when
             * no catalog can be opened.  The native bridge cannot safely hand
             * a 64-bit Catalog pointer or native TagItem list to this guest,
             * so until the generated opaque-handle/tag shadow exists, NULL is
             * the honest compatibility result.  Keep this AFTER g_oscall so an
             * embedder with a complete catalog crossing still wins. */
            if (!strcmp(r->openlib[i].name, "locale.library") && lvo == 25) {
                if (el) e[0] = 0;
                st->d[0] = 0;
                return 0;
            }
            ledger_record(lvo, r->name[0] ? r->name : NULL);
            /* The type compiler reports the precise missing policy (including
             * tag and domain). Preserve that instead of replacing it with the
             * generic LVO-only message below. */
            if (el && e[0]) return 1;
            snprintf(e, el, "capability gap: %s function LVO %d (offset %d) "
                            "is not available yet", r->openlib[i].name, lvo, -6 * lvo);
            return 1;
        }
    }

    /* the built-in stub OS (the corpus path: PutChar/AllocMem/FreeMem) */
    int rc = stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
    if (rc) {
        ledger_record(lvo, r && r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: library function LVO %d (offset %d) on "
                        "an unrecognised base %08x is not marshalled yet",
                 lvo, -6 * lvo, st->a[6]);
    }
    return rc;
}

static j5d_poll_action quantum_poll(void *user);

int emu68k_run_call_hook(emu68k_run *r, unsigned long entry,
                         unsigned long hook, unsigned long object,
                         unsigned long message, unsigned int *result,
                         char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    j5d_sandbox sb;
    struct bctx c;
    uint32_t d0 = 0;
    uint32_t stack;

    if (!r || entry > UINT32_MAX || hook > UINT32_MAX ||
        object > UINT32_MAX || message > UINT32_MAX)
    {
        if (err && errlen) snprintf(err, errlen, "invalid 68k Hook callback context");
        return 1;
    }
    if (!r->callback_stack_top)
    {
        stack = guest_alloc(r, 16384);
        if (!stack)
        {
            if (err && errlen) snprintf(err, errlen, "guest memory exhausted for Hook stack");
            return 1;
        }
        r->callback_stack_top = (stack + 16384u) & ~15u;
    }
    memset(&st, 0, sizeof st);
    st.a[0] = (uint32_t)hook;
    st.a[1] = (uint32_t)message;
    st.a[2] = (uint32_t)object;
    st.a[7] = r->callback_stack_top;
    sb.host_mem = r->sb.host_mem;
    sb.origin = r->sb.sandbox_origin;
    sb.size = r->sb.size;
    c.lib = &r->lib;
    c.sb = &r->sb;
    c.run = r;
    j5d_engine_activate(r->eng);
    /* A nested callback must complete as part of the native call; yielding it
     * would strand the native stack. Restore the outer quantum immediately. */
    j5d_set_poll(NULL, NULL, 0);
    int rc = j5d_run(&sb, (uint32_t)entry, RUN_LIBBASE, &st, &d0,
                     bridge, &c, err, errlen);
    j5d_set_poll(quantum_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    if (rc != 0)
        return 1;
    if (result) *result = d0;
    return 0;
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
    /* [T3] Plant the in-guest OS routines before the program loads. These are
     * the vectors the bridge redirects to rather than serving natively, because
     * they call back into the program's own code. */
    if (sizeof emu68k_rawdofmt_bin > OSCODE_RETURN - OSCODE_BASE) {
        snprintf(err, errlen, "in-guest OS code does not fit its region");
        goto fail;
    }
    memcpy(j4_sandbox_host(&r->sb, OSCODE_RAWDOFMT), emu68k_rawdofmt_bin,
           sizeof emu68k_rawdofmt_bin);
    put_be16(&r->sb, OSCODE_RETURN, 0x4e75u);

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
        {
            uint8_t *cli = j4_sandbox_host(&r->sb, GUEST_CLI);
            uint8_t *cmd = j4_sandbox_host(&r->sb, GUEST_COMMAND);
            uint32_t cmd_b = GUEST_MKBADDR(GUEST_COMMAND);
            static const char command[] = "program";
            memset(cli, 0, 128);
            cli[CLI_COMMAND_OFF + 0] = (uint8_t)(cmd_b >> 24);
            cli[CLI_COMMAND_OFF + 1] = (uint8_t)(cmd_b >> 16);
            cli[CLI_COMMAND_OFF + 2] = (uint8_t)(cmd_b >> 8);
            cli[CLI_COMMAND_OFF + 3] = (uint8_t)cmd_b;
            cmd[0] = (uint8_t)(sizeof command - 1u); /* Amiga BSTR length byte */
            memcpy(cmd + 1, command, sizeof command - 1u);
        }
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
    r->sb.next_alloc = r->exec_heap;       /* one cursor for AllocMem + LoadSeg */
    r->stack_lower = r->exec_heap_end;
    r->stack_upper = GUEST_TOP;
    gwrite32(&r->sb, GUEST_PROCESS + TASK_SPREG_OFF, GUEST_TOP);
    gwrite32(&r->sb, GUEST_PROCESS + TASK_SPLOWER_OFF, r->stack_lower);
    gwrite32(&r->sb, GUEST_PROCESS + TASK_SPUPPER_OFF, r->stack_upper);
    r->active_loader = -1;
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
        r->poll_quantum = q;
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

    int rc = j5d_run(&j5sb, r->resume_pc, RUN_LIBBASE, &r->st, &d0,
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

void *emu68k_run_guest0(emu68k_run *r) { return r ? r->reserve : NULL; }

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
