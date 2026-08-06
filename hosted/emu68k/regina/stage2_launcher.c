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

    result = SystemTags("MacRW:Regina68k/commands/RX MacRW:Regina68k/stage2_turbocalc.rexx", TAG_DONE);
    if (result != 0) {
        PutStr("STAGE2-FAIL RX returned non-zero\n");
        return 22;
    }
    return 0;
}
