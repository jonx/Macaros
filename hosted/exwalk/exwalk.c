/*
 * ExWalk -- concurrent directory-walker stress for the emul-handler.
 *
 * Spawns WALKERS child processes; each one Lock()s DIR and walks it ROUNDS
 * times with Examine/ExNext (and, with the EXALL switch, every other round
 * with ExAll ED_COMMENT, which also drives the sidecar-metadata path -- the
 * deepest host-call frames in the handler). All walkers hammer the same
 * handler process with concurrent EXAMINE_NEXT/EXAMINE_ALL packet streams,
 * keeping it the current task for almost every scheduler tick, so hosted
 * interrupt delivery lands on its deepest stack frames.
 *
 * This reproduces the Feraille-metadata-walker fault: with the old 16 KB
 * emul-handler stack the run ends in "[KRN] Task <vol> went out of stack
 * limits" plus a wild bus-fault in DoExamineNext (walkers then hang or return
 * errors); with the fixed stack it completes cleanly.
 *
 * Usage:   ExWalk DIR <path on a host volume> [WALKERS n] [ROUNDS n] [EXALL]
 * Example: ExWalk SYS:C WALKERS 8 ROUNDS 25 EXALL
 *
 * Exit code: 0 = clean, 10 = walk errors, 20 = timeout (handler wedged/dead).
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/exall.h>

#define TEMPLATE   "DIR/A,WALKERS/N/K,ROUNDS/N/K,TIMEOUT/N/K,EXALL/S"
#define DEF_WALKERS 8
#define DEF_ROUNDS  25
#define DEF_TIMEOUT 120        /* seconds before declaring the handler wedged */
#define EXALL_BUF   4096

struct Ctl
{
    struct SignalSemaphore sem;
    STRPTR  dir;
    LONG    rounds;
    LONG    exall;
    LONG    done;
    LONG    entries;
    LONG    errors;
};

static struct Ctl g;    /* shared: children run our seglist in-process */

static LONG walk_exnext(BPTR lock, LONG *errors)
{
    struct FileInfoBlock *fib;
    LONG count = 0;

    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib)
    {
        (*errors)++;
        return 0;
    }

    if (Examine(lock, fib))
    {
        while (ExNext(lock, fib))
            count++;
        if (IoErr() != ERROR_NO_MORE_ENTRIES)
            (*errors)++;
    }
    else
        (*errors)++;

    FreeDosObject(DOS_FIB, fib);
    return count;
}

static LONG walk_exall(BPTR lock, LONG *errors)
{
    struct ExAllControl *eac;
    APTR buf;
    LONG count = 0;
    BOOL more;

    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (!eac)
    {
        (*errors)++;
        return 0;
    }
    buf = AllocMem(EXALL_BUF, MEMF_ANY);
    if (!buf)
    {
        FreeDosObject(DOS_EXALLCONTROL, eac);
        (*errors)++;
        return 0;
    }

    eac->eac_LastKey = 0;
    do
    {
        more = ExAll(lock, (struct ExAllData *)buf, EXALL_BUF, ED_COMMENT, eac);
        if (!more && IoErr() != ERROR_NO_MORE_ENTRIES)
        {
            (*errors)++;
            break;
        }
        if (eac->eac_Entries)
        {
            struct ExAllData *ead = (struct ExAllData *)buf;
            for (; ead; ead = ead->ed_Next)
                count++;
        }
    } while (more);

    FreeMem(buf, EXALL_BUF);
    FreeDosObject(DOS_EXALLCONTROL, eac);
    return count;
}

static LONG walker(void)
{
    LONG entries = 0, errors = 0, r;

    for (r = 0; r < g.rounds; r++)
    {
        BPTR lock = Lock(g.dir, SHARED_LOCK);

        if (!lock)
        {
            errors++;
            continue;
        }
        if (g.exall && (r & 1))
            entries += walk_exall(lock, &errors);
        else
            entries += walk_exnext(lock, &errors);
        UnLock(lock);
    }

    ObtainSemaphore(&g.sem);
    g.done++;
    g.entries += entries;
    g.errors += errors;
    ReleaseSemaphore(&g.sem);
    return 0;
}

int main(void)
{
    IPTR args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    LONG walkers = DEF_WALKERS, timeout = DEF_TIMEOUT;
    LONG started = 0, waited = 0, done, entries, errors;
    LONG i, rc;

    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "ExWalk");
        return RETURN_ERROR;
    }

    InitSemaphore(&g.sem);
    g.dir    = (STRPTR)args[0];
    g.rounds = args[2] ? *(LONG *)args[2] : DEF_ROUNDS;
    g.exall  = args[4] ? 1 : 0;
    if (args[1])
        walkers = *(LONG *)args[1];
    if (args[3])
        timeout = *(LONG *)args[3];
    if (walkers < 1)
        walkers = 1;
    if (g.rounds < 1)
        g.rounds = 1;

    /* Fail early on a bad directory rather than in every child. */
    {
        BPTR lock = Lock(g.dir, SHARED_LOCK);
        if (!lock)
        {
            PrintFault(IoErr(), g.dir);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        UnLock(lock);
    }

    Printf("ExWalk: %ld walkers x %ld rounds over \"%s\"%s\n",
           walkers, g.rounds, g.dir, g.exall ? " (ExNext+ExAll)" : " (ExNext)");

    for (i = 0; i < walkers; i++)
    {
        struct TagItem tags[] =
        {
            { NP_Entry,     (IPTR)walker    },
            { NP_Name,      (IPTR)"ExWalk walker" },
            { NP_StackSize, 65536           },
            { NP_Priority,  0               },
            { TAG_DONE,     0               }
        };

        if (CreateNewProc(tags))
            started++;
    }

    if (!started)
    {
        Printf("ExWalk: FAIL - could not start any walker\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    if (started < walkers)
        Printf("ExWalk: only %ld of %ld walkers started\n", started, walkers);

    /* Poll for completion; a dead/suspended handler never replies the walkers'
     * packets, so a timeout here means the handler is gone. */
    for (;;)
    {
        ObtainSemaphore(&g.sem);
        done = g.done;
        entries = g.entries;
        errors = g.errors;
        ReleaseSemaphore(&g.sem);

        if (done >= started)
            break;

        if (waited >= timeout * TICKS_PER_SECOND)
        {
            Printf("ExWalk: FAIL - timeout after %lds, %ld/%ld walkers done "
                   "(handler wedged or suspended; %ld walkers leaked)\n",
                   timeout, done, started, started - done);
            /* The stuck walkers still reference this binary; do not unload. */
            Flush(Output());
            Wait(SIGBREAKF_CTRL_C);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        Delay(10);
        waited += 10;

        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            Printf("ExWalk: break\n");
            FreeArgs(rda);
            return RETURN_WARN;
        }
    }

    rc = errors ? RETURN_ERROR : RETURN_OK;
    Printf("ExWalk: %s - %ld entries walked, %ld errors\n",
           errors ? "FAIL" : "PASS", entries, errors);

    FreeArgs(rda);
    return rc;
}
