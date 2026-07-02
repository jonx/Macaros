/* aros_process_glue.c -- run a shell command line for the Rust std::process pal.
 *
 * AROS has no fork/exec: dos `SystemTagList` runs a command *line* (the shell parses
 * it, searching C: etc.) and returns the command's return code. `SYS_Output`/
 * `SYS_Error` redirect the child's stdout/stderr to file handles we open here, which
 * is how std::process::Command::output() captures output (into T: temp files the Rust
 * side then reads back). With out_path/err_path NULL the child inherits the caller's
 * streams (the status() case).
 *
 * Header-clean isn't needed here: <proto/dos.h> + <dos/dostags.h> are AROS headers,
 * no macOS SDK. Compiled with -ffixed-x18 like the other glues.
 *
 * Independent work: from the AROS dos.library autodocs (System/SystemTagList).
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>

/* Returns the command's return code, or -1 if the shell couldn't run it.
 * A requested capture redirection that cannot be set up is also -1: silently
 * running the child with INHERITED streams would hand the caller an empty
 * "captured" output indistinguishable from the child printing nothing.
 *
 * We suppress DOS requesters for the duration (pr_WindowPtr = -1), so a missing temp
 * volume/assign makes Open() fail immediately instead of popping a blocking
 * "please insert volume ..." requester (that once hung the whole run). */
long aros_system(const char *cmdline, const char *out_path, const char *err_path)
{
    BPTR out = (BPTR)0, err = (BPTR)0;
    struct TagItem tags[3];
    int nt = 0;
    long rc;
    struct Process *me;
    APTR oldwin;

    if (!cmdline)
        return -1;

    me = (struct Process *)FindTask(NULL);
    oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;         /* no requesters while we probe temp files */

    if (out_path) {
        out = Open((CONST_STRPTR)out_path, MODE_NEWFILE);
        if (!out) goto capture_failed;
        tags[nt].ti_Tag = SYS_Output; tags[nt].ti_Data = (IPTR)out; nt++;
    }
    if (err_path) {
        err = Open((CONST_STRPTR)err_path, MODE_NEWFILE);
        if (!err) goto capture_failed;
        tags[nt].ti_Tag = SYS_Error; tags[nt].ti_Data = (IPTR)err; nt++;
    }

    me->pr_WindowPtr = oldwin;

    tags[nt].ti_Tag = TAG_DONE;
    tags[nt].ti_Data = 0;

    rc = SystemTagList((CONST_STRPTR)cmdline, tags);

    /* synchronous (no SYS_Asynch), so the handles are still ours to close */
    if (out) Close(out);
    if (err) Close(err);
    return rc;

capture_failed:
    me->pr_WindowPtr = oldwin;
    if (out) Close(out);
    if (err) Close(err);
    return -1;
}
