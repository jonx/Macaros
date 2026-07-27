/* aros_proc_glue.c -- streaming child processes for the Rust std::process pal.
 *
 * The sibling aros_process_glue.c runs a command to completion and captures its
 * output into temp files. That cannot stream: you cannot read a child's stdout
 * while it is still running, nor write to its stdin. This glue does, which is
 * what a language server over stdio and the integrated terminal both need.
 *
 * Shape:
 *   - Each piped stream is a `PIPE:` pipe opened twice, once for each end. The
 *     handler replies to an open immediately (it does not wait for a peer) and
 *     allows one reader plus one writer, so the parent can open both ends up
 *     front without deadlocking.
 *   - The child is started with `SystemTagList(..., SYS_Asynch, TRUE)` so the
 *     shell still parses the command line (searching C: and friends) and the
 *     call returns while the child runs. The child owns the ends it was given
 *     and closes them on exit, which is what gives the parent EOF.
 *   - Exit is reported through `NP_ExitCode`/`NP_ExitData`, which SystemTagList
 *     passes through to CreateNewProc unfiltered. The hook runs in the dying
 *     child with its return code, stores it, and signals the spawning task, so
 *     the parent learns of the exit from a signal rather than by polling.
 *
 * Readiness composes with sockets: the pipe handler's ACTION_PIPE_READ_NOTIFY
 * signals a task when a pipe becomes readable, and the exit signal is an
 * ordinary exec signal, so one WaitSelect can wait on a socket, a child pipe
 * and a child exit together.
 *
 * Compiled with -ffixed-x18 like the other glues.
 *
 * Independent work: from the AROS dos.library autodocs (SystemTagList,
 * CreateNewProc) and the pipe-handler headers in the tree.
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <exec/memory.h>

/* pipe-handler.h private actions (workbench/fs/pipe/pipe-handler.h). */
#define ACTION_PIPE_READ_NOTIFY   0x50524E31L   /* 'PRN1' */
#define ACTION_PIPE_SET_NONBLOCK  0x50534E42L   /* 'PSNB' */
#define ERROR_PIPE_WOULD_BLOCK    0x50574F42L   /* 'PWOB' */

/* Stdio dispositions, matching the Rust side. */
#define APS_INHERIT 0
#define APS_PIPE    1
#define APS_NULL    2

struct AProc {
    /* Who to signal on exit, and with what. Only ever set while that task is
     * inside wait(): a task that has finished waiting may be gone by the time
     * the child dies, and signalling a dead Task faults. Guarded by Forbid(). */
    struct Task *waiter;
    ULONG        waiter_sig;
    volatile LONG exited;
    volatile LONG code;
};

/* Runs in the dying child, before its cleanup. `result` is the process's return
 * value -- but for a shell that is only RETURN_OK/RETURN_FAIL: AROS's Shell ends
 * with `return error ? RETURN_FAIL : RETURN_OK`, collapsing every failure to 20.
 * The command's real code is kept in cli_ReturnCode, and pr_CLI is still valid
 * here, so prefer it and fall back to `result` for a non-CLI process. */
static void proc_exit_hook(IPTR result, IPTR data)
{
    struct AProc *p = (struct AProc *)data;
    struct Process *me = (struct Process *)FindTask((CONST_STRPTR)0);
    struct CommandLineInterface *cli;

    if (!p)
        return;
    p->code = (LONG)result;
    if (me && me->pr_CLI) {
        cli = (struct CommandLineInterface *)BADDR(me->pr_CLI);
        if (cli)
            p->code = cli->cli_ReturnCode;
    }
    p->exited = 1;

    /* Forbid() pairs with aros_proc_set_waiter: the waiter cannot deregister
     * (and then exit) between our read and the Signal. */
    Forbid();
    if (p->waiter && p->waiter_sig)
        Signal(p->waiter, p->waiter_sig);
    Permit();
}

/* Register (task != NULL) or clear (task == NULL) the task to wake on exit.
 * Returns 1 if the child had already exited, so the caller does not wait. */
int aros_proc_set_waiter(void *handle, void *task, ULONG sigmask)
{
    struct AProc *p = (struct AProc *)handle;
    int already;

    if (!p)
        return 1;
    Forbid();
    p->waiter = (struct Task *)task;
    p->waiter_sig = task ? sigmask : 0;
    already = p->exited ? 1 : 0;
    Permit();
    return already;
}

void *aros_task_self(void)
{
    return (void *)FindTask((CONST_STRPTR)0);
}

/* Unique-per-call pipe names. Only ever touched under Forbid(). */
static ULONG proc_seq;

static ULONG next_seq(void)
{
    ULONG n;
    Forbid();
    n = ++proc_seq;
    Permit();
    return n;
}

/* Which Open failed: 1 = the MODE_NEWFILE end, 2 = the MODE_OLDFILE end. */
static volatile LONG g_pipe_step = 0;

/* Open both ends of one pipe. `parent_writes` picks which end is whose:
 * the child's stdin is written by us, its stdout/stderr are read by us. */
static int open_pipe_pair(ULONG seq, const char *which, int parent_writes,
                          BPTR *parent_end, BPTR *child_end)
{
    char name[64];
    ULONG n = seq;
    int i = 0, j;
    char digits[12];

    /* "PIPE:rustN-<which>" without sprintf (no stdio in this glue). */
    const char *p = "PIPE:rust";
    while (*p) name[i++] = *p++;
    j = 0;
    if (n == 0) digits[j++] = '0';
    while (n) { digits[j++] = (char)('0' + (n % 10)); n /= 10; }
    while (j) name[i++] = digits[--j];
    name[i++] = '-';
    p = which;
    while (*p) name[i++] = *p++;
    name[i] = '\0';

    /* The writer opens MODE_NEWFILE, the reader MODE_OLDFILE. Open the writer
     * first: the handler creates the pipe on either open, and a reader on a
     * pipe with no writer would see EOF rather than block. */
    if (parent_writes) {
        *parent_end = Open((CONST_STRPTR)name, MODE_NEWFILE);
        if (!*parent_end) { g_pipe_step = 1; return 0; }
        *child_end = Open((CONST_STRPTR)name, MODE_OLDFILE);
        if (!*child_end) { g_pipe_step = 2; Close(*parent_end); *parent_end = (BPTR)0; return 0; }
    } else {
        *child_end = Open((CONST_STRPTR)name, MODE_NEWFILE);
        if (!*child_end) { g_pipe_step = 1; return 0; }
        *parent_end = Open((CONST_STRPTR)name, MODE_OLDFILE);
        if (!*parent_end) { g_pipe_step = 2; Close(*child_end); *child_end = (BPTR)0; return 0; }
    }
    return 1;
}

/* Why the last spawn failed, so a caller can say more than "it did not run".
 * See APS_FAIL_* in the pal. */
static volatile LONG g_last_fail = 0;
static volatile LONG g_last_ioerr = 0;

LONG aros_proc_last_fail(LONG *ioerr, LONG *step)
{
    if (ioerr)
        *ioerr = g_last_ioerr;
    if (step)
        *step = g_pipe_step;
    return g_last_fail;
}

/* Spawn `cmdline`, wiring each stream per its disposition.
 *
 * `interactive` picks which kind of shell. A background one runs a single
 * command line and exits, which is what running a command means. A foreground
 * one is a new CLI: it keeps reading its input, which is what a terminal is.
 * SystemTagList's default is the background kind.
 *
 * `cwd` (NULL for none) is where the child starts. Passing it as the process's
 * directory rather than a `CD` command matters for an interactive shell: a
 * command would be read, run and prompted for like anything the user typed.
 *
 * Nothing is signalled until a task registers itself with
 * aros_proc_set_waiter, which it does only while it is actually waiting.
 *
 * Returns an opaque handle, or NULL if the child could not be started. The
 * parent ends of any piped streams land in *p_in / *p_out / *p_err as BPTRs
 * (0 when that stream is not piped).
 */
void *aros_proc_spawn(const char *cmdline, int in_mode, int out_mode, int err_mode,
                      int interactive, const char *cwd,
                      BPTR *p_in, BPTR *p_out, BPTR *p_err)
{
    struct AProc *p;
    struct TagItem tags[10];
    BPTR dirlock = (BPTR)0;
    BPTR c_in = (BPTR)0, c_out = (BPTR)0, c_err = (BPTR)0;
    BPTR nil_in = (BPTR)0, nil_out = (BPTR)0, nil_err = (BPTR)0;
    ULONG seq;
    int nt = 0;
    LONG rc;
    struct Process *me;
    APTR oldwin;

    if (!cmdline)
        return (void *)0;

    g_last_fail = 0; g_last_ioerr = 0; g_pipe_step = 0;
    *p_in = (BPTR)0; *p_out = (BPTR)0; *p_err = (BPTR)0;

    p = AllocMem(sizeof(struct AProc), MEMF_ANY | MEMF_CLEAR);
    if (!p) {
        g_last_fail = 1;
        return (void *)0;
    }
    seq = next_seq();

    /* No requesters while we open pipes: a missing PIPE: mount must fail
     * immediately rather than pop a blocking "please insert volume" box. */
    me = (struct Process *)FindTask((CONST_STRPTR)0);
    oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    if (in_mode == APS_PIPE) {
        if (!open_pipe_pair(seq, "in", 1, p_in, &c_in)) { g_last_fail = 2; goto fail; }
    } else if (in_mode == APS_NULL) {
        c_in = nil_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
        if (!c_in) { g_last_fail = 5; goto fail; }
    }
    if (out_mode == APS_PIPE) {
        if (!open_pipe_pair(seq, "out", 0, p_out, &c_out)) { g_last_fail = 3; goto fail; }
    } else if (out_mode == APS_NULL) {
        c_out = nil_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        if (!c_out) { g_last_fail = 5; goto fail; }
    }
    if (err_mode == APS_PIPE) {
        if (!open_pipe_pair(seq, "err", 0, p_err, &c_err)) { g_last_fail = 4; goto fail; }
    } else if (err_mode == APS_NULL) {
        c_err = nil_err = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        if (!c_err) { g_last_fail = 5; goto fail; }
    }

    if (cwd && *cwd) {
        dirlock = Lock((CONST_STRPTR)cwd, SHARED_LOCK);
        if (!dirlock) { g_last_fail = 8; goto fail; }
    }

    me->pr_WindowPtr = oldwin;

    if (dirlock) { tags[nt].ti_Tag = NP_CurrentDir; tags[nt].ti_Data = (IPTR)dirlock; nt++; }
    if (c_in)  { tags[nt].ti_Tag = SYS_Input;  tags[nt].ti_Data = (IPTR)c_in;  nt++; }
    if (c_out) { tags[nt].ti_Tag = SYS_Output; tags[nt].ti_Data = (IPTR)c_out; nt++; }
    if (c_err) { tags[nt].ti_Tag = SYS_Error;  tags[nt].ti_Data = (IPTR)c_err; nt++; }
    tags[nt].ti_Tag = SYS_Asynch;  tags[nt].ti_Data = (IPTR)TRUE;        nt++;
    if (interactive) {
        tags[nt].ti_Tag = SYS_Background; tags[nt].ti_Data = (IPTR)FALSE;  nt++;
    }
    tags[nt].ti_Tag = NP_ExitCode; tags[nt].ti_Data = (IPTR)proc_exit_hook; nt++;
    tags[nt].ti_Tag = NP_ExitData; tags[nt].ti_Data = (IPTR)p;           nt++;
    tags[nt].ti_Tag = TAG_DONE;    tags[nt].ti_Data = 0;

    rc = SystemTagList((CONST_STRPTR)cmdline, tags);
    if (rc == -1) {
        g_last_fail = 6;
        g_last_ioerr = IoErr();
        /* The shell never started: SYS_Asynch never took ownership of the
         * child ends, so they are still ours to close. */
        if (c_in) Close(c_in);
        if (c_out) Close(c_out);
        if (c_err) Close(c_err);
        /* The shell never started, so it never took the directory either. */
        if (dirlock) { UnLock(dirlock); dirlock = (BPTR)0; }
        goto fail_after_win;
    }

    /* From here the child owns c_in/c_out/c_err and the directory lock, and
     * releases them when it exits. */
    return (void *)p;

fail:
    if (!g_last_fail) g_last_fail = 7;
    g_last_ioerr = IoErr();
    me->pr_WindowPtr = oldwin;
    if (dirlock) UnLock(dirlock);
    if (c_in) Close(c_in);
    if (c_out) Close(c_out);
    if (c_err) Close(c_err);
fail_after_win:
    if (*p_in) { Close(*p_in); *p_in = (BPTR)0; }
    if (*p_out) { Close(*p_out); *p_out = (BPTR)0; }
    if (*p_err) { Close(*p_err); *p_err = (BPTR)0; }
    FreeMem(p, sizeof(struct AProc));
    return (void *)0;
}

/* 1 and *code set once the child has exited, 0 while it is still running. */
int aros_proc_exited(void *handle, LONG *code)
{
    struct AProc *p = (struct AProc *)handle;

    if (!p)
        return 0;
    if (!p->exited)
        return 0;
    if (code)
        *code = p->code;
    return 1;
}

/* Release the handle. Only safe once the child has exited: the exit hook
 * writes through this pointer. */
void aros_proc_free(void *handle)
{
    if (handle)
        FreeMem(handle, sizeof(struct AProc));
}

/*--------------------------------------------------------------------------
 * Pipe endpoint I/O. These are dos BPTRs, not posixc fds, so the Rust side
 * drives them through this glue rather than through the fs pal.
 *------------------------------------------------------------------------*/

/* >=0 bytes read (0 = EOF), -1 on error, -2 when a non-blocking pipe is empty. */
long aros_pipe_read(BPTR fh, void *buf, unsigned long len)
{
    LONG n = Read(fh, buf, (LONG)len);

    if (n < 0 && IoErr() == ERROR_PIPE_WOULD_BLOCK)
        return -2;
    return (long)n;
}

long aros_pipe_write(BPTR fh, const void *buf, unsigned long len)
{
    return (long)Write(fh, (APTR)buf, (LONG)len);
}

void aros_pipe_close(BPTR fh)
{
    if (fh)
        Close(fh);
}

/* Drive the handler's private actions on this pipe. The key is fh_Arg1 and the
 * handler's port is fh_Type, per pipe-handler.h. */
static struct FileHandle *fh_of(BPTR fh)
{
    return (struct FileHandle *)BADDR(fh);
}

int aros_pipe_set_nonblock(BPTR fh, int enable)
{
    struct FileHandle *h = fh_of(fh);

    if (!h || !h->fh_Type)
        return -1;
    return (int)DoPkt(h->fh_Type, ACTION_PIPE_SET_NONBLOCK, h->fh_Arg1,
                      (IPTR)enable, 0, 0, 0);
}

/* Signal `task` with `sigmask` whenever the pipe is readable (and at once if it
 * already is). Pass sigmask 0 to clear the registration. */
int aros_pipe_read_notify(BPTR fh, ULONG sigmask, void *task)
{
    struct FileHandle *h = fh_of(fh);

    if (!h || !h->fh_Type)
        return -1;
    return (int)DoPkt(h->fh_Type, ACTION_PIPE_READ_NOTIFY, h->fh_Arg1,
                      (IPTR)sigmask, (IPTR)task, 0, 0);
}

/*--------------------------------------------------------------------------
 * Exec signal plumbing, so a parent can block on a child's exit instead of
 * polling. The signal is allocated by the task that will wait on it.
 *------------------------------------------------------------------------*/

int aros_sig_alloc(void)
{
    BYTE bit = AllocSignal(-1);

    if (bit == -1)
        return -1;
    SetSignal(0, 1UL << bit);        /* start clear */
    return (int)bit;
}

void aros_sig_free(int bit)
{
    if (bit >= 0)
        FreeSignal((BYTE)bit);
}

ULONG aros_sig_wait(ULONG mask)
{
    return Wait(mask);
}
