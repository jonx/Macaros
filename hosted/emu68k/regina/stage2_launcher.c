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

    if (SystemTags("MacRW:Regina68k/commands/RexxMast", SYS_Asynch, TRUE, TAG_DONE) == -1 ||
        !wait_for_port("REXX")) {
        PutStr("STAGE2-FAIL REXX port did not appear\n");
        return 20;
    }

    if (SystemTags("MacRW:TurboCalc5/TurboCalc/TurboCalc", SYS_Asynch, TRUE, TAG_DONE) == -1 ||
        !wait_for_port("TCALC")) {
        PutStr("STAGE2-FAIL TCALC port did not appear\n");
        return 21;
    }

    /* TCALC publishes its public port before the GUI task has finished
     * opening its first sheet and entering its command loop.  Give that
     * task a bounded startup quantum before RX sends the first request;
     * otherwise the request can arrive during initialization and leave
     * the Rexx process waiting forever for a reply. */
    Delay(1000);

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
    for (unsigned turns = 0; turns < 6000u; turns++) {
        if (result_complete()) return 0;
        Delay(25);
    }
    PutStr("STAGE2-FAIL result file did not complete\n");
    return 23;
}
