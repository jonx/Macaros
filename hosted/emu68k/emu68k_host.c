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
#include "bridge_lab.h"
#include "emu68k_guest_offsets.h"
#include "emu68k_internal.h"
#include "j5n_symbols.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include "nativelib/rawdofmt_blob.h"

const char *emu68k_host_getenv(const char *name)
{
    typedef char *(*native_getenv_fn)(const char *);
    static native_getenv_fn native_getenv;
    static int resolved;

    /* AROSBootstrap exports its guest C library globally.  A plain getenv()
     * reference in a subsequently dlopened host shim can therefore resolve to
     * that library, which only knows the guest process ENV: list.  Resolve
     * getenv from libSystem by its concrete image, not the global symbol
     * scope; runtime observability controls must not silently vanish. */
    if (!resolved) {
        void *libsystem = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
        native_getenv = libsystem ?
            (native_getenv_fn)dlsym(libsystem, "getenv") : NULL;
        resolved = 1;
    }
    return native_getenv ? native_getenv(name) : NULL;
}

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
#define RUN_LIBBASE     (emu68k_oscall ? EXEC_BASE : LIBBASE)
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
#define GUEST_ETASK     (GUEST_PROCESS + 0x00000100u)
#define GUEST_CLI       0x00211000u
#define GUEST_COMMAND   (GUEST_CLI + 64u)
#define PR_TASK_LN_TYPE 8            /* tc_Node.ln_Type: NT_PROCESS = 13       */
#define PR_CLI_OFFSET   CLASSIC_PR_CLI
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
#define MP_SIGTASK   M68K_MsgPort_mp_SigTask
#define MP_SIGBIT    M68K_MsgPort_mp_SigBit
#define MP_MSGLIST   M68K_MsgPort_mp_MsgList_lh_Head
#define MN_REPLYPORT M68K_Message_mn_ReplyPort
#define TASK_SIGRECVD_OFF M68K_Task_tc_SigRecvd
#define EXECBASE_THISTASK M68K_ExecBase_ThisTask
/* struct Library: ln(14) lib_Flags(14) lib_pad(15) lib_NegSize(16)
 * lib_PosSize(18) lib_Version(20) lib_Revision(22). Programs written for
 * AmigaOS 2.0 and later routinely check lib_Version before doing anything and
 * quit silently if it is too low - which a zeroed base always is. */
#define LIB_VERSION_OFF   M68K_Library_lib_Version
#define LIB_REVISION_OFF  M68K_Library_lib_Revision
#define GUEST_LIB_VERSION 39    /* what an OS 3.0 program expects to find      */
#define GUEST_LIB_REV     106
/* SysBase fronts the current native AROS Exec, not an OS 3.0 implementation.
 * AROS-built guest libraries test this before their InitLib runs; advertising
 * 39 made a current library fail initialization before it could make a single
 * bridge call. Keep ordinary facade libraries conservative above, while Exec
 * reports the ABI generation actually behind this hosted boundary. */
#define GUEST_EXEC_VERSION 51
#define GUEST_EXEC_REV     8
#define LVO_STACKSWAP    122    /* -732 */
#define TASK_SIGALLOC_OFF M68K_Task_tc_SigAlloc
#define TASK_SPREG_OFF    M68K_Task_tc_SPReg
#define TASK_SPLOWER_OFF  M68K_Task_tc_SPLower
#define TASK_SPUPPER_OFF  M68K_Task_tc_SPUpper
#define TASK_SIGEXCEPT_OFF M68K_Task_tc_SigExcept
#define TASK_TRAPALLOC_OFF M68K_Task_tc_UnionETask_tc_ETrap_tc_ETrapAlloc
#define TASK_ETASK_OFF     M68K_Task_tc_UnionETask_tc_ETrap_tc_ETrapAlloc
#define TASKF_ETASK        (1u << 3)
#define TASK_EXCEPTDATA_OFF M68K_Task_tc_ExceptData
#define TASK_EXCEPTCODE_OFF M68K_Task_tc_ExceptCode
#define TF_EXCEPT_GUEST    (1u << 5)
#define SSM_LENGTH_OFF     M68K_SemaphoreMessage_ssm_Message_mn_Length
#define SSM_SEMAPHORE_OFF  M68K_SemaphoreMessage_ssm_Semaphore
#define MH_ATTR_OFF        M68K_MemHeader_mh_Attributes
#define MH_FIRST_OFF       M68K_MemHeader_mh_First
#define MH_LOWER_OFF       M68K_MemHeader_mh_Lower
#define MH_UPPER_OFF       M68K_MemHeader_mh_Upper
#define MH_FREE_OFF        M68K_MemHeader_mh_Free
#define MC_NEXT_OFF        M68K_MemChunk_mc_Next
#define MC_BYTES_OFF       M68K_MemChunk_mc_Bytes
#define INTR_DATA_OFF      M68K_Interrupt_is_Data
#define INTR_CODE_OFF      M68K_Interrupt_is_Code

/* struct AVLNode is four guest LONGs: left, right, parent, balance. */
#define AVL_LEFT_OFF       0u
#define AVL_RIGHT_OFF      4u
#define AVL_PARENT_OFF     8u

#define TASKTAG_DUMMY       0x80100000u
#define TASKTAG_ERROR       (TASKTAG_DUMMY + 0u)
#define TASKTAG_CODETYPE    (TASKTAG_DUMMY + 1u)
#define TASKTAG_PC          (TASKTAG_DUMMY + 2u)
#define TASKTAG_FINALPC     (TASKTAG_DUMMY + 3u)
#define TASKTAG_STACKSIZE   (TASKTAG_DUMMY + 4u)
#define TASKTAG_NAME        (TASKTAG_DUMMY + 6u)
#define TASKTAG_USERDATA    (TASKTAG_DUMMY + 7u)
#define TASKTAG_PRI         (TASKTAG_DUMMY + 8u)
#define TASKTAG_POOLPUDDLE  (TASKTAG_DUMMY + 9u)
#define TASKTAG_POOLTHRESH  (TASKTAG_DUMMY + 10u)
#define TASKTAG_ARG1        (TASKTAG_DUMMY + 16u)
#define TASKTAG_ARG8        (TASKTAG_DUMMY + 23u)
#define TASKTAG_STARTUPMSG  (TASKTAG_DUMMY + 24u)
#define TASKTAG_TASKMSGPORT (TASKTAG_DUMMY + 25u)
#define TASKTAG_FLAGS       (TASKTAG_DUMMY + 26u)
#define TASKTAG_TCBEXTRASIZE (TASKTAG_DUMMY + 28u)
#define TASKTAG_AFFINITY    (TASKTAG_DUMMY + 29u)
#define TASKTAG_PRELAUNCHHOOK (TASKTAG_DUMMY + 30u)

/* Private exec vectors used only by synthesized guest loader continuations.
 * They are inside the engine's vector-recognition window but beyond Exec's
 * public table. */
#define LVO_GL_INIT_DONE  650   /* -3900 */
#define LVO_GL_OPEN_DONE  651   /* -3906 */
#define LVO_GL_CLOSE_DONE 652   /* -3912 */
#define LVO_GL_RECLAIM    653   /* -3918 */

static const char *g_crash_dir = NULL;

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
emu68k_oscall_fn emu68k_oscall = NULL;
void            *emu68k_oscall_user = NULL;
void emu68k_set_oscall(emu68k_oscall_fn fn, void *user)
{ emu68k_oscall = fn; emu68k_oscall_user = user; }

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

    if (!st || !emu68k_host_getenv("EMU68K_TRACE_FAULT"))
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

/* The EXECUTION DOMAIN, as a depth rather than a flag.
 *
 * Nonzero means native code is running on the guest's behalf. It is not a
 * boolean because a native call can re-enter the GUEST - a Hook, a BOOPSI
 * dispatcher, another 68k context - and hardware touched by that nested guest
 * code is genuinely the program's, not ours. Every nested guest entry saves the
 * depth and clears it for the duration, so the domain follows the real
 * guest -> native -> guest nesting instead of latching on at the first bridge
 * call and staying on. */
static int g_in_bridge;

static int domain_enter_guest(void)      /* -> the depth to restore */
{
    int saved = g_in_bridge;
    g_in_bridge = 0;
    return saved;
}

static void domain_leave_guest(int saved) { g_in_bridge = saved; }

static int classify_hardware(void *fault_addr, void *user)
{
    /* A crash inside a bridge call is OURS, not the program addressing the
     * machine. The address window below is generous by necessity - it has to
     * catch a guest access computed at run time - and native allocations fall
     * inside it easily, so a native null-ish dereference was being reported as
     * "this program needs a full Amiga emulator". That reads as a routing
     * verdict about the program when it is a bug in the bridge, and it hides
     * the crash bundle that would say where. */
    if (g_in_bridge) {
        if (emu68k_host_getenv("EMU68K_TRACE_FAULT"))
            fprintf(stderr, "[emu68k] fault at %p inside a bridge call: ours, "
                    "not a guest hardware access\n", fault_addr);
        return 0;
    }
    struct emu68k_run *r = user;
    unsigned long long host = (unsigned long long)(uintptr_t)fault_addr;
    unsigned long long base = (unsigned long long)(uintptr_t)r->sb.host_mem;
    unsigned long long guest;

    if (host < base - 0x10000000ull || host > base + 0x10000000ull) {
        if (emu68k_host_getenv("EMU68K_TRACE_FAULT"))
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
        if (emu68k_host_getenv("EMU68K_TRACE_FAULT"))
            fprintf(stderr, "[emu68k] unclassified fault at guest $%06llX%s "
                    "(arena $%06X..$%06llX)\n", guest, g_hw_origin,
                    r->sb.sandbox_origin,
                    (unsigned long long)r->sb.sandbox_origin + r->sb.size);
        return 0;                                  /* a genuine wild access     */
    }
    if (emu68k_host_getenv("EMU68K_TRACE_FAULT"))
        fprintf(stderr, "[emu68k] classified hardware fault: %s\n",
                g_hw_detail);
    return 1;
}

/* The OS side registers its module resolver (debug facility) here so a host
 * crash report can name the OS module behind the faulting host pc. */
void emu68k_set_symbolizer(void (*fn)(unsigned long long, char *, unsigned))
{
    j5n_set_symbolizer((j5n_symbolize_fn)fn);
}

void emu68k_run_set_name(struct emu68k_run *r, const char *name)
{
    if (r) snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    /* The name is what a report is about, and it arrives after the run is
     * built, so this is the first point a trace can be opened knowing it. */
    bl_open(name);
}

/* ---- [T1d] the capability-gap ledger: every library call the bridge cannot
 * marshal is RECORDED (lvo + count + last program) and the run aborts with a
 * classified message — the design's no-guessing rule, and the data that drives
 * which function gets marshalled next. Also mirrored to stderr, which hosted
 * AROS forwards to the host log, so the gap is visible without tooling. ---- */
#define EMU68K_LEDGER_MAX 64
static struct { int lvo; unsigned long count; char prog[64]; } g_ledger[EMU68K_LEDGER_MAX];
static int g_ledger_n = 0;

void emu68k_ledger_record(int lvo, const char *prog)
{
    bl_event(BL_SUMMARY, -1, 0, 0, "bridge.gap",
             "\"lvo\":%d,\"offset\":%d", lvo, -6 * lvo);
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
const char *emu68k_guest_cstr(j4_sandbox *sb, uint32_t addr)
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
/* The name of an exec vector, from the generated table (which comes from the
 * .conf). A gap that says "AddTask" answers the next question; a gap that says
 * "LVO 47" only starts a lookup. */
static const char *exec_lvo_name(int lvo)
{
#define EMU68K_EXEC_NAME_ROW(n, s) if (lvo == (n)) return s;
    EMU68K_EXEC_LVO_NAMES(EMU68K_EXEC_NAME_ROW)
#undef EMU68K_EXEC_NAME_ROW
    return NULL;
}

uint8_t emu68k_gread8(j4_sandbox *sb, uint32_t a)
{
    return *(const uint8_t *)j4_sandbox_host(sb, a);
}

void emu68k_gwrite8(j4_sandbox *sb, uint32_t a, uint8_t v)
{
    if (a < sb->sandbox_origin || a >= sb->sandbox_origin + sb->size) return;
    *(uint8_t *)j4_sandbox_host(sb, a) = v;
}

uint32_t emu68k_gread32(j4_sandbox *sb, uint32_t a)
{
    const uint8_t *p;
    if (a < sb->sandbox_origin || a + 4 > sb->sandbox_origin + sb->size) return 0;
    p = j4_sandbox_host(sb, a);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
void emu68k_gwrite32(j4_sandbox *sb, uint32_t a, uint32_t v)
{
    uint8_t *p;
    if (a < sb->sandbox_origin || a + 4 > sb->sandbox_origin + sb->size) return;
    p = j4_sandbox_host(sb, a);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
uint32_t emu68k_gread16(j4_sandbox *sb, uint32_t a)
{
    const uint8_t *p;
    if (a < sb->sandbox_origin || a + 2 > sb->sandbox_origin + sb->size) return 0;
    p = j4_sandbox_host(sb, a);
    return ((uint32_t)p[0] << 8) | p[1];
}
void emu68k_gwrite16(j4_sandbox *sb, uint32_t a, uint32_t v)
{
    uint8_t *p;
    if (a < sb->sandbox_origin || a + 2 > sb->sandbox_origin + sb->size) return;
    p = j4_sandbox_host(sb, a);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
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
uint32_t emu68k_guest_alloc(struct emu68k_run *r, uint32_t size)
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

/* A base the guest calls a DEVICE through. Devices reach their vectors exactly
 * as libraries do, so this is the same facade table: one base per device name,
 * reused on a second open the way a native base is. */
unsigned long emu68k_run_device_base(emu68k_run *r, const char *name)
{
    uint32_t base;
    int i;
    if (!r || !name) return 0;
    for (i = 0; i < r->nlib; i++)
        if (!strcmp(r->openlib[i].name, name)) return r->openlib[i].base;
    if (r->nlib >= LIBBASE_MAX) return 0;
    base = LIBBASE_FIRST + (uint32_t)r->nlib * LIBBASE_STRIDE;
    snprintf(r->openlib[r->nlib].name, sizeof r->openlib[r->nlib].name,
             "%s", name);
    r->openlib[r->nlib].base = base;
    r->nlib++;
    j5d_register_libbase(base);
    memset(j4_sandbox_host(&r->sb, base), 0, 64);
    return base;
}

unsigned long emu68k_run_guest_alloc(emu68k_run *r, unsigned long size)
{
    if (!r || size > UINT32_MAX) return 0;
    return (unsigned long)emu68k_guest_alloc(r, (uint32_t)size);
}

uint32_t emu68k_guest_strdup(struct emu68k_run *r, const char *s, size_t n)
{
    uint32_t a = emu68k_guest_alloc(r, (uint32_t)n + 1);
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
int emu68k_dos_readargs(struct emu68k_run *r, j4_sandbox *sb,
                        struct j5d_m68k_state *st, char *e, unsigned el)
{
    const char *tmpl = emu68k_guest_cstr(sb, st->d[1]);
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
                emu68k_gwrite32(sb, arr + 4u * (uint32_t)k, 1u);          /* DOSTRUE-ish */
            } else if (i + 1 < ntok) {
                used[i + 1] = 1;
                if (items[k].num) {
                    uint32_t cell = emu68k_guest_alloc(r, 4);
                    emu68k_gwrite32(sb, cell, (uint32_t)strtol(tok[i + 1], NULL, 10));
                    emu68k_gwrite32(sb, arr + 4u * (uint32_t)k, cell);
                } else {
                    emu68k_gwrite32(sb, arr + 4u * (uint32_t)k,
                             emu68k_guest_strdup(r, tok[i + 1], strlen(tok[i + 1])));
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
            if (emu68k_gread32(sb, arr + 4u * (uint32_t)i)) continue;     /* already set */
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
                emu68k_gwrite32(sb, arr + 4u * (uint32_t)i, emu68k_guest_strdup(r, joined, n));
            } else if (items[i].mult) {             /* /M: a string vector       */
                uint32_t vec, cnt = 0, j2;
                for (j2 = (uint32_t)t; j2 < (uint32_t)ntok; j2++) if (!used[j2]) cnt++;
                vec = emu68k_guest_alloc(r, (cnt + 1) * 4);
                cnt = 0;
                for (j2 = (uint32_t)t; j2 < (uint32_t)ntok; j2++) {
                    if (used[j2]) continue;
                    emu68k_gwrite32(sb, vec + 4 * cnt,
                             emu68k_guest_strdup(r, tok[j2], strlen(tok[j2])));
                    used[j2] = 1; cnt++;
                }
                emu68k_gwrite32(sb, vec + 4 * cnt, 0);     /* NULL terminator           */
                emu68k_gwrite32(sb, arr + 4u * (uint32_t)i, vec);
            } else if (items[i].num) {
                uint32_t cell = emu68k_guest_alloc(r, 4);
                emu68k_gwrite32(sb, cell, (uint32_t)strtol(tok[t], NULL, 10));
                emu68k_gwrite32(sb, arr + 4u * (uint32_t)i, cell);
                used[t] = 1;
            } else {
                emu68k_gwrite32(sb, arr + 4u * (uint32_t)i,
                         emu68k_guest_strdup(r, tok[t], strlen(tok[t])));
                used[t] = 1;
            }
        }
    }

    /* required arguments must have been satisfied */
    for (i = 0; i < nit; i++)
        if (items[i].req && !emu68k_gread32(sb, arr + 4u * (uint32_t)i)) {
            r->last_ioerr = ERROR_REQUIRED_ARG_MISSING_;
            st->d[0] = 0;                            /* the AmigaDOS failure     */
            return 0;
        }

    /* a guest RDArgs the program can hold and hand to FreeArgs */
    st->d[0] = emu68k_guest_alloc(r, 64);
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
    uint32_t start = emu68k_guest_alloc(r, call_init ? 40u : 34u), pc = start;
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
    uint32_t start = emu68k_guest_alloc(r, 32u), pc = start;
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
    if (!emu68k_oscall) return 1;
    return emu68k_oscall("dos.library", lvo, st, r->reserve, emu68k_oscall_user,
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

    guest_path = emu68k_guest_strdup(r, path, pathlen);
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
    scratch = emu68k_guest_alloc(r, size < 65536u ? size : 65536u);
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
    if (emu68k_oscall) {
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

    paths = emu68k_host_getenv("EMU68K_LIBS_PATH");
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

/* Resolve an unqualified command name through an explicitly supplied guest
 * command path. Native C: binaries are normally AArch64 ELF and cannot be
 * entered by this engine; packages and test corpora can put their 68k HUNK
 * commands in EMU68K_COMMAND_PATH without replacing the native installation.
 * Qualified names retain normal AROS DOS path semantics and never consult the
 * host path. */
static uint8_t *resolve_guest_command(const char *name, char *found,
                                      size_t foundlen, size_t *imagelen)
{
    const char *paths;
    uint8_t *p;
    if (!name || !*name || strchr(name, ':') || strchr(name, '/') ||
        strstr(name, ".."))
        return NULL;
    paths = emu68k_host_getenv("EMU68K_COMMAND_PATH");
    while (paths && *paths) {
        const char *end = strchr(paths, ':');
        size_t n = end ? (size_t)(end - paths) : strlen(paths);
        if ((p = try_library_at(paths, n, name, found, foundlen, imagelen)))
            return p;
        paths = end ? end + 1 : NULL;
    }
    return NULL;
}

/* dos.LoadSeg as seen BY a 68k program.  Unlike the outer AROS loader's native
 * proxy, this value is dereferenced by guest code, so it is a classic BPTR chain
 * in the guest arena: BADDR(seg) is the link word and BADDR(seg)+4 is the hunk
 * payload.  The common J4 relocator supplies guest addresses throughout. */
int emu68k_dos_loadseg(struct emu68k_run *r, j4_sandbox *sb,
                             struct j5d_m68k_state *st,
                             char *e, unsigned el)
{
    const char *guest_name = emu68k_guest_cstr(sb, st->d[1]);
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
    if (!image)
        image = resolve_guest_command(name, why, sizeof why, &imagelen);
    if (!image) {
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
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
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
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
    if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
        fprintf(stderr, "[68k] LoadSeg(\"%s\") -> %08x entry=%08x hunks=%d%s%s\n",
                name, bptr, r->guestseg[slot].seg.entry,
                r->guestseg[slot].seg.numhunks, why[0] ? " path=" : "",
                why[0] ? why : "");
    (void)e; (void)el;
    return 0;
}

int emu68k_dos_unloadseg(struct emu68k_run *r,
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

int emu68k_find_guestlib_name(struct emu68k_run *r, const char *name)
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
int emu68k_find_guestlib_base(struct emu68k_run *r, uint32_t base)
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

void emu68k_guestlib_save_preserved(struct guestlib_live *g,
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

int emu68k_load_guestlib(struct emu68k_run *r, const char *name, uint32_t version,
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

int emu68k_guestlib_init_done(struct emu68k_run *r, struct j5d_m68k_state *st,
                              char *e, unsigned el)
{
    int idx = (int)st->d[1];
    if (idx < 0 || idx >= GUESTLIB_MAX) { snprintf(e, el, "bad guest-library continuation"); return 1; }
    struct guestlib_live *g = &r->guestlib[idx];
    if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
        fprintf(stderr, "[68k] guestlib init.done idx=%d name=%s state=%d "
                "d0=%08x base=%08x parent=%d\n",
                idx, g->name, g->state, st->d[0], g->base, g->parent);
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

int emu68k_guestlib_open_done(struct emu68k_run *r, struct j5d_m68k_state *st,
                              char *e, unsigned el)
{
    int idx = (int)st->d[1];
    if (idx < 0 || idx >= GUESTLIB_MAX) return 1;
    struct guestlib_live *g = &r->guestlib[idx];
    if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
        fprintf(stderr, "[68k] guestlib open.done idx=%d name=%s state=%d "
                "d0=%08x root=%08x parent=%d\n",
                idx, g->name, g->state, st->d[0], g->base, g->parent);
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

/* Open a guest library to completion while servicing another bridge call.
 *
 * Some AROS shared libraries assume that process-wide C runtime libraries
 * have already installed their guest task-storage bases.  That is true for a
 * native AROS process, but a transparently started 68k process begins with an
 * intentionally separate, empty 32-bit task-storage namespace.  Dependency
 * bootstrap therefore has to execute the real guest Init/Open code before the
 * dependent library starts; manufacturing its base or copying host storage
 * would cross the pointer boundary incorrectly. */
int emu68k_open_guestlib_now(struct emu68k_run *r, const char *name,
                             uint32_t version, uint32_t *base_out,
                             char *e, unsigned el)
{
    struct j5d_m68k_state call;
    struct guestlib_live *g;
    uint32_t result = 0;
    int idx = emu68k_find_guestlib_name(r, name);

    if (idx >= 0) {
        g = &r->guestlib[idx];
        if (g->state != GL_READY || g->resident.version < version) {
            snprintf(e, el, "%s dependency is not ready", name);
            return 1;
        }
        if (base_out) *base_out = g->base;
        return 0;
    }

    if (emu68k_load_guestlib(r, name, version, &idx, e, el))
        return 1;
    g = &r->guestlib[idx];
    memset(&call, 0, sizeof call);
    emu68k_guestlib_save_preserved(g, &call);
    call.d[0] = (g->resident.flags & GL68_RTF_AUTOINIT) ? g->init.base : 0;
    call.a[0] = g->init.seglist;
    call.a[6] = EXEC_BASE;
    if (emu68k_run_guest_subroutine(r, g->init_trampoline, &call, 0,
                                    &result, e, el) != 0)
        return 1;
    if (g->state != GL_READY || !result) {
        snprintf(e, el, "%s dependency Init/Open returned zero", name);
        return 1;
    }
    if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
        fprintf(stderr, "[68k] guest dependency %s ready at %08x\n",
                name, result);
    if (base_out) *base_out = result;
    return 0;
}

int emu68k_guestlib_close_done(struct emu68k_run *r, struct j5d_m68k_state *st,
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

int emu68k_guestlib_reclaim(struct emu68k_run *r, struct j5d_m68k_state *st,
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
/* The guest Task of the context that is CURRENTLY running. FindTask(NULL),
 * a signal and a wait are all per-context, and answering them from the one
 * fixed guest Process was right only while there was one. */
#define LVO_EVENT_PUMP 9001

/* Who is waiting on what. A port value alone cannot say whether the context
 * that OWNS a port ever runs; only the context identity can, and that is the
 * difference between "input is not being delivered" and "this program has not
 * reached its interactive state". Opt-in: EMU68K_TRACE_TASKS=1. */
uint32_t emu68k_ctx_task(struct emu68k_run *r);

int emu68k_trace_tasks(void)
{
    static int on = -1;
    if (on < 0) on = emu68k_host_getenv("EMU68K_TRACE_TASKS") ? 1 : 0;
    return on;
}

void emu68k_trace_port_call(struct emu68k_run *r, const char *what,
                            struct j5d_m68k_state *st, uint32_t arg)
{
    if (!emu68k_trace_tasks()) return;
    fprintf(stderr, "[68k/task] ctx=%d task=%08x pc=%08x %s %08x\n",
            r->cur_ctx, emu68k_ctx_task(r), st->pc, what, arg);
}

static const char *event_kind_name(unsigned kind)
{
    switch (kind) {
    case EMU68K_EVENT_IDCMP: return "idcmp";
    case EMU68K_EVENT_DEVICE: return "device";
    default: return "unknown";
    }
}

int emu68k_event_bind(struct emu68k_run *r, unsigned kind, uint32_t identity,
                      uint32_t port, uint32_t mask, const char *reason,
                      uint32_t pc)
{
    int free_slot = -1;
    for (int i = 0; i < EMU68K_EVENT_MAX; i++) {
        if (r->event_source[i].live &&
            r->event_source[i].kind == kind &&
            r->event_source[i].identity == identity) {
            r->event_source[i].port = port;
            r->event_source[i].mask = mask;
            free_slot = i;
            break;
        }
        if (!r->event_source[i].live && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        bl_event(BL_SUMMARY, r->cur_ctx, emu68k_ctx_task(r), pc,
                 "event.source.overflow", "\"kind\":\"%s\"",
                 event_kind_name(kind));
        return 1;
    }
    r->event_source[free_slot].kind = (uint8_t)kind;
    r->event_source[free_slot].identity = identity;
    r->event_source[free_slot].port = port;
    r->event_source[free_slot].mask = mask;
    r->event_source[free_slot].live = 1;
    bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), pc,
             "event.source.bind",
             "\"kind\":\"%s\",\"source\":\"%s\","
             "\"destination\":\"%s\",\"mask\":\"0x%08x\","
             "\"reason\":\"%s\"",
             event_kind_name(kind), bl_id("source", identity),
             bl_id("port", port), mask, reason ? reason : "bind");
    return 0;
}

void emu68k_event_unbind_port(struct emu68k_run *r, uint32_t port,
                              const char *reason)
{
    for (int i = 0; i < EMU68K_EVENT_MAX; i++) {
        if (!r->event_source[i].live || r->event_source[i].port != port)
            continue;
        bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), 0,
                 "event.source.unbind",
                 "\"kind\":\"%s\",\"source\":\"%s\","
                 "\"destination\":\"%s\",\"reason\":\"%s\"",
                 event_kind_name(r->event_source[i].kind),
                 bl_id("source", r->event_source[i].identity),
                 bl_id("port", port), reason ? reason : "unbind");
        memset(&r->event_source[i], 0, sizeof r->event_source[i]);
    }
}

/* Ask the OS-side broker to poll typed native sources. `port` selects one
 * destination for GetMsg/WaitPort; `mask` selects every source able to wake a
 * Wait. Ordinary guest mailboxes match neither and are never fed native data. */
int emu68k_event_pump(struct emu68k_run *r, struct j5d_m68k_state *st,
                      uint32_t port, uint32_t mask, unsigned *matched)
{
    struct j5d_m68k_state probe;
    char scratch[128];
    unsigned local_matches = 0;
    int delivered = 0;
    if (matched) *matched = 0;
    if (!emu68k_oscall || (!port && !mask)) return 0;
    probe = *st;
    probe.a[0] = port;
    probe.d[0] = mask;
    probe.d[1] = 0;
    if (emu68k_oscall("exec.library", LVO_EVENT_PUMP, &probe, r->reserve,
                      emu68k_oscall_user, scratch, sizeof scratch) != 0)
        return 0;
    delivered = (int)probe.d[0];
    local_matches = (unsigned)probe.d[1];
    if (matched) *matched = local_matches;
    if (delivered || local_matches)
        bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), st->pc,
                 "event.pump",
                 "\"destination\":\"%s\",\"mask\":\"0x%08x\","
                 "\"matched_sources\":%u,\"delivered\":%d,"
                 "\"classes\":\"0x%08x\",\"last_class\":\"0x%08x\","
                 "\"last_code\":\"0x%04x\"",
                 port ? bl_id("port", port) : "*", mask,
                 local_matches, delivered, probe.d[2], probe.d[3],
                 probe.d[4] & 0xffffu);
    if (delivered && port)
        bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), st->pc,
                 "port.pump", "\"port\":\"%s\",\"messages\":%d",
                 bl_id("port", port), delivered);
    return delivered;
}

uint32_t emu68k_ctx_task(struct emu68k_run *r)
{
    if (r && r->nctx && r->ctx[r->cur_ctx].live)
        return r->ctx[r->cur_ctx].task;
    return GUEST_PROCESS;
}

/* Waterline routing override: a leaf name listed in EMU68K_GUESTSIDE_LIBS
 * (comma-separated) is served by a guest-side 68k library even though the
 * bridge could serve it natively. When the 68k file cannot be found the open
 * FAILS rather than falling back to the bridged one: a run must be entirely
 * one route or the other for the two to be comparable. */
int emu68k_route_guestside(const char *leaf)
{
    const char *p = emu68k_host_getenv("EMU68K_GUESTSIDE_LIBS");
    size_t n = strlen(leaf);
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg == n && !strncmp(p, leaf, n)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

int emu68k_run_context_nested(struct emu68k_run *r, j4_sandbox *sb, int idx,
                              char *e, unsigned el);
static int bridge(int lvo, struct j5d_m68k_state *st, void *user,
                  char *e, unsigned el);
static j5d_poll_action quantum_poll(void *user);
static j5d_poll_action nested_poll(void *user);
static j5d_poll_action context_poll(void *user);

uint32_t emu68k_callback_stack_acquire(struct emu68k_run *r, char *err,
                                       unsigned errlen)
{
    unsigned depth;
    uint32_t stack;
    if (!r || r->callback_depth >= EMU68K_CALLBACK_DEPTH_MAX) {
        if (err && errlen)
            snprintf(err, errlen, "more than %d nested guest callbacks",
                     EMU68K_CALLBACK_DEPTH_MAX);
        return 0;
    }
    depth = r->callback_depth++;
    if (!r->callback_stack_top[depth]) {
        stack = emu68k_guest_alloc(r, 16384u);
        if (!stack) {
            r->callback_depth--;
            if (err && errlen)
                snprintf(err, errlen, "guest memory exhausted for callback stack");
            return 0;
        }
        r->callback_stack_top[depth] = (stack + 16384u) & ~15u;
    }
    return r->callback_stack_top[depth];
}

void emu68k_callback_stack_release(struct emu68k_run *r)
{
    if (r && r->callback_depth) r->callback_depth--;
}
int emu68k_run_guest_subroutine(struct emu68k_run *r, uint32_t entry,
                                struct j5d_m68k_state *initial,
                                uint32_t stack_top, uint32_t *result,
                                char *e, unsigned el);
int emu68k_add_guest_task_context(struct emu68k_run *r, j4_sandbox *sb,
                                  uint32_t task, uint32_t initial,
                                  uint32_t final, uint32_t tags,
                                  struct j5d_m68k_state *caller,
                                  char *e, unsigned el);
int emu68k_create_guest_task(struct emu68k_run *r, j4_sandbox *sb,
                             uint32_t tags, struct j5d_m68k_state *caller,
                             char *e, unsigned el);

int emu68k_exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *st, char *e, unsigned el);

static int guest_span_ok(j4_sandbox *sb, uint32_t addr, uint32_t size)
{
    return addr >= sb->sandbox_origin &&
           (uint64_t)addr + size <= (uint64_t)sb->sandbox_origin + sb->size;
}

/* EMU68K_TRACE_CALLS: log every library call a program makes. */
static int g_trace = -1;

static void trace_call(struct emu68k_run *r, const char *lib, int lvo,
                       struct j5d_m68k_state *st)
{
    if (g_trace < 0) g_trace = emu68k_host_getenv("EMU68K_TRACE_CALLS") ? 1 : 0;
    if (!g_trace) return;
    fprintf(stderr, "[68k] %s LVO %d (%d) pc=%08x  "
            "d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x "
            "d6=%08x d7=%08x "
            "a0=%08x a1=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x\n",
            lib, lvo, -6 * lvo, st->pc,
            st->d[0], st->d[1], st->d[2], st->d[3], st->d[4], st->d[5],
            st->d[6], st->d[7], st->a[0], st->a[1], st->a[3],
            st->a[4], st->a[5], st->a[6], st->a[7]);
    (void)r;
}

/* Run one context until it blocks back, finishes, or faults.
 *
 * Nested, on this thread, with its own engine instance - the shape the engine
 * already proves in T0P3. `on_stack` marks it for the duration so a wait inside
 * it cannot ask to re-enter a context that is below it on this very stack. */
int emu68k_run_context_nested(struct emu68k_run *r, j4_sandbox *sb, int idx,
                              char *e, unsigned el)
{
    struct emu68k_ctx *ctx = &r->ctx[idx];
    struct bctx c;
    j5d_sandbox nsb;
    j5d_engine *outer_eng = r->ctx[r->cur_ctx].eng;
    int outer = r->cur_ctx, rc;
    uint32_t d0 = 0, pc;

    /* Save the caller's live state before we leave it: the engine writes the
     * 68k state through the pointer j5d_run was given, and we are about to
    * hand it a different one. */
    if (!ctx->started) {
        uint32_t sp;
        memset(&ctx->st, 0, sizeof ctx->st);
        sp = ctx->initial_sp ? ctx->initial_sp :
             ((ctx->stack + ctx->stack_size) & ~15u);
        if (sp < ctx->stack + 4u || sp > ctx->stack + ctx->stack_size) {
            snprintf(e, el, "task %08x has invalid initial SP %08x", ctx->task, sp);
            return 1;
        }
        sp -= 4u;
        emu68k_gwrite32(sb, sp, ctx->final_entry); /* RTS enters finalizer or stops */
        ctx->st.a[7] = sp;
        ctx->st.a[0] = ctx->argstr;
        ctx->st.d[0] = ctx->argsize;
        ctx->started = 1;
    } else if (ctx->blocked) {
        /* It parked in Wait. Deliver what arrived and let Wait return it. */
        uint32_t got = emu68k_gread32(sb, ctx->task + TASK_SIGRECVD_OFF) & ctx->wait_mask;
        if (!got) return 0;                       /* still nothing for it      */
        emu68k_gwrite32(sb, ctx->task + TASK_SIGRECVD_OFF,
                 emu68k_gread32(sb, ctx->task + TASK_SIGRECVD_OFF) & ~got);
        ctx->st.d[0] = got;
        ctx->blocked = 0;
    }
    pc = ctx->st.pc;
    if (!ctx->st.pc) pc = ctx->entry;

    nsb = r->jit_sb;
    c.lib = &r->lib; c.sb = &r->sb; c.run = r;

    bl_event(BL_RUNTIME, idx, ctx->task, pc, "scheduler.resume",
             "\"from\":%d", outer);
    ctx->on_stack = 1;
    r->cur_ctx = idx;
    /* ExecBase is guest memory shared by every translated context.  Classic
     * code commonly reads SysBase->ThisTask directly (including Exec's own
     * CreateMsgPort implementation), so changing only ctx_task() leaves ports
     * owned by whichever task happened to run first. */
    emu68k_gwrite32(sb, EXEC_BASE + EXECBASE_THISTASK, ctx->task);
    j5d_engine_activate(ctx->eng);
    /* The bases a program has opened are recorded IN the engine instance, and
     * this context has its own. Without replaying them its engine does not know
     * that a jsr through a library base is a bridge call, and tries to decode
     * the base as code. Replayed on every entry, not just the first, because
     * the parent keeps opening libraries after the child exists. */
    {
        int i;
        j5d_register_libbase(EXEC_BASE);
        for (i = 0; i < r->nlib; i++)
            j5d_register_libbase(r->openlib[i].base);
        for (i = 0; i < GUESTLIB_MAX; i++)
            if (r->guestlib[i].state == GL_READY && r->guestlib[i].base)
                j5d_register_guest_libbase(r->guestlib[i].base);
    }
    /* A child context may be a polling task that never calls Wait.  Give it a
     * bounded quantum so it can park at an engine safe point and return the
     * turn without recursively entering the parent below it on this stack. */
    /* Keep child turns short. A helper may signal its parent and then enter an
     * endless polling loop; waiting thousands of translated blocks before the
     * parent observes that signal is user-visible starvation. */
    j5d_set_poll(context_poll, r, 64u);
    if (setjmp(ctx->unwind) == 0) {
        int dom = domain_enter_guest();   /* this context IS the guest again */
        ctx->can_unwind = 1;
        rc = j5d_run(&nsb, pc, RUN_LIBBASE, &ctx->st, &d0, bridge, &c, e, el);
        ctx->can_unwind = 0;
        domain_leave_guest(dom);
        if (rc == 0) {
            bl_event(BL_RUNTIME, idx, ctx->task, ctx->st.pc,
                     "scheduler.finish", "\"reason\":\"return\"");
            ctx->finished = 1;            /* it returned: the process exited  */
        } else if (rc == J5D_RC_YIELD) {
            if (!ctx->blocked)
                bl_event(BL_RUNTIME, idx, ctx->task, ctx->st.pc,
                         "scheduler.yield", "\"reason\":\"quantum\"");
            rc = 0;                       /* parked safely; parent continues  */
        }
    } else {
        ctx->can_unwind = 0;              /* it blocked back; state is parked */
        if (ctx->finished)
            bl_event(BL_RUNTIME, idx, ctx->task, ctx->st.pc,
                     "scheduler.finish", "\"reason\":\"guest-exit\"");
        rc = 0;
    }
    j5d_engine_activate(outer_eng);
    j5d_set_poll(quantum_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    r->cur_ctx = outer;
    emu68k_gwrite32(sb, EXEC_BASE + EXECBASE_THISTASK,
                    r->ctx[outer].task);
    ctx->on_stack = 0;

    return rc != 0;
}

/* Give each runnable sibling guest context one cooperative turn.  Native AROS
 * may block inside a bridged call such as graphics.WaitTOF, but its scheduler
 * cannot see our 68k contexts: without an explicit handoff the caller resumes
 * after the native wait and can starve every guest sibling indefinitely. */
int emu68k_reschedule_siblings(struct emu68k_run *r, j4_sandbox *sb,
                               const char *reason, uint32_t pc,
                               char *e, unsigned el)
{
    int outer = r->cur_ctx;
    int ran = 0;

    bl_event(BL_RUNTIME, outer, emu68k_ctx_task(r), pc, "scheduler.yield",
             "\"reason\":\"%s\"", reason ? reason : "cooperative");
    for (int i = 0; i < r->nctx; i++)
    {
        struct emu68k_ctx *other = &r->ctx[i];
        if (i == outer || !other->live || other->finished || other->on_stack)
            continue;
        if (other->blocked &&
            !(emu68k_gread32(sb, other->task + TASK_SIGRECVD_OFF) &
              other->wait_mask))
            continue;
        if (emu68k_run_context_nested(r, sb, i, e, el) != 0)
            return 1;
        ran++;
    }
    bl_event(BL_RUNTIME, outer, emu68k_ctx_task(r), pc, "scheduler.resume",
             "\"after\":\"%s\",\"siblings_ran\":%d",
             reason ? reason : "cooperative", ran);
    return 0;
}

static uint32_t task_next_tag(j4_sandbox *sb, uint32_t *cursor,
                              char *e, unsigned el)
{
    unsigned guard = 0;
    uint32_t p = *cursor;
    while (p && ++guard < 65536u) {
        uint32_t tag, data;
        if (!guest_span_ok(sb, p, 8u)) {
            snprintf(e, el, "task tag list %08x is outside guest memory", p);
            *cursor = 0;
            return UINT32_MAX;
        }
        tag = emu68k_gread32(sb, p);
        data = emu68k_gread32(sb, p + 4u);
        if (tag == 0) { *cursor = 0; return 0; }          /* TAG_DONE */
        if (tag == 1) { p += 8u; continue; }             /* TAG_IGNORE */
        if (tag == 2) { p = data; continue; }            /* TAG_MORE */
        if (tag == 3) {                                  /* TAG_SKIP */
            if (data > 0x1fffffffu) {
                snprintf(e, el, "task TAG_SKIP is out of range");
                *cursor = 0;
                return UINT32_MAX;
            }
            p += 8u * (data + 1u);
            continue;
        }
        *cursor = p + 8u;
        return p;
    }
    if (p) snprintf(e, el, "task tag list exceeds 65536 items or contains a cycle");
    *cursor = 0;
    return p ? UINT32_MAX : 0;
}

int emu68k_add_guest_task_context(struct emu68k_run *r, j4_sandbox *sb,
                                  uint32_t task, uint32_t initial,
                                  uint32_t final, uint32_t tags,
                                  struct j5d_m68k_state *caller,
                                  char *e, unsigned el)
{
    struct emu68k_ctx *ctx;
    uint32_t lower, upper, sp, cursor = tags, tagp;
    uint32_t argv[8] = {0}, argmask = 0, prelaunch = 0;
    int idx;

    if (!guest_span_ok(sb, task, M68K_Task_SIZEOF) ||
        !guest_span_ok(sb, initial, 2u)) {
        snprintf(e, el, "AddTask Task or initialPC is outside guest memory");
        caller->d[0] = 0;
        return 1;
    }
    while ((tagp = task_next_tag(sb, &cursor, e, el)) != 0) {
        uint32_t tag, data;
        if (tagp == UINT32_MAX) { caller->d[0] = 0; return 1; }
        tag = emu68k_gread32(sb, tagp); data = emu68k_gread32(sb, tagp + 4u);
        if (tag >= TASKTAG_ARG1 && tag <= TASKTAG_ARG8) {
            unsigned n = tag - TASKTAG_ARG1;
            argv[n] = data;
            argmask |= 1u << n;
        } else if (tag == TASKTAG_PRELAUNCHHOOK) {
            prelaunch = data;
        }
    }
    lower = emu68k_gread32(sb, task + TASK_SPLOWER_OFF);
    upper = emu68k_gread32(sb, task + TASK_SPUPPER_OFF);
    sp = emu68k_gread32(sb, task + TASK_SPREG_OFF);
    if (!sp) sp = upper;
    if (lower < sb->sandbox_origin || upper <= lower || sp < lower || sp > upper ||
        !guest_span_ok(sb, lower, upper - lower)) {
        snprintf(e, el, "AddTask has invalid stack %08x..%08x sp=%08x",
                 lower, upper, sp);
        caller->d[0] = 0;
        return 1;
    }
    if (argmask) {
        if (sp < lower + 36u) {
            snprintf(e, el, "AddTask stack has no room for TASKTAG_ARG1..8");
            caller->d[0] = 0;
            return 1;
        }
        sp -= 32u;
        for (unsigned i = 0; i < 8; i++) emu68k_gwrite32(sb, sp + i * 4u, argv[i]);
    }
    if (r->nctx == 0) {
        r->ctx[0].eng = r->eng;
        r->ctx[0].task = GUEST_PROCESS;
        r->ctx[0].live = 1;
        r->ctx[0].started = 1;
        r->nctx = 1;
        r->cur_ctx = 0;
    }
    if (r->nctx >= EMU68K_MAX_CTX) {
        snprintf(e, el, "more guest Tasks than this run keeps");
        caller->d[0] = 0;
        return 1;
    }
    if (!emu68k_gread8(sb, task + M68K_Task_tc_Node_ln_Type))
        emu68k_gwrite8(sb, task + M68K_Task_tc_Node_ln_Type, 1); /* NT_TASK */
    emu68k_gwrite8(sb, task + M68K_Task_tc_State, 1);            /* TS_ADDED */
    emu68k_gwrite8(sb, task + M68K_Task_tc_IDNestCnt, UINT8_MAX);
    emu68k_gwrite8(sb, task + M68K_Task_tc_TDNestCnt, UINT8_MAX);
    emu68k_gwrite32(sb, task + M68K_Task_tc_SigWait, 0);
    emu68k_gwrite32(sb, task + TASK_SIGRECVD_OFF, 0);
    emu68k_gwrite32(sb, task + TASK_SIGEXCEPT_OFF, 0);
    if (!emu68k_gread32(sb, task + M68K_Task_tc_MemEntry_lh_Head)) {
        uint32_t list = task + M68K_Task_tc_MemEntry_lh_Head;
        emu68k_gwrite32(sb, list, list + 4u);
        emu68k_gwrite32(sb, list + 4u, 0);
        emu68k_gwrite32(sb, list + 8u, list);
    }
    emu68k_gwrite32(sb, task + TASK_SPREG_OFF, sp);

    idx = r->nctx;
    ctx = &r->ctx[idx];
    memset(ctx, 0, sizeof *ctx);
    ctx->eng = j5d_engine_new();
    if (!ctx->eng) {
        snprintf(e, el, "no engine instance for guest Task");
        caller->d[0] = 0;
        return 1;
    }
    ctx->task = task;
    ctx->entry = initial;
    ctx->final_entry = final;
    ctx->stack = lower;
    ctx->stack_size = upper - lower;
    ctx->initial_sp = sp;
    ctx->live = 1;
    r->nctx++;

    if (prelaunch) {
        uint32_t hook_entry;
        if (!guest_span_ok(sb, prelaunch, M68K_Hook_SIZEOF)) {
            snprintf(e, el, "TASKTAG_PRELAUNCHHOOK is outside guest memory");
            return 1;
        }
        hook_entry = emu68k_gread32(sb, prelaunch + M68K_Hook_h_Entry);
        if (emu68k_run_call_hook(r, hook_entry, prelaunch, task, 0, NULL, e, el) != 0)
            return 1;
    }
    if (emu68k_run_context_nested(r, sb, idx, e, el) != 0) return 1;
    caller->d[0] = task;
    return 0;
}

int emu68k_create_guest_task(struct emu68k_run *r, j4_sandbox *sb,
                             uint32_t tags, struct j5d_m68k_state *caller,
                             char *e, unsigned el)
{
    uint32_t cursor = tags, tagp, errorp = 0, initial = 0, final = 0;
    uint32_t stacksize = 16384u, name = 0, userdata = 0, flags = 0;
    uint32_t extra = 0, msgportp = 0, startupmsg = 0;
    uint32_t task, stack, port = 0, copied_name = 0;
    int pri = 0;

    while ((tagp = task_next_tag(sb, &cursor, e, el)) != 0) {
        uint32_t tag, data;
        if (tagp == UINT32_MAX) goto fail;
        tag = emu68k_gread32(sb, tagp); data = emu68k_gread32(sb, tagp + 4u);
        switch (tag) {
        case TASKTAG_ERROR: errorp = data; break;
        case TASKTAG_CODETYPE:
            if (data != 0) {
                snprintf(e, el, "NewCreateTaskA requests non-68k code type %u", data);
                goto fail;
            }
            break;
        case TASKTAG_PC: initial = data; break;
        case TASKTAG_FINALPC: final = data; break;
        case TASKTAG_STACKSIZE: stacksize = data; break;
        case TASKTAG_NAME: name = data; break;
        case TASKTAG_USERDATA: userdata = data; break;
        case TASKTAG_PRI: pri = (int8_t)data; break;
        case TASKTAG_POOLPUDDLE: case TASKTAG_POOLTHRESH:
        case TASKTAG_AFFINITY: case TASKTAG_PRELAUNCHHOOK:
            break; /* allocator/scheduler hints; semantics are unchanged here */
        case TASKTAG_STARTUPMSG: startupmsg = data; break;
        case TASKTAG_TASKMSGPORT: msgportp = data; break;
        case TASKTAG_FLAGS: flags = data; break;
        case TASKTAG_TCBEXTRASIZE: extra = data; break;
        default:
            if (tag < TASKTAG_ARG1 || tag > TASKTAG_ARG8) {
                snprintf(e, el, "NewCreateTaskA tag $%08x is not described", tag);
                goto fail;
            }
            break;
        }
    }
    if (!initial || !guest_span_ok(sb, initial, 2u)) {
        snprintf(e, el, "NewCreateTaskA has no valid TASKTAG_PC");
        goto fail;
    }
    if (stacksize < 16384u) stacksize = 16384u;
    if (extra > 65536u || stacksize > 8u * 1024u * 1024u) {
        snprintf(e, el, "NewCreateTaskA allocation sizes are out of range");
        goto fail;
    }
    task = emu68k_guest_alloc(r, M68K_Task_SIZEOF + extra);
    stack = emu68k_guest_alloc(r, stacksize);
    if (!task || !stack) {
        snprintf(e, el, "guest memory exhausted creating Task");
        goto fail;
    }
    if (name) {
        const char *s = emu68k_guest_cstr(sb, name);
        if (!s || !(copied_name = emu68k_guest_strdup(r, s, strlen(s)))) {
            snprintf(e, el, "NewCreateTaskA task name is invalid");
            goto fail;
        }
    }
    emu68k_gwrite8(sb, task + M68K_Task_tc_Node_ln_Type, 1);     /* NT_TASK */
    emu68k_gwrite8(sb, task + M68K_Task_tc_Node_ln_Pri, (uint8_t)pri);
    emu68k_gwrite32(sb, task + M68K_Task_tc_Node_ln_Name, copied_name);
    emu68k_gwrite8(sb, task + M68K_Task_tc_Flags, (uint8_t)flags);
    emu68k_gwrite32(sb, task + M68K_Task_tc_UserData, userdata);
    emu68k_gwrite32(sb, task + TASK_SPLOWER_OFF, stack);
    emu68k_gwrite32(sb, task + TASK_SPUPPER_OFF, stack + stacksize);
    emu68k_gwrite32(sb, task + TASK_SPREG_OFF, stack + stacksize);
    if (msgportp) {
        if (!guest_span_ok(sb, msgportp, 4u)) {
            snprintf(e, el, "TASKTAG_TASKMSGPORT result pointer is outside guest memory");
            goto fail;
        }
        port = emu68k_guest_alloc(r, M68K_MsgPort_SIZEOF);
        if (!port) {
            snprintf(e, el, "guest memory exhausted creating Task MsgPort");
            goto fail;
        }
        emu68k_gwrite8(sb, port + M68K_MsgPort_mp_Node_ln_Type, 4); /* NT_MSGPORT */
        emu68k_gwrite8(sb, port + MP_SIGBIT, 15);
        emu68k_gwrite32(sb, port + MP_SIGTASK, task);
        emu68k_gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_Head,
                 port + MP_MSGLIST + M68K_List_lh_Tail);
        emu68k_gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_Tail, 0);
        emu68k_gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_TailPred,
                 port + MP_MSGLIST + M68K_List_lh_Head);
        emu68k_gwrite32(sb, task + TASK_SIGALLOC_OFF,
                 emu68k_gread32(sb, task + TASK_SIGALLOC_OFF) | (1u << 15));
        emu68k_gwrite32(sb, msgportp, port);
    }
    if (emu68k_add_guest_task_context(r, sb, task, initial, final, tags,
                               caller, e, el) != 0) goto fail;
    if (startupmsg) {
        struct j5d_m68k_state send = *caller;
        if (!port) {
            snprintf(e, el, "TASKTAG_STARTUPMSG needs TASKTAG_TASKMSGPORT");
            goto fail;
        }
        send.a[0] = port;
        send.a[1] = startupmsg;
        if (emu68k_exec_call(r, sb, LVO_PUTMSG, &send, e, el) != 0) goto fail;
    }
    if (errorp && guest_span_ok(sb, errorp, 4u)) emu68k_gwrite32(sb, errorp, 0);
    caller->d[0] = task;
    return 0;

fail:
    if (errorp && guest_span_ok(sb, errorp, 4u)) emu68k_gwrite32(sb, errorp, 1);
    caller->d[0] = 0;
    return 1;
}

/* CreateNewProc(tags D1) -> struct Process *
 *
 * The tags that matter are NP_Entry (68k code), NP_StackSize and NP_Name. The
 * result is the child's guest Process, which is what the parent will PutMsg to
 * - and because the child's port lives in guest memory, the parent can address
 * it. Tags this cannot honour are REFUSED by number rather than ignored: a
 * process silently started without the input stream or the current directory it
 * asked for is worse than one that did not start. */
/* The signal a Process's own port uses. AmigaOS reserves the low bits for the
 * system's own use and a port needs one that nothing else claims. */
#define EMU68K_PROC_SIGBIT 8
#define PROC_MSGPORT M68K_Process_pr_MsgPort_mp_Node_ln_Succ

#define NP_Entry     0x800003EBu
#define NP_StackSize 0x800003F3u
#define NP_Name      0x800003F4u
#define NP_Priority  0x800003F5u
#define NP_Input     0x800003ECu
#define NP_Output    0x800003EDu
#define NP_CloseIn   0x800003EEu
#define NP_CloseOut  0x800003EFu
#define NP_Error     0x800003F0u
#define NP_CloseErr  0x800003F1u
#define NP_Cli       0x800003FAu
#define NP_Arguments 0x800003FDu

/* AROS libc does not call a bridge vector to discover process identity: its
 * GetETask() macro reads tc_Flags and tc_UnionETask directly.  Every guest
 * Process therefore needs a real m68k-layout ETask mirror, including children
 * created by CreateNewProc. */
static void init_guest_etask(j4_sandbox *sb, uint32_t task, uint32_t etask,
                             uint32_t unique_id, uint32_t parent)
{
    uint32_t children = etask + M68K_ETask_et_Children_mlh_Head;
    uint32_t messages = etask + M68K_ETask_et_TaskMsgPort_mp_MsgList_lh_Head;

    memset(j4_sandbox_host(sb, etask), 0, M68K_ETask_SIZEOF);
    emu68k_gwrite8(sb, task + M68K_Task_tc_Flags,
                   emu68k_gread8(sb, task + M68K_Task_tc_Flags) | TASKF_ETASK);
    emu68k_gwrite32(sb, task + TASK_ETASK_OFF, etask);
    emu68k_gwrite32(sb, etask + M68K_ETask_et_Parent, parent);
    emu68k_gwrite32(sb, etask + M68K_ETask_et_UniqueID, unique_id);

    /* Empty MinList/List sentinels, in the guest address space. */
    emu68k_gwrite32(sb, children, children + 4u);
    emu68k_gwrite32(sb, children + 4u, 0);
    emu68k_gwrite32(sb, children + 8u, children);
    emu68k_gwrite32(sb, messages, messages + 4u);
    emu68k_gwrite32(sb, messages + 4u, 0);
    emu68k_gwrite32(sb, messages + 8u, messages);
    emu68k_gwrite32(sb,
                    etask + M68K_ETask_et_TaskMsgPort_mp_SigTask, task);
}

int emu68k_dos_create_new_proc(struct emu68k_run *r, j4_sandbox *sb,
                               struct j5d_m68k_state *st, char *e, unsigned el)
{
    uint32_t tags = st->d[1], entry = 0, stacksize = 4096;
    uint32_t name = 0, input = 0, output = 0, error = 0, want_cli = 0;
    uint32_t arguments = 0;
    int32_t priority = 0;
    uint32_t t, v, at, etask, parent_task;
    struct emu68k_ctx *ctx;
    int idx;

    for (at = tags; at; at += 8) {
        if (!guest_span_ok(sb, at, 8u)) {
            snprintf(e, el, "CreateNewProc taglist leaves guest memory at $%08lx",
                     (unsigned long)at);
            return 1;
        }
        t = emu68k_gread32(sb, at);
        v = emu68k_gread32(sb, at + 4);
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] CreateNewProc tag[%08x]=%08x,%08x\n",
                    at, t, v);
        if (!t) break;                                   /* TAG_DONE          */
        if (t == 1u) continue;                           /* TAG_IGNORE        */
        if (t == 2u) {                                   /* TAG_MORE          */
            if (!v) break;
            at = v - 8u;
            continue;
        }
        if (t == 3u) {                                   /* TAG_SKIP          */
            at += v * 8u;
            continue;
        }
        /* AROS's inline stdarg wrappers construct a finite automatic array,
         * but longstanding callers commonly omit TAG_DONE. Native DOS then
         * happens to stop after the function-specific TAG_USER values. Make
         * that de-facto ABI deterministic: a non-control system-space value
         * terminates this NP_/ADO-only list instead of scanning stack garbage. */
        if (!(t & 0x80000000u)) break;
        switch (t) {
        case NP_Entry:     entry = v; break;
        case NP_StackSize: stacksize = v; break;
        case NP_Name:      name = v; break;
        case NP_Priority:  priority = (int32_t)v; break;
        case NP_Input:     input = v; break;
        case NP_Output:    output = v; break;
        case NP_Error:     error = v; break;
        case NP_Cli:       want_cli = v; break;
        case NP_Arguments: arguments = v; break;
        case NP_CloseIn: case NP_CloseOut: case NP_CloseErr:
            break;                    /* cleanup policy when the child exits */
        default:
            snprintf(e, el, "capability gap: CreateNewProc tag $%08lx is not "
                     "served, and starting the process without it would be a "
                     "guess", (unsigned long)t);
            return 1;
        }
    }
    if (!entry) {
        snprintf(e, el, "capability gap: CreateNewProc without NP_Entry needs a "
                        "segment to load, which is a different mechanism");
        return 1;
    }
    if (name && !emu68k_guest_cstr(sb, name)) {
        snprintf(e, el, "CreateNewProc NP_Name is not a guest C string");
        return 1;
    }
    if (r->nctx == 0) {                       /* first call: adopt the program */
        r->ctx[0].eng = r->eng;
        r->ctx[0].task = GUEST_PROCESS;
        r->ctx[0].live = 1;
        r->ctx[0].started = 1;
        r->nctx = 1;
        r->cur_ctx = 0;
    }
    if (r->nctx >= EMU68K_MAX_CTX) {
        snprintf(e, el, "capability gap: more 68k processes than this run keeps");
        return 1;
    }
    idx = r->nctx;
    parent_task = emu68k_ctx_task(r);
    ctx = &r->ctx[idx];
    memset(ctx, 0, sizeof *ctx);
    if (stacksize < 16384) stacksize = 16384;
    ctx->stack_size = stacksize;
    ctx->stack = emu68k_guest_alloc(r, stacksize);
    ctx->task  = emu68k_guest_alloc(r, CLASSIC_PROCESS_SIZE);
    etask = emu68k_guest_alloc(r, M68K_ETask_SIZEOF);
    if (!ctx->stack || !ctx->task || !etask) {
        snprintf(e, el, "guest memory exhausted starting a 68k process");
        return 1;
    }
    ctx->eng = j5d_engine_new();
    if (!ctx->eng) {
        snprintf(e, el, "no engine instance for a 68k process");
        return 1;
    }
    /* Its Task has to look like one: a program finds itself with FindTask and
     * then reads its own port and stack bounds out of it. */
    memset(j4_sandbox_host(sb, ctx->task), 0, CLASSIC_PROCESS_SIZE);
    {
        uint8_t *tk = j4_sandbox_host(sb, ctx->task);
        tk[M68K_Process_pr_Task_tc_Node_ln_Type] = NT_PROCESS;
        tk[M68K_Process_pr_Task_tc_Node_ln_Pri] = (uint8_t)priority;
    }
    if (name)
        emu68k_gwrite32(sb, ctx->task + M68K_Process_pr_Task_tc_Node_ln_Name,
                        name);
    emu68k_gwrite32(sb, ctx->task + TASK_SPLOWER_OFF, ctx->stack);
    emu68k_gwrite32(sb, ctx->task + TASK_SPUPPER_OFF, ctx->stack + stacksize);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_STACKSIZE, stacksize);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_STACKBASE,
                    GUEST_MKBADDR(ctx->stack + stacksize));
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_TASKNUM, (uint32_t)idx + 1u);
    init_guest_etask(sb, ctx->task, etask, (uint32_t)idx + 1u, parent_task);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_CIS, input);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_COS, output);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_CES, error);
    emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_ARGUMENTS, arguments);
    if (want_cli) {
        const char *process_name = name ? emu68k_guest_cstr(sb, name) : NULL;
        uint32_t cli = emu68k_guest_alloc(r, M68K_CommandLineInterface_SIZEOF);
        uint32_t command = 0;
        size_t command_len = process_name ? strlen(process_name) : 0;
        if (!cli) {
            snprintf(e, el, "guest memory exhausted creating a 68k CLI");
            return 1;
        }
        memset(j4_sandbox_host(sb, cli), 0, M68K_CommandLineInterface_SIZEOF);
        if (command_len > 255u) command_len = 255u;
        if (command_len) {
            command = emu68k_guest_alloc(r, 256u);
            if (!command) {
                snprintf(e, el, "guest memory exhausted naming a 68k CLI");
                return 1;
            }
            emu68k_gwrite8(sb, command, (uint8_t)command_len);
            memcpy(j4_sandbox_host(sb, command + 1u), process_name, command_len);
            emu68k_gwrite32(sb,
                cli + M68K_CommandLineInterface_cli_CommandName,
                GUEST_MKBADDR(command));
        }
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_FailLevel, 10u);
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_StandardInput,
                        input);
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_CurrentInput,
                        input);
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_CurrentOutput,
                        output);
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_StandardOutput,
                        output);
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_StandardError,
                        error);
        /* The consumer is m68k, where CLI_DEFAULTSTACK_UNIT is four bytes. */
        emu68k_gwrite32(sb, cli + M68K_CommandLineInterface_cli_DefaultStack,
                        (stacksize + 3u) / 4u);
        emu68k_gwrite32(sb, ctx->task + CLASSIC_PR_CLI,
                        GUEST_MKBADDR(cli));
    }
    /* Its pr_MsgPort, ready to be waited on. A process finds itself and waits
     * on this port without ever creating it - it is part of being a Process -
     * so an uninitialised one is a wait on a list that never ends and a signal
     * bit nobody sets. */
    {
        uint32_t port = ctx->task + PROC_MSGPORT;
        uint32_t list = port + MP_MSGLIST;
        emu68k_gwrite32(sb, list + M68K_List_lh_Head, list + M68K_List_lh_Tail);
        emu68k_gwrite32(sb, list + M68K_List_lh_Tail, 0);
        emu68k_gwrite32(sb, list + M68K_List_lh_TailPred, list + M68K_List_lh_Head);
        emu68k_gwrite32(sb, port + MP_SIGTASK, ctx->task);
        *(uint8_t *)j4_sandbox_host(sb, port + MP_SIGBIT) = EMU68K_PROC_SIGBIT;
        emu68k_gwrite32(sb, ctx->task + TASK_SIGALLOC_OFF, 1u << EMU68K_PROC_SIGBIT);
    }
    ctx->entry = entry;
    if (arguments) {
        const char *tail = emu68k_guest_cstr(sb, arguments);
        if (!tail) {
            snprintf(e, el, "CreateNewProc NP_Arguments is not a guest C string");
            return 1;
        }
        ctx->argstr = arguments;
        ctx->argsize = (uint32_t)strlen(tail);
    }
    ctx->live = 1;
    r->nctx++;
    /* Give it its first slice now, so it reaches the port it is about to wait
     * on before the parent sends to it. */
    if (emu68k_run_context_nested(r, sb, idx, e, el) != 0)
        return 1;
    bl_event(BL_RUNTIME, idx, ctx->task, entry, "process.create",
             "\"port\":\"%s\",\"stack_size\":%u",
             bl_id("port", ctx->task + PROC_MSGPORT), (unsigned)stacksize);
    if (emu68k_trace_tasks())
        fprintf(stderr, "[68k/task] ctx=%d CREATED task=%08x entry=%08x "
                "pr_MsgPort=%08x stack=%08x+%u\n", idx, ctx->task, entry,
                ctx->task + PROC_MSGPORT, ctx->stack, (unsigned)stacksize);
    st->d[0] = ctx->task;
    return 0;
}

static int bridge_inner(int lvo, struct j5d_m68k_state *st, void *user,
                        char *e, unsigned el);

static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e,
                  unsigned el)
{
    int rc;
    g_in_bridge++;
    rc = bridge_inner(lvo, st, user, e, el);
    g_in_bridge--;
    /* Pair EMU68K_TRACE_CALLS' entry record with the value the crossing
     * actually returned.  This is deliberately outside bridge_inner so every
     * host adapter and generated OS crossing is covered uniformly.  Redirects
     * are identified explicitly: their register values are only the setup for
     * the guest routine, not that routine's eventual result. */
    if (g_trace > 0)
        fprintf(stderr, "[68k] -> rc=%d d0=%08x d1=%08x a0=%08x a1=%08x"
                " pc=%08x%s\n", rc, st->d[0], st->d[1], st->a[0], st->a[1],
                st->pc, (rc == J5D_LVO_REDIRECT ||
                         rc == J5D_LVO_REDIRECT_RTE) ? " redirect" : "");
    return rc;
}

static int bridge_inner(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    struct emu68k_run *r = c->run;
    uint32_t a6 = st->a[6];

    /* A vector a guest library PATCHED with SetFunction runs the guest routine
     * instead of the bridge - that is what patching means, and it has to hold
     * for bridged libraries too or the patch is silently ignored. Checked
     * before anything is served, for every library. */
    if (r) {
        for (int i = 0; i < EMU68K_PATCH_MAX; i++)
            if (r->patch[i].base == a6 && r->patch[i].lvo == lvo &&
                r->patch[i].guest_fn) {
                st->pc = r->patch[i].guest_fn;
                return J5D_LVO_REDIRECT;
            }
    }

    /* which library did the program call through? */
    if (r && a6 == EXEC_BASE) {
        trace_call(r, "exec.library", lvo, st);
        if (el) e[0] = 0;
        {   /* a redirect is neither "served" nor "failed": the guest is about
             * to run 68k code for this vector, so pass it straight through. */
            int rc = emu68k_exec_call(r, c->sb, lvo, st, e, el);
            if (rc == 0 || rc == J5D_LVO_REDIRECT ||
                rc == J5D_LVO_REDIRECT_RTE || rc == J5D_LVO_BLOCK) return rc;
        }
        if (e[0]) {          /* exec_call said something specific: do not bury it
                              * under a generic "capability gap" message */
            emu68k_ledger_record(lvo, r->name[0] ? r->name : NULL);
            return 1;
        }
        /* fall through to the OS callback: the embedder may serve more of exec */
        if (emu68k_oscall &&
            emu68k_oscall("exec.library", lvo, st, r->reserve, emu68k_oscall_user,
                     e, el) == 0)
            return 0;
        emu68k_ledger_record(lvo, r->name[0] ? r->name : NULL);
        {
            /* Name what the program ASKED for where the vector says. An LVO
             * number tells the reader to go and look it up; "timer.device" or
             * "console.device" tells them what the program wanted and whether
             * it matters. OpenLibrary already does this. */
            const char *what = NULL;
            const char *vname = exec_lvo_name(lvo);
            if (lvo == LVO_OPENDEVICE) what = emu68k_guest_cstr(c->sb, st->a[0]);
            if (what && *what)
                snprintf(e, el, "capability gap: exec.library OpenDevice(\"%s\") "
                                "is not available yet", what);
            else if (vname)
                snprintf(e, el, "capability gap: exec.library.%s (LVO %d, "
                                "offset %d) is not served", vname, lvo, -6 * lvo);
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
            if (el) e[0] = 0;
            /* [T3] dos calls whose RESULTS are guest pointers are served in the
             * guest: handing back native pointers would give the program
             * addresses it cannot dereference. */
            if (!strcmp(r->openlib[i].name, "dos.library")) {
                int hrc = emu68k_dos_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "intuition.library")) {
                int hrc = emu68k_intuition_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "utility.library") &&
                emu68k_utility_call(r, c->sb, lvo, st, e, el) == 0)
                return 0;
            if (!strcmp(r->openlib[i].name, "utility.library") && e[0])
                return 1;
            if (!strcmp(r->openlib[i].name, "graphics.library")) {
                int hrc = emu68k_graphics_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "gadtools.library")) {
                int hrc = emu68k_gadtools_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "layers.library")) {
                int hrc = emu68k_layers_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "cybergraphics.library")) {
                int hrc = emu68k_cybergraphics_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "task.resource")) {
                int hrc = emu68k_taskresource_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (!strcmp(r->openlib[i].name, "timer.device")) {
                int hrc = emu68k_timerdevice_call(r, c->sb, lvo, st, e, el);
                if (hrc == 0 || hrc == J5D_LVO_REDIRECT ||
                    hrc == J5D_LVO_REDIRECT_RTE) return hrc;
                if (e[0]) return 1;
            }
            if (emu68k_oscall &&
                emu68k_oscall(r->openlib[i].name, lvo, st, r->reserve, emu68k_oscall_user,
                         e, el) == 0) {
                if (!strcmp(r->openlib[i].name, "intuition.library"))
                    emu68k_intuition_post_call(r, c->sb, lvo, st);
                return 0;
            }
            /* OpenCatalogA is deliberately allowed to fail: locale clients
             * are required to keep their built-in strings and use those when
             * no catalog can be opened.  The native bridge cannot safely hand
             * a 64-bit Catalog pointer or native TagItem list to this guest,
             * so until the generated opaque-handle/tag shadow exists, NULL is
             * the honest compatibility result.  Keep this AFTER emu68k_oscall so an
             * embedder with a complete catalog crossing still wins. */
            if (!strcmp(r->openlib[i].name, "locale.library") && lvo == 25) {
                if (el) e[0] = 0;
                st->d[0] = 0;
                return 0;
            }
            emu68k_ledger_record(lvo, r->name[0] ? r->name : NULL);
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
        emu68k_ledger_record(lvo, r && r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: library function LVO %d (offset %d) on "
                        "an unrecognised base %08x is not marshalled yet",
                 lvo, -6 * lvo, st->a[6]);
    }
    return rc;
}

static j5d_poll_action quantum_poll(void *user);
static j5d_poll_action nested_poll(void *user);
static j5d_poll_action context_poll(void *user);

int emu68k_run_guest_subroutine(struct emu68k_run *r, uint32_t entry,
                                struct j5d_m68k_state *initial,
                                uint32_t stack_top, uint32_t *result,
                                char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    j5d_sandbox sb;
    struct bctx c;
    j5d_engine *eng;
    uint32_t d0 = 0;
    int owns_stack = 0;
    int dom, rc;

    if (!r || !entry || !initial || !guest_span_ok(&r->sb, entry, 2u)) {
        if (err && errlen) snprintf(err, errlen, "invalid guest subroutine context");
        return 1;
    }
    if (!stack_top) {
        stack_top = emu68k_callback_stack_acquire(r, err, errlen);
        if (!stack_top) return 1;
        owns_stack = 1;
    }
    st = *initial;
    st.a[7] = stack_top;
    sb = r->jit_sb;
    c.lib = &r->lib;
    c.sb = &r->sb;
    c.run = r;
    eng = (r->nctx && r->ctx[r->cur_ctx].eng) ? r->ctx[r->cur_ctx].eng : r->eng;
    j5d_engine_activate(eng);
    j5d_set_poll(nested_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    dom = domain_enter_guest();
    rc = j5d_run(&sb, entry, RUN_LIBBASE, &st, &d0, bridge, &c, err, errlen);
    domain_leave_guest(dom);
    j5d_set_poll(quantum_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    if (owns_stack) emu68k_callback_stack_release(r);
    if (rc != 0) return 1;
    if (result) *result = d0;
    return 0;
}

/* RunCommand has one extra control-flow rule: dos.Exit() must unwind only the
 * command invocation and return its supplied code to the caller.  This is the
 * hosted equivalent of dos.library's StackState/longjmp implementation. */
int emu68k_run_guest_command(struct emu68k_run *r, uint32_t entry,
                             struct j5d_m68k_state *initial,
                             uint32_t stack_top, uint32_t *result,
                             char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    j5d_sandbox sb;
    struct bctx c;
    j5d_engine *eng;
    uint32_t d0 = 0;
    volatile int dom = 0;
    int rc;

    if (!r || !entry || !initial || !stack_top ||
        !guest_span_ok(&r->sb, entry, 2u)) {
        if (err && errlen) snprintf(err, errlen, "invalid guest command context");
        return 1;
    }
    st = *initial;
    st.a[7] = stack_top;
    sb = r->jit_sb;
    c.lib = &r->lib;
    c.sb = &r->sb;
    c.run = r;
    eng = (r->nctx && r->ctx[r->cur_ctx].eng) ? r->ctx[r->cur_ctx].eng : r->eng;
    j5d_engine_activate(eng);
    j5d_set_poll(nested_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    r->command_can_unwind = 1;
    if (setjmp(r->command_unwind) == 0) {
        dom = domain_enter_guest();
        rc = j5d_run(&sb, entry, RUN_LIBBASE, &st, &d0,
                     bridge, &c, err, errlen);
        domain_leave_guest(dom);
    } else {
        /* dos.Exit arrived through the bridge.  It deliberately bypasses the
         * guest command's C/68k cleanup, but the host execution domain and
         * poll callback still have to be restored here. */
        domain_leave_guest(dom);
        d0 = r->command_return;
        rc = 0;
    }
    r->command_can_unwind = 0;
    j5d_set_poll(quantum_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    if (rc != 0) return 1;
    if (result) *result = d0;
    return 0;
}

int emu68k_run_call_hook(emu68k_run *r, unsigned long entry,
                         unsigned long hook, unsigned long object,
                         unsigned long message, unsigned int *result,
                         char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    j5d_sandbox sb;
    struct bctx c;
    uint32_t d0 = 0;
    uint32_t stack_top;

    if (!r || entry > UINT32_MAX || hook > UINT32_MAX ||
        object > UINT32_MAX || message > UINT32_MAX)
    {
        if (err && errlen) snprintf(err, errlen, "invalid 68k Hook callback context");
        return 1;
    }
    stack_top = emu68k_callback_stack_acquire(r, err, errlen);
    if (!stack_top) return 1;
    memset(&st, 0, sizeof st);
    st.a[0] = (uint32_t)hook;
    st.a[1] = (uint32_t)message;
    st.a[2] = (uint32_t)object;
    st.a[7] = stack_top;
    sb = r->jit_sb;
    c.lib = &r->lib;
    c.sb = &r->sb;
    c.run = r;
    j5d_engine_activate(r->eng);
    /* A nested callback must complete as part of the native call; yielding it
     * would strand the native stack. Poll for kill/deadline only, never yield. */
    j5d_set_poll(nested_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    int dom = domain_enter_guest();   /* the hook body is GUEST code */
    int rc = j5d_run(&sb, (uint32_t)entry, RUN_LIBBASE, &st, &d0,
                     bridge, &c, err, errlen);
    domain_leave_guest(dom);
    j5d_set_poll(quantum_poll, r, r->poll_quantum ? r->poll_quantum : 4096u);
    emu68k_callback_stack_release(r);
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

/* poll for nested (callback) runs: honour kill requests and the wall-clock
 * deadline, but never yield, since the native stack below cannot be parked. */
static j5d_poll_action nested_poll(void *user)
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
    return J5D_POLL_CONTINUE;
}

/* A cooperatively scheduled 68k child owns no native stack that must complete
 * atomically.  Unlike a native->guest callback, it can therefore yield at an
 * engine safe point and resume from ctx->st.pc on its next turn. */
static j5d_poll_action context_poll(void *user)
{
    struct emu68k_run *r = user;
    j5d_poll_action action = nested_poll(user);
    if (action != J5D_POLL_CONTINUE)
        return action;
    if (r->nctx && r->cur_ctx >= 0 && r->cur_ctx < r->nctx &&
        r->ctx[r->cur_ctx].forbid_depth)
        return J5D_POLL_CONTINUE;
    return J5D_POLL_YIELD;
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
    r->jit_sb.host_mem = (uint8_t *)r->reserve;
    r->jit_sb.origin = 0;
    r->jit_sb.size = GUEST_RESERVE;
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
        j5d_set_ciaa_pra(0xff);          /* active-low buttons: released */
        j5d_clear_libbases();
        j5d_register_libbase(EXEC_BASE);
        j5d_engine_activate(prev);
    }

    j5n_symbols_parse(r->image, imagelen, &r->seg, &r->symtab);
    {
        j5n_diag_init(&r->diag, r->image, imagelen, &r->jit_sb, r->seg.entry, LIBBASE,
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
            eb[LIB_VERSION_OFF]      = (uint8_t)(GUEST_EXEC_VERSION >> 8);
            eb[LIB_VERSION_OFF + 1]  = (uint8_t)GUEST_EXEC_VERSION;
            eb[LIB_REVISION_OFF]     = (uint8_t)(GUEST_EXEC_REV >> 8);
            eb[LIB_REVISION_OFF + 1] = (uint8_t)GUEST_EXEC_REV;
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
        emu68k_gwrite32(&r->sb, GUEST_PROCESS + CLASSIC_PR_TASKNUM, 1);
        init_guest_etask(&r->sb, GUEST_PROCESS, GUEST_ETASK, 1, 0);
        emu68k_gwrite32(&r->sb, GUEST_PROCESS + CLASSIC_PR_ARGUMENTS,
                        ARGS_BASE);
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
        const char *lim = emu68k_host_getenv("EMU68K_MAX_SECONDS");
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
    emu68k_gwrite32(&r->sb, GUEST_PROCESS + TASK_SPREG_OFF, GUEST_TOP);
    emu68k_gwrite32(&r->sb, GUEST_PROCESS + TASK_SPLOWER_OFF, r->stack_lower);
    emu68k_gwrite32(&r->sb, GUEST_PROCESS + TASK_SPUPPER_OFF, r->stack_upper);
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

void emu68k_run_set_mouse_buttons(emu68k_run *r, unsigned int buttons)
{
    j5d_engine *previous;
    uint8_t pra = 0xff;

    if (!r || !r->eng) return;
    if (buttons & 1u) pra &= (uint8_t)~0x40u;
    previous = j5d_engine_active();
    j5d_engine_activate(r->eng);
    j5d_set_ciaa_pra(pra);
    j5d_engine_activate(previous);
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
    j5d_sandbox j5sb = r->jit_sb;
    uint32_t d0 = 0;
    char lerr[256] = {0};

    /* The root context is executing on this native stack too.  Mark it just
     * like a nested context so a sibling that reaches a cooperative yield
     * cannot recursively re-enter its still-active parent. */
    if (r->nctx && r->cur_ctx >= 0 && r->cur_ctx < r->nctx)
        r->ctx[r->cur_ctx].on_stack = 1;
    int rc = j5d_run(&j5sb, r->resume_pc, RUN_LIBBASE, &r->st, &d0,
                     bridge, &c, lerr, sizeof lerr);
    if (r->nctx && r->cur_ctx >= 0 && r->cur_ctx < r->nctx)
        r->ctx[r->cur_ctx].on_stack = 0;
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
        r->failed = 1;          /* routed away is not a clean finish either */
        bl_event(BL_SUMMARY, -1, 0, 0, "bridge.hardware", "\"detail\":\"%s\"",
                 g_hw_detail[0] ? g_hw_detail : "unmapped hardware window");
        return EMU68K_RC_HARDWARE;
    }
    r->done = 1;
    r->failed = 1;
    if (r->diag.bundles_written > 0)
        snprintf(err, errlen, "68k program fault (crash bundle: %s)",
                 r->diag.last_bundle[0] ? r->diag.last_bundle : "written");
    else
        snprintf(err, errlen, "%s", lerr[0] ? lerr : "68k program failed");
    /* A run that FAULTED must not report ok. Anything reading the trace would
     * otherwise treat a crash as a clean finish, which is the same class of
     * lie as reporting a contract supported because nothing exercised it. */
    bl_event(BL_SUMMARY, -1, 0, 0, "bridge.fault", "\"detail\":\"%s\"",
             r->diag.bundles_written > 0 ? "fault, crash bundle written"
                                         : "run failed");
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
    bl_close(!r ? "unknown" : r->failed ? "fault"
                                        : r->done ? "ok" : "incomplete");
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
