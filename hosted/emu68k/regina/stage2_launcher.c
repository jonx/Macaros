/* Stage 2: put a real 68k application, RexxMast and RX in one emu68k run.
 *
 * Public port addresses are meaningful only inside their shared 32-bit guest
 * arena.  Starting all three commands through guest dos.library gives the
 * deterministic ARexx script the same single-system view a classic Amiga has.
 */
#include <exec/ports.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

static int wait_for_port(const char *name)
{
    unsigned turns;

    for (turns = 0; turns < 200000u; turns++) {
        struct MsgPort *port;
        Forbid();
        port = FindPort(name);
        Permit();
        if (port) return 1;
        Reschedule();
    }
    return 0;
}

static int result_complete(void)
{
    BPTR fh;
    char buf[256];
    LONG n;

    fh = Open("MacRW:Regina68k/stage2.result", MODE_OLDFILE);
    if (!fh) return 0;
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "PASS") != 0 || strstr(buf, "FAIL") != 0;
}

int main(void)
{
    LONG result;

    /* Each launch owns one result contract.  Leaving an older PASS in place
     * lets the launcher return before the new RX process has executed, while
     * positional LINEOUT writes can leave stale trailing diagnostics. */
    DeleteFile("MacRW:Regina68k/stage2.result");

    if (SystemTags("MacRW:Regina68k/commands/RexxMast", SYS_Asynch, TRUE, TAG_DONE) == -1 ||
        !wait_for_port("REXX")) {
        PutStr("STAGE2-FAIL REXX port did not appear\n");
        return 20;
    }

    /* Launch the application from its OWN directory, the way a user does.
     * A classic application reads its settings, catalogs and startup assets
     * relative to the current directory; started from elsewhere it silently
     * takes a different startup path. */
    {
        BPTR dir = Lock("MacRW:TurboCalc5/TurboCalc", SHARED_LOCK);
        BPTR previous = dir ? CurrentDir(dir) : (BPTR)0;
        LONG started = SystemTags("MacRW:TurboCalc5/TurboCalc/TurboCalc",
                                  SYS_Asynch, TRUE, TAG_DONE);
        if (dir) {
            CurrentDir(previous);
            UnLock(dir);
        } else {
            PutStr("STAGE2-FAIL TurboCalc directory could not be locked\n");
            return 21;
        }
        if (started == -1 || !wait_for_port("TCALC")) {
            PutStr("STAGE2-FAIL TCALC port did not appear\n");
            return 21;
        }
    }

    /* TCALC publishes its public port before the GUI task has finished
     * opening its first sheet and entering its command loop.  Wait for the
     * worker's private port as the stronger readiness boundary, then give the
     * application a bounded startup quantum before RX sends the first request.
     * Otherwise the request can arrive during initialization and leave the
     * Rexx process waiting forever for a reply. */
    /* No readiness delay.  An ARexx message QUEUES on the public port and is
     * served whenever the application next polls it, so sleeping before
     * sending buys nothing; it only hides whether the application serves the
     * port at all.  Measured: a request sent during startup stayed queued for
     * ~295,000 further scheduler events after the application had fully
     * started, and was never polled.  The one real precondition is that the
     * port exists, which is waited for above. */

    /* RX is a real guest process.  Do not make the launcher sit in a
     * synchronous RunCommand() while the script is driving TCALC: that leaves
     * the top-level task in the host event-pump path and can starve the app's
     * command task during its late GUI initialization.  Keep this process
     * alive at a scheduler wait point until the script's result-file contract
     * reaches PASS or FAIL; returning immediately would let a one-shot hosted
     * command tear down its still-running child contexts. */
    result = SystemTags("MacRW:Regina68k/commands/RX MacRW:Regina68k/stage2_turbocalc.rexx",
                        SYS_Asynch, TRUE, TAG_DONE);
    if (result == -1) {
        PutStr("STAGE2-FAIL RX could not be launched\n");
        return 22;
    }
    /* 600 * 25 ticks at the Amiga 50 Hz DOS clock = five minutes. */
    for (unsigned turns = 0; turns < 600u; turns++) {
        if (result_complete()) return 0;
        Delay(25);
    }
    PutStr("STAGE2-FAIL result file did not complete\n");
    return 23;
}
