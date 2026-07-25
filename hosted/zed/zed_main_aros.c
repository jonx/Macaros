/* zed_main_aros.c -- C entry for the full `zed` crate on AROS.
 *
 * Same contract as zed_aros_main.c (the editor-core entry): set the argc/argv
 * globals the Rust std args pal reads, claim the INIT_ARRAY symbol set so the
 * compiler-emitted static constructors run (Rust's `inventory` registrations,
 * which zed's settings and action registries depend on), then call the
 * staticlib entry on a large stack.
 *
 * The entry itself is `zed_aros_main` in crates/zed/src/aros_entry.rs, which
 * calls zed's real `main`.
 */

#include <proto/exec.h>
#include <exec/memory.h>
#include <aros/symbolsets.h>

extern int zed_aros_main(void);

int aros_argc = 0;
char **aros_argv = 0;

THIS_PROGRAM_HANDLES_SYMBOLSET(INIT_ARRAY)
DECLARESET(INIT_ARRAY)

/* zed's startup (settings, themes, languages, workspace layout) recurses far
 * deeper than the few-KB default CLI stack. collect-aros emits no seglist stack
 * cookie for hosted ELF, so a `__stack` global is not honoured; swap onto a
 * large heap stack instead. */
#define ZED_STACK (16UL * 1024 * 1024)

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;

    APTR lower = AllocMem(ZED_STACK, MEMF_ANY);
    if (!lower) {
        return zed_aros_main(); /* fall back to the CLI stack */
    }

    struct StackSwapStruct sss;
    struct StackSwapArgs ssa;
    sss.stk_Lower = lower;
    sss.stk_Upper = (APTR)((UBYTE *)lower + ZED_STACK);
    sss.stk_Pointer = sss.stk_Upper;

    IPTR rc = NewStackSwap(&sss, (APTR)zed_aros_main, &ssa);

    FreeMem(lower, ZED_STACK);
    return (int)rc;
}
