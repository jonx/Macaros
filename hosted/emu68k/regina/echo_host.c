/* Minimal ARexx host: the smallest program that speaks the public-port
 * command protocol TurboCalc implements.  It exists to split one question in
 * two: if RX -> ECHO -> reply -> result file works here, the entire ARexx
 * transport (RexxMast, RX, rexxsyslib, port publish/find, RexxMsg layout,
 * reply routing) is proven inside one emu68k arena, and an application that
 * still stalls does so in its own dispatch, not in the bridge. */
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/rexxsyslib.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <string.h>

struct RxsLib *RexxSysBase = NULL;

static int str_eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

int main(void)
{
    struct MsgPort *port;
    int quit = 0;

    RexxSysBase = (struct RxsLib *)OpenLibrary("rexxsyslib.library", 0);
    if (!RexxSysBase) {
        PutStr("ECHO-HOST-FAIL rexxsyslib\n");
        return 20;
    }

    port = CreatePort("ECHO", 0);
    if (!port) {
        CloseLibrary((struct Library *)RexxSysBase);
        PutStr("ECHO-HOST-FAIL port\n");
        return 21;
    }

    while (!quit) {
        struct RexxMsg *rm;

        WaitPort(port);
        while ((rm = (struct RexxMsg *)GetMsg(port)) != NULL) {
            const char *arg = (const char *)rm->rm_Args[0];

            rm->rm_Result1 = RC_OK;
            rm->rm_Result2 = 0;
            if (arg && str_eq_nocase(arg, "quit")) {
                quit = 1;
            } else if (arg && (rm->rm_Action & RXFF_RESULT)) {
                char buf[128];
                unsigned n = 0;
                const char *p = "PONG:";
                while (*p && n < sizeof(buf) - 1) buf[n++] = *p++;
                p = arg;
                while (*p && n < sizeof(buf) - 1) buf[n++] = *p++;
                buf[n] = 0;
                rm->rm_Result2 = (IPTR)CreateArgstring(buf, n);
            }
            ReplyMsg((struct Message *)rm);
        }
    }

    /* Drain anything queued between quit and teardown, then vanish. */
    {
        struct RexxMsg *rm;
        while ((rm = (struct RexxMsg *)GetMsg(port)) != NULL) {
            rm->rm_Result1 = RC_OK;
            rm->rm_Result2 = 0;
            ReplyMsg((struct Message *)rm);
        }
    }
    DeletePort(port);
    CloseLibrary((struct Library *)RexxSysBase);
    return 0;
}
