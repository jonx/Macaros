/* Private contract between the emu68k host runtime and per-library handlers. */
#ifndef EMU68K_INTERNAL_H
#define EMU68K_INTERNAL_H

#include "emu68k_host.h"
#include "guestlib68k.h"
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j5n_diag.h"
#include "j5n_symbols.h"
#include "stublib.h"

#include <limits.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define GUESTLIB_MAX 16
#define GUESTSEG_MAX 32
#define GUESTPOOL_MAX 32
#define EMU68K_PATCH_MAX 16
#define EMU68K_LIBALIAS_MAX 32
#define GUESTLIB_OPENS_MAX 32
#define EMU68K_MAX_CTX 8
#define LIBBASE_MAX 64
#define EMU68K_EVENT_MAX 32
#define EMU68K_TASK_HOOK_TYPES_MAX 16
#define EMU68K_TASK_HOOKS_MAX 32
#define EMU68K_TASK_STORAGE_MAX 128
#define EMU68K_CALLBACK_DEPTH_MAX 16
#define EMU68K_PUBLIC_PORT_MAX 64

enum emu68k_event_kind {
    EMU68K_EVENT_IDCMP = 1,
    EMU68K_EVENT_DEVICE = 2
};

#define EXEC_BASE       0x00220000u
#define LIBBASE_VECTOR_RESERVE 0x00001000u
#define LIBBASE_FIELDS_SIZE    64u
#define GUEST_PROCESS   0x00210000u
#define GUEST_CLI       0x00211000u
#define GUEST_ARGS      0x00238000u
#define OSCODE_BASE     0x00240000u
#define OSCODE_END      0x00250000u
#define OSCODE_RAWDOFMT OSCODE_BASE
#define OSCODE_RETURN   (OSCODE_END - 2u)

/* Classic AmigaOS has a 34-byte MsgPort.  AROS-m68k's public Process layout
 * includes an eight-byte semaphore pad in MsgPort, so every Process field
 * after pr_MsgPort has a different generated offset.  The structures in this
 * arena are consumed by classic binaries and must therefore use the classic
 * offsets; using M68K_Process_pr_* here makes direct field reads disagree with
 * dos.library calls in particularly quiet and destructive ways. */
#define CLASSIC_PROCESS_SIZE       228u
#define CLASSIC_PR_SEGLIST         128u
#define CLASSIC_PR_STACKSIZE       132u
#define CLASSIC_PR_GLOBVEC         136u
#define CLASSIC_PR_TASKNUM         140u
#define CLASSIC_PR_STACKBASE       144u
#define CLASSIC_PR_RESULT2         148u
#define CLASSIC_PR_CURRENTDIR      152u
#define CLASSIC_PR_CIS             156u
#define CLASSIC_PR_COS             160u
#define CLASSIC_PR_CONSOLETASK     164u
#define CLASSIC_PR_FILESYSTEMTASK  168u
#define CLASSIC_PR_CLI             172u
#define CLASSIC_PR_RETURNADDR      176u
#define CLASSIC_PR_PKTWAIT         180u
#define CLASSIC_PR_WINDOWPTR       184u
#define CLASSIC_PR_HOMEDIR         188u
#define CLASSIC_PR_FLAGS           192u
#define CLASSIC_PR_EXITCODE        196u
#define CLASSIC_PR_EXITDATA        200u
#define CLASSIC_PR_ARGUMENTS       204u
#define CLASSIC_PR_LOCALVARS       208u
#define CLASSIC_PR_SHELLPRIVATE    220u
#define CLASSIC_PR_CES             224u

#define MP_SIGTASK         M68K_MsgPort_mp_SigTask
#define MP_SIGBIT          M68K_MsgPort_mp_SigBit
#define MP_MSGLIST         M68K_MsgPort_mp_MsgList_lh_Head
#define MN_REPLYPORT       M68K_Message_mn_ReplyPort
#define TASK_SIGRECVD_OFF  M68K_Task_tc_SigRecvd
#define LIB_VERSION_OFF    M68K_Library_lib_Version
#define LIB_REVISION_OFF   M68K_Library_lib_Revision
#define GUEST_LIB_VERSION  39
#define GUEST_LIB_REV      106
#define LVO_STACKSWAP      122
#define TASK_SIGALLOC_OFF  M68K_Task_tc_SigAlloc
#define TASK_SPREG_OFF     M68K_Task_tc_SPReg
#define TASK_SPLOWER_OFF   M68K_Task_tc_SPLower
#define TASK_SPUPPER_OFF   M68K_Task_tc_SPUpper
#define TASK_SIGEXCEPT_OFF M68K_Task_tc_SigExcept
#define TASK_TRAPALLOC_OFF M68K_Task_tc_UnionETask_tc_ETrap_tc_ETrapAlloc
#define TASK_ETASK_OFF     M68K_Task_tc_UnionETask_tc_ETrap_tc_ETrapAlloc
#define ETASK_TRAPALLOC_OFF M68K_ETask_et_TrapAlloc
#define TASK_EXCEPTDATA_OFF M68K_Task_tc_ExceptData
#define TASK_EXCEPTCODE_OFF M68K_Task_tc_ExceptCode
#define TF_ETASK_GUEST     (1u << 3)
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
#define AVL_LEFT_OFF       0u
#define AVL_RIGHT_OFF      4u
#define AVL_PARENT_OFF     8u

#define LVO_GL_INIT_DONE   650
#define LVO_GL_OPEN_DONE   651
#define LVO_GL_CLOSE_DONE  652
#define LVO_GL_RECLAIM     653

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
    uint32_t            open_base[GUESTLIB_OPENS_MAX];
    int                 open_count;
    uint32_t            closing_base;
    int                 parent;
    int                 reclaim_pending;
    uint32_t            saved_d[6];
    uint32_t            saved_a[5];
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

struct emu68k_ctx {
    j5d_engine           *eng;
    struct j5d_m68k_state st;
    uint32_t              task;
    uint32_t              entry;
    uint32_t              stack;
    uint32_t              stack_size;
    uint32_t              initial_sp;
    uint32_t              final_entry;
    /* AmigaDOS process startup ABI: A0 points at the command tail and D0 is
     * its byte count (normally including the terminating newline). Tasks
     * leave both zero; CreateNewProc children preserve NP_Arguments here. */
    uint32_t              argstr;
    uint32_t              argsize;
    /* Directory this context's program was loaded from, so PROGDIR: resolves
     * per program and not per native process (all contexts share one). Empty
     * for a context that is not a loaded program, such as a plain task. */
    char                  progdir[256];
    uint8_t               live;
    uint8_t               started;
    uint8_t               on_stack;
    uint8_t               finished;
    uint8_t               failed;
    uint8_t               blocked;
    uint16_t              forbid_depth;
    uint32_t              wait_mask;
    jmp_buf               unwind;
    uint8_t               can_unwind;
};

struct emu68k_run {
    void                 *reserve;
    uint8_t              *arena;
    unsigned long         arena_size;
    unsigned long         hole_mask;
    j4_sandbox            sb;
    /* The loader/bridge sandbox above exposes only the allocatable high arena.
     * Translated code also legitimately reads the separately mapped low page
     * (AbsExecBase at address 4), so the JIT sees the whole reserved guest span.
     * PROT_NONE holes still enforce hardware/unprovided-memory routing. */
    j5d_sandbox           jit_sb;
    j4_seglist            seg;
    /* BPTR of the program's own seglist. A program with no relocations finds
     * its later hunks by walking this chain from the link word before its
     * first hunk, so the framing has to exist even when nobody asked for it. */
    uint32_t              seg_bptr;
    stub_lib              lib;
    struct j5d_m68k_state st;
    j5d_engine           *eng;
    j5n_diag              diag;
    j5n_symtab            symtab;
    uint8_t              *image;
    unsigned long         imagelen;
    emu68k_sink_fn        sink;
    void                 *sink_user;
    long                  flushed;
    uint32_t              resume_pc;
    int                   started;
    /* A guest SystemTags(SYS_Asynch) child shares this run's 32-bit arena.
     * The root command may return while those processes are still alive, but
     * the arena cannot be freed underneath them.  Keep the process group
     * scheduled until its last child exits (or the run is explicitly killed). */
    int                   root_finished;
    uint32_t              root_d0;
    int                   done;
    volatile int          kill_req;
    double                deadline;
    uint32_t              last_ioerr;
    char                  name[PATH_MAX];
    struct { uint32_t base; char name[32]; } openlib[LIBBASE_MAX];
    int                   nlib;
    struct guestlib_live  guestlib[GUESTLIB_MAX];
    struct guestseg_live  guestseg[GUESTSEG_MAX];
    struct guestpool_live guestpool[GUESTPOOL_MAX];
    struct { uint32_t base; int lvo; uint32_t guest_fn; } patch[EMU68K_PATCH_MAX];
    /* Classic ARexx exposes rexxsyslib.library in RexxMsg.rm_LibBase. AROS
     * renamed that word rm_Private2 and Regina stores an interpreter-private
     * pointer there. Legacy applications still call vectors through the word,
     * so retain a per-run alias without rewriting Regina's shared message. */
    struct { uint32_t alias, target; } libalias[EMU68K_LIBALIAS_MAX];
    int                   nlibalias;
    struct {
        uint32_t port;
        uint32_t guest_name;
        uint8_t  live;
    } public_port[EMU68K_PUBLIC_PORT_MAX];
    uint32_t              int_vector[32];
    struct { uint32_t level, interrupt; } int_server[64];
    int                   nintserver;
    int                   active_loader;
    stub_lib             *run_lib;
    uint32_t              exec_heap;
    uint32_t              exec_heap_end;
    uint32_t              stack_lower;
    uint32_t              stack_upper;
    uint32_t              callback_stack_top[EMU68K_CALLBACK_DEPTH_MAX];
    uint8_t               callback_depth;
    jmp_buf               command_unwind;
    uint32_t              command_return;
    uint8_t               command_can_unwind;
    uint32_t              poll_quantum;
    /* Live classic input state.  Native Intuition can queue several edges in
     * one host-side pump, but a 68k task observes the corresponding CIA state
     * when it consumes each IDCMP message. */
    unsigned int          mouse_buttons;
    /* The raw all-library-call trace goes to Macaros's stderr capture rather
     * than Bridge Lab, so it needs its own per-run bound.  Otherwise a guest
     * tight loop can grow /tmp/aros-window.log without limit. */
    unsigned long         trace_calls_count;
    unsigned long         trace_calls_max;
    uint8_t               trace_calls_initialized;
    uint8_t               trace_calls_on;
    uint8_t               trace_calls_pending;
    uint8_t               trace_calls_truncated;
    int                   scheduler_ran;
    int                   failed;
    struct {
        uint32_t port;
        uint32_t mask;
        uint32_t identity;
        uint8_t  kind;
        uint8_t  live;
    } event_source[EMU68K_EVENT_MAX];
    struct { uint32_t control, fib; } exall[8];
    uint32_t              intuition_edit_hook;
    struct { uint32_t object, hook; } layer_hook[64];
    struct { uint32_t object, hook; } layerinfo_hook[16];
    /* task.resource state cannot be borrowed from the native process: its
     * Hook pointers and task-local IPTR values live in the 32-bit guest. */
    struct {
        uint32_t task;
        uint32_t type;
        uint32_t dispatcher;
        uint32_t hook[EMU68K_TASK_HOOKS_MAX];
        uint8_t  hook_count;
        uint8_t  flags;
        uint8_t  ran;
        uint8_t  live;
    } task_hook_type[EMU68K_TASK_HOOK_TYPES_MAX];
    struct {
        uint32_t task;
        uint32_t slot;
        uint32_t value;
        uint8_t  live;
    } task_storage[EMU68K_TASK_STORAGE_MAX];
    uint32_t              next_task_storage_slot;
    /* utility.library NamedObjects are guest-readable (no_Object is public)
     * and guest libraries use their user-space as shared state.  Native
     * opaque tokens therefore cannot represent this namespace. */
    struct {
        uint32_t object;
        uint32_t userspace;
        uint32_t user_size;
        uint32_t guest_name;
        uint32_t parent;
        int32_t  refs;
        int8_t   priority;
        uint8_t  flags;
        uint8_t  live;
        uint8_t  has_namespace;
        uint8_t  added;
        uint8_t  pending_remove;
        char     name[96];
    } named[128];
    struct emu68k_ctx      ctx[EMU68K_MAX_CTX];
    int                    nctx;
    int                    cur_ctx;
};

/* Runtime services used by the Exec handler. */
uint8_t emu68k_gread8(j4_sandbox *sb, uint32_t addr);
void emu68k_gwrite8(j4_sandbox *sb, uint32_t addr, uint8_t value);
uint32_t emu68k_gread16(j4_sandbox *sb, uint32_t addr);
void emu68k_gwrite16(j4_sandbox *sb, uint32_t addr, uint32_t value);
uint32_t emu68k_gread32(j4_sandbox *sb, uint32_t addr);
void emu68k_gwrite32(j4_sandbox *sb, uint32_t addr, uint32_t value);
uint32_t emu68k_guest_alloc(struct emu68k_run *run, uint32_t size);
uint32_t emu68k_guest_strdup(struct emu68k_run *run, const char *text, size_t size);
const char *emu68k_guest_cstr(j4_sandbox *sb, uint32_t addr);
uint32_t emu68k_native_facade_base(struct emu68k_run *run, const char *name,
                                   char *err, unsigned errlen);

#define gread8       emu68k_gread8
#define gwrite8      emu68k_gwrite8
#define gread16      emu68k_gread16
#define gwrite16     emu68k_gwrite16
#define gread32      emu68k_gread32
#define gwrite32     emu68k_gwrite32
#define guest_alloc  emu68k_guest_alloc
#define guest_strdup emu68k_guest_strdup
#define guest_cstr   emu68k_guest_cstr

extern emu68k_oscall_fn emu68k_oscall;
extern void *emu68k_oscall_user;

int emu68k_run_context_nested(struct emu68k_run *, j4_sandbox *, int,
                              char *, unsigned);
int emu68k_reschedule_siblings(struct emu68k_run *, j4_sandbox *,
                               const char *, uint32_t, char *, unsigned);
int emu68k_run_guest_subroutine(struct emu68k_run *, uint32_t,
                                struct j5d_m68k_state *, uint32_t,
                                uint32_t *, char *, unsigned);
int emu68k_run_guest_command(struct emu68k_run *, uint32_t,
                             struct j5d_m68k_state *, uint32_t,
                             uint32_t *, char *, unsigned);
int emu68k_add_guest_task_context(struct emu68k_run *, j4_sandbox *, uint32_t,
                                  uint32_t, uint32_t, uint32_t,
                                  struct j5d_m68k_state *, char *, unsigned);
int emu68k_create_guest_task(struct emu68k_run *, j4_sandbox *, uint32_t,
                             struct j5d_m68k_state *, char *, unsigned);
uint32_t emu68k_ctx_task(struct emu68k_run *);
uint32_t emu68k_callback_stack_acquire(struct emu68k_run *, char *, unsigned);
void emu68k_callback_stack_release(struct emu68k_run *);
int emu68k_route_guestside(const char *);
int emu68k_event_bind(struct emu68k_run *, unsigned, uint32_t, uint32_t,
                      uint32_t, uint32_t, const char *, uint32_t);
void emu68k_event_unbind_port(struct emu68k_run *, uint32_t, const char *);
int emu68k_event_pump(struct emu68k_run *, struct j5d_m68k_state *,
                      uint32_t, uint32_t, unsigned *);
void emu68k_trace_port_call(struct emu68k_run *, const char *,
                            struct j5d_m68k_state *, uint32_t);
void emu68k_ledger_record(int, const char *);
int emu68k_trace_calls_active(struct emu68k_run *);

int emu68k_find_guestlib_name(struct emu68k_run *, const char *);
int emu68k_find_guestlib_base(struct emu68k_run *, uint32_t);
int emu68k_register_libalias(struct emu68k_run *, uint32_t, uint32_t,
                             char *, unsigned);
int emu68k_load_guestlib(struct emu68k_run *, const char *, uint32_t, int *,
                         char *, unsigned);
int emu68k_open_guestlib_now(struct emu68k_run *, const char *, uint32_t,
                             uint32_t *, char *, unsigned);
void emu68k_guestlib_save_preserved(struct guestlib_live *,
                                    const struct j5d_m68k_state *);
int emu68k_guestlib_init_done(struct emu68k_run *, struct j5d_m68k_state *,
                              char *, unsigned);
int emu68k_guestlib_open_done(struct emu68k_run *, struct j5d_m68k_state *,
                              char *, unsigned);
int emu68k_guestlib_close_done(struct emu68k_run *, struct j5d_m68k_state *,
                               char *, unsigned);
int emu68k_guestlib_reclaim(struct emu68k_run *, struct j5d_m68k_state *,
                            char *, unsigned);

#define run_context_nested      emu68k_run_context_nested
#define run_guest_subroutine    emu68k_run_guest_subroutine
#define add_guest_task_context  emu68k_add_guest_task_context
#define create_guest_task       emu68k_create_guest_task
#define ctx_task                emu68k_ctx_task
#define route_guestside         emu68k_route_guestside
#define event_pump              emu68k_event_pump
#define trace_port_call         emu68k_trace_port_call
#define ledger_record           emu68k_ledger_record
#define find_guestlib_name      emu68k_find_guestlib_name
#define find_guestlib_base      emu68k_find_guestlib_base
#define load_guestlib           emu68k_load_guestlib
#define open_guestlib_now       emu68k_open_guestlib_now
#define guestlib_save_preserved emu68k_guestlib_save_preserved
#define guestlib_init_done      emu68k_guestlib_init_done
#define guestlib_open_done      emu68k_guestlib_open_done
#define guestlib_close_done     emu68k_guestlib_close_done
#define guestlib_reclaim        emu68k_guestlib_reclaim
#define g_oscall                emu68k_oscall
#define g_oscall_user           emu68k_oscall_user

int emu68k_exec_call(struct emu68k_run *run, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *state, char *error,
                     unsigned error_len);

int emu68k_taskresource_call(struct emu68k_run *, j4_sandbox *, int,
                             struct j5d_m68k_state *, char *, unsigned);
int emu68k_timerdevice_call(struct emu68k_run *, j4_sandbox *, int,
                            struct j5d_m68k_state *, char *, unsigned);

int emu68k_dos_call(struct emu68k_run *, j4_sandbox *, int,
                    struct j5d_m68k_state *, char *, unsigned);
int emu68k_dos_loadseg(struct emu68k_run *, j4_sandbox *,
                       struct j5d_m68k_state *, char *, unsigned);
int emu68k_dos_unloadseg(struct emu68k_run *, struct j5d_m68k_state *);
int emu68k_dos_readargs(struct emu68k_run *, j4_sandbox *,
                        struct j5d_m68k_state *, char *, unsigned);
int emu68k_dos_create_new_proc(struct emu68k_run *, j4_sandbox *,
                               struct j5d_m68k_state *, char *, unsigned);

int emu68k_utility_call(struct emu68k_run *, j4_sandbox *, int,
                        struct j5d_m68k_state *, char *, unsigned);
int emu68k_guest_format(j4_sandbox *, uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t *, char *, unsigned);
int emu68k_intuition_call(struct emu68k_run *, j4_sandbox *, int,
                          struct j5d_m68k_state *, char *, unsigned);
void emu68k_intuition_post_call(struct emu68k_run *, j4_sandbox *, int,
                                struct j5d_m68k_state *);
int emu68k_trace_tasks(void);
int emu68k_graphics_call(struct emu68k_run *, j4_sandbox *, int,
                         struct j5d_m68k_state *, char *, unsigned);
int emu68k_gadtools_call(struct emu68k_run *, j4_sandbox *, int,
                         struct j5d_m68k_state *, char *, unsigned);
int emu68k_layers_call(struct emu68k_run *, j4_sandbox *, int,
                       struct j5d_m68k_state *, char *, unsigned);
int emu68k_cybergraphics_call(struct emu68k_run *, j4_sandbox *, int,
                              struct j5d_m68k_state *, char *, unsigned);

#endif
