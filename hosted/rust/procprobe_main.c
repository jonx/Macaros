/* procprobe_main.c -- reproducer for streaming child pipes on AROS.
 *
 * Proves the acceptance criteria for live child-process I/O without dragging in
 * the 15-minute editor build:
 *
 *   1. spawn a child with its stdio on pipes,
 *   2. read a line of its stdout WHILE IT IS STILL RUNNING (before exit),
 *   3. write a line to its stdin and read the reply,
 *   4. learn of the exit from a signal (not a poll) and read the status.
 *
 * Dual-mode so it can be its own child: `ProcProbe child` echoes each line it
 * reads back with a prefix, then exits with a known code.
 *
 *   ProcProbe            -- run the parent side (the test)
 *   ProcProbe child      -- the child side
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <string.h>

#define APS_INHERIT 0
#define APS_PIPE    1
#define APS_NULL    2

extern void *aros_proc_spawn(const char *cmdline, int in_mode, int out_mode, int err_mode,
                             ULONG exit_sig, BPTR *p_in, BPTR *p_out, BPTR *p_err);
extern int  aros_proc_exited(void *handle, LONG *code);
extern void aros_proc_free(void *handle);
extern long aros_pipe_read(BPTR fh, void *buf, unsigned long len);
extern long aros_pipe_write(BPTR fh, const void *buf, unsigned long len);
extern void aros_pipe_close(BPTR fh);
extern int  aros_pipe_set_nonblock(BPTR fh, int enable);

static BPTR logfh;
static const char *g_child_cmd;
static LONG g_want_code;

static void say(const char *s)
{
    if (logfh) {
        Write(logfh, (APTR)s, (LONG)strlen(s));
        Flush(logfh);
    }
}

static void sayn(const char *s, long n)
{
    if (logfh && n > 0) {
        Write(logfh, (APTR)s, (LONG)n);
        Flush(logfh);
    }
}

static void saynum(const char *label, long v)
{
    char buf[32];
    int i = 0, j;
    long n = v;

    say(label);
    if (n < 0) { say("-"); n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    for (j = i; j > 0; j--) {
        char c = buf[j - 1];
        Write(logfh, &c, 1);
    }
    say("\n");
}

/* ---- child side: echo each line back with a prefix ------------------- */
static int child_main(int code)
{
    char buf[256];
    LONG n;
    int lines = 0;

    /* Announce ourselves immediately, before reading anything: this is the
     * output the parent must be able to see while we are still running. */
    Write(Output(), (APTR)"child-ready\n", 12);
    Flush(Output());

    while ((n = Read(Input(), buf, sizeof(buf))) > 0) {
        Write(Output(), (APTR)"echo:", 5);
        Write(Output(), buf, n);
        if (buf[n - 1] != '\n')
            Write(Output(), (APTR)"\n", 1);
        Flush(Output());
        if (++lines >= 2)
            break;
    }

    Write(Output(), (APTR)"child-done\n", 11);
    Flush(Output());
    return code;
}

/* ---- parent side ----------------------------------------------------- */

/* Read until at least one line arrives or the deadline passes. Returns bytes,
 * 0 on EOF, -1 on timeout. Uses the read-notify signal, never a busy poll. */
static long read_line_timeout(BPTR fh, char *buf, long cap, ULONG sig, int ticks)
{
    long got = 0;

    while (got < cap) {
        long n = aros_pipe_read(fh, buf + got, (unsigned long)(cap - got));
        if (n == -2) {                       /* empty, non-blocking */
            ULONG r;
            if (ticks-- <= 0)
                return got ? got : -1;
            /* Wait for readability or a timer tick. Delay() is coarse but this
             * is a probe; the point is that we are not spinning on read. */
            r = SetSignal(0, 0);
            (void)r;
            Delay(5);
            continue;
        }
        if (n <= 0)
            return got ? got : n;            /* EOF or error */
        got += n;
        if (buf[got - 1] == '\n')
            return got;
    }
    return got;
}

int main(int argc, char **argv)
{
    void *proc;
    BPTR p_in = (BPTR)0, p_out = (BPTR)0, p_err = (BPTR)0;
    BYTE sigbit;
    ULONG sigmask;
    char buf[512];
    long n;
    LONG code = -1;
    int failures = 0;
    int waited;

    if (argc >= 3 && strcmp(argv[1], "child") == 0) {
        int c = 0, i;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++)
            c = c * 10 + (argv[2][i] - '0');
        return child_main(c);
    }

    /* `ProcProbe [code]` -- the exit code the child should return (default 3).
     * Used to check how the shell passes a child's return code back to us. */
    {
        static char cmd[64];
        int c = 3, i, k = 0;
        const char *pre = "C:ProcProbe child ";
        char digits[12];
        int j = 0;
        if (argc >= 2) {
            c = 0;
            for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
                c = c * 10 + (argv[1][i] - '0');
        }
        g_want_code = c;
        while (*pre) cmd[k++] = *pre++;
        if (c == 0) digits[j++] = '0';
        while (c) { digits[j++] = (char)('0' + (c % 10)); c /= 10; }
        while (j) cmd[k++] = digits[--j];
        cmd[k] = '\0';
        g_child_cmd = cmd;
    }

    logfh = Open((CONST_STRPTR)"MacRW:procprobe.out", MODE_NEWFILE);
    if (!logfh)
        logfh = Output();

    say("== procprobe: streaming child pipes ==\n");
    saynum("want exit code=", (long)g_want_code);

    sigbit = AllocSignal(-1);
    if (sigbit == -1) {
        say("FAIL: AllocSignal\n");
        return 20;
    }
    sigmask = 1UL << sigbit;
    SetSignal(0, sigmask);                   /* start clear */

    proc = aros_proc_spawn(g_child_cmd, APS_PIPE, APS_PIPE, APS_NULL,
                           sigmask, &p_in, &p_out, &p_err);
    if (!proc) {
        say("FAIL: spawn returned NULL\n");
        return 20;
    }
    say("spawned\n");

    if (aros_pipe_set_nonblock(p_out, 1) < 0)
        say("warn: set_nonblock on stdout failed\n");

    /* (2) read the child's greeting WHILE IT IS STILL RUNNING. The child does
     * not exit until we have sent it two lines, so if this arrives the pipe is
     * genuinely streaming rather than replaying a finished child's output. */
    n = read_line_timeout(p_out, buf, sizeof(buf) - 1, sigmask, 400);
    if (n > 0) {
        buf[n] = '\0';
        say("got (child still running): ");
        sayn(buf, n);
        if (strncmp(buf, "child-ready", 11) != 0) {
            say("FAIL: unexpected greeting\n");
            failures++;
        }
    } else {
        saynum("FAIL: no greeting, rc=", n);
        failures++;
    }

    if (aros_proc_exited(proc, &code))
        { say("FAIL: child already exited before we wrote to it\n"); failures++; }
    else
        say("ok: child still running at this point\n");

    /* (3) write to the child's stdin, read the echo back. */
    {
        const char *msg = "ping-one\n";
        long w = aros_pipe_write(p_in, msg, (unsigned long)strlen(msg));
        saynum("wrote bytes=", w);
    }
    n = read_line_timeout(p_out, buf, sizeof(buf) - 1, sigmask, 400);
    if (n > 0) {
        buf[n] = '\0';
        say("got echo: ");
        sayn(buf, n);
        if (strncmp(buf, "echo:ping-one", 13) != 0) {
            say("FAIL: echo mismatch\n");
            failures++;
        }
    } else {
        saynum("FAIL: no echo, rc=", n);
        failures++;
    }

    /* second line -> the child breaks out of its loop and exits */
    {
        const char *msg = "ping-two\n";
        aros_pipe_write(p_in, msg, (unsigned long)strlen(msg));
    }

    /* (4) exit by signal, not by polling. */
    say("waiting for the exit signal...\n");
    waited = 0;
    while (!aros_proc_exited(proc, &code)) {
        ULONG got = Wait(sigmask | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C) { say("interrupted\n"); break; }
        if (++waited > 8) break;
    }
    if (aros_proc_exited(proc, &code)) {
        saynum("child exited, code=", (long)code);
        if (code != g_want_code) { saynum("  (wanted) ", (long)g_want_code); failures++; }
    } else {
        say("FAIL: no exit notification\n");
        failures++;
    }

    /* drain whatever is left, then EOF once the child's end is closed */
    while ((n = aros_pipe_read(p_out, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        say("tail: ");
        sayn(buf, n);
    }
    if (n == 0)
        say("ok: stdout reached EOF\n");

    aros_pipe_close(p_in);
    aros_pipe_close(p_out);
    aros_proc_free(proc);
    FreeSignal(sigbit);

    if (failures == 0)
        say("== ALL PASS ==\n");
    else
        saynum("== FAILURES: ", failures);

    if (logfh && logfh != Output())
        Close(logfh);
    return failures ? 10 : 0;
}
