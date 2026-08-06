/* Echo stage: RexxMast + the minimal ECHO host + RX in one emu68k arena.
 * Passing proves the whole guest ARexx transport with no real application
 * involved; see echo_host.c for what that isolates. */
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

    fh = Open("MacRW:Regina68k/echo.result", MODE_OLDFILE);
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
        PutStr("ECHO-FAIL REXX port did not appear\n");
        return 20;
    }

    if (SystemTags("MacRW:Regina68k/commands/echohost", SYS_Asynch, TRUE, TAG_DONE) == -1 ||
        !wait_for_port("ECHO")) {
        PutStr("ECHO-FAIL ECHO port did not appear\n");
        return 21;
    }

    result = SystemTags("MacRW:Regina68k/commands/RX MacRW:Regina68k/echo.rexx",
                        SYS_Asynch, TRUE, TAG_DONE);
    if (result == -1) {
        PutStr("ECHO-FAIL RX could not be launched\n");
        return 22;
    }
    for (unsigned turns = 0; turns < 2400u; turns++) {
        if (result_complete()) return 0;
        Delay(25);
    }
    PutStr("ECHO-FAIL result file did not complete\n");
    return 23;
}
