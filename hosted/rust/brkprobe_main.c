/* brkprobe_main.c -- C:BrkProbe, the terminal-interrupt check.
 *
 * The question: can the terminal stop a running command? The Amiga answer is
 * SIGBREAKF_CTRL_C to the shell's process (shell and command are one process,
 * and commands poll for break). The child is found again by CLI number, which
 * SystemTagList now actually stores through SYS_CliNumPtr (it was declared in
 * dostags.h but implemented nowhere until today's dos.library change).
 *
 * Shape: spawn an interactive shell on pipes (the editor terminal's exact
 * spawn), type `Wait 30` at it, break it two seconds later, and require the
 * shell to come back within a few seconds rather than thirty.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/types.h>

#include <stdio.h>
#include <string.h>

#define APS_PIPE 1

extern void *aros_proc_spawn(const char *cmdline, int in_mode, int out_mode,
                             int err_mode, int interactive, const char *cwd,
                             BPTR *p_in, BPTR *p_out, BPTR *p_err);
extern int   aros_proc_interrupt(void *handle);
extern int   aros_proc_exited(void *handle, LONG *code);
extern long  aros_pipe_read(BPTR fh, void *buf, unsigned long len);
extern long  aros_pipe_write(BPTR fh, const void *buf, unsigned long len);
extern int   aros_pipe_set_nonblock(BPTR fh, int enable);
extern void  aros_pipe_close(BPTR fh);

/* Drain whatever the shell has said so far into buf (nonblocking reads over
 * `ticks` fiftieths of a second). Returns bytes gathered. */
static int drain(BPTR out, char *buf, int cap, int ticks)
{
    int got = 0, t;
    long n;
    for (t = 0; t < ticks; t++) {
        Delay(5);
        for (;;) {
            n = aros_pipe_read(out, buf + got, (unsigned long)(cap - 1 - got));
            if (n <= 0)
                break;
            got += (int)n;
            if (got >= cap - 1)
                return got;
        }
    }
    buf[got] = '\0';
    return got;
}

int main(void)
{
    BPTR in = (BPTR)0, out = (BPTR)0, err = (BPTR)0;
    void *h;
    char buf[2048];
    int got, sent, waited;
    LONG code;

    h = aros_proc_spawn("", APS_PIPE, APS_PIPE, APS_PIPE, 1, (const char *)0,
                        &in, &out, &err);
    if (!h) {
        printf("[BRK] FAIL spawn\n");
        return 20;
    }
    aros_pipe_set_nonblock(out, 1);

    /* Let the banner and prompt arrive, then start something long. */
    got = drain(out, buf, sizeof(buf), 10);
    printf("[BRK] banner: %d bytes\n", got);

    aros_pipe_write(in, "Wait 30\n", 8);

    /* Give the shell time to read the line and start Wait. */
    Delay(100);

    sent = aros_proc_interrupt(h);
    printf("[BRK] interrupt sent: %s\n", sent ? "yes" : "NO");

    /* A broken Wait returns immediately; the shell echoes ***Break and
     * prompts again. Allow 4 seconds, against the 28 still owed. */
    got = drain(out, buf, sizeof(buf), 40);
    printf("[BRK] after break: %d bytes: \"%s\"\n", got, buf);

    /* Wind the shell up and give it a moment to exit. */
    aros_pipe_write(in, "EndShell\n", 9);
    for (waited = 0; waited < 50; waited++) {
        Delay(10);
        if (aros_proc_exited(h, &code))
            break;
    }

    aros_pipe_close(in);
    aros_pipe_close(out);
    aros_pipe_close(err);

    if (!sent) {
        printf("BRKPROBE FAIL (no interrupt target)\n");
        return 20;
    }
    if (got > 0 && (strstr(buf, "Break") || strstr(buf, ">"))) {
        printf("BRKPROBE PASS\n");
        return 0;
    }
    printf("BRKPROBE FAIL (Wait was not interrupted)\n");
    return 10;
}
