/* sockprobe_main.c -- C entry for the async-stack socket reproducer.
 * Same startup contract as zed_aros_main.c (argc/argv globals, INIT_ARRAY
 * symbol set for life-before-main constructors, NewStackSwap onto a large
 * stack), but calls sockprobe_main. */

#include <proto/exec.h>
#include <exec/memory.h>
#include <aros/symbolsets.h>

extern int sockprobe_main(void);

int aros_argc = 0;
char **aros_argv = 0;

THIS_PROGRAM_HANDLES_SYMBOLSET(INIT_ARRAY)
DECLARESET(INIT_ARRAY)

#define PROBE_STACK (4UL * 1024 * 1024)

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;

    APTR lower = AllocMem(PROBE_STACK, MEMF_ANY);
    if (!lower) {
        return sockprobe_main();
    }
    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    sss.stk_Lower = lower;
    sss.stk_Upper = (APTR)((UBYTE *)lower + PROBE_STACK);
    sss.stk_Pointer = sss.stk_Upper;
    IPTR rc = NewStackSwap(&sss, (APTR)sockprobe_main, &ssa);
    FreeMem(lower, PROBE_STACK);
    return (int)rc;
}
