/* zed_aros_main.c -- C entry for hosted Zed editor-core on AROS.
 * Sets the argc/argv globals the Rust std args pal reads (same contract as
 * hosted/rust/rs3_main.c), runs the compiler-emitted static constructors
 * (INIT_ARRAY -- see below), then calls the staticlib entry on a large stack.
 *
 * We link startup.o + this main directly (like hosted/feraille), which runs
 * the autoinit chain (so the INIT_ARRAY symbol set that carries Rust's
 * `inventory` life-before-main constructors is invoked -- zed's settings and
 * action registries depend on it). We only claim the set handler here.
 *
 * Stack: the editor's init + gpui layout recurse far deeper than the few-KB
 * default CLI stack, which overflows into a SIGSEGV. collect-aros does not
 * emit a seglist stack cookie for hosted ELF, so a `__stack` global is not
 * honoured; instead we NewStackSwap onto a large heap stack ourselves.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <aros/symbolsets.h>

extern int zed_aros_main(void);

int aros_argc = 0;
char **aros_argv = 0;

/* Claim the INIT_ARRAY set so the linker gathers .init_array into SETNAME and
 * the autoinit startup runs it (without a handler it is silently skipped). */
THIS_PROGRAM_HANDLES_SYMBOLSET(INIT_ARRAY)
DECLARESET(INIT_ARRAY)

#define EDITOR_STACK (16UL * 1024 * 1024)

int main(int argc, char **argv)
{
    struct Process *self = (struct Process *)FindTask(NULL);

    /* The editor reports every path failure through its own error path, so a
     * modal dos requester can only get in the way -- and a Workbench start
     * lands in a current directory whose posix spelling dos cannot parse back,
     * which raises one before the window is even open. */
    self->pr_WindowPtr = (APTR)-1;

    aros_argc = argc;
    aros_argv = argv;

    /* Started from Workbench: argv is the WBStartup message, not a string
     * array (compiler/autoinit/fromwb.c), and argc is 0. Give the editor the
     * one argument it expects instead. */
    if (argc == 0) {
        static char *wb_argv[] = { "Zed", 0 };
        aros_argc = 1;
        aros_argv = wb_argv;
    }

    APTR lower = AllocMem(EDITOR_STACK, MEMF_ANY);
    if (!lower) {
        return zed_aros_main(); /* fall back to the CLI stack */
    }

    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    sss.stk_Lower = lower;
    sss.stk_Upper = (APTR)((UBYTE *)lower + EDITOR_STACK);
    sss.stk_Pointer = sss.stk_Upper;

    IPTR rc = NewStackSwap(&sss, (APTR)zed_aros_main, &ssa);

    FreeMem(lower, EDITOR_STACK);
    return (int)rc;
}
