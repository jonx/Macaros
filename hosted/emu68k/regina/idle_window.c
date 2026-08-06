/* The idle-guest responsiveness regression.
 *
 * A 68k program that opens a window and then does nothing but wait is the
 * ordinary shape of every classic GUI application between two user actions.
 * That state used to freeze the whole instance: the translated task idled by
 * sleeping the HOST thread, so AROS - which schedules cooperatively - never
 * ran `cocoa.hidd input` or `input.device`, and the very click the program was
 * waiting for could no longer be produced.
 *
 * So: open a window, announce readiness, go idle, and prove a click sent well
 * after going idle still arrives.  A test that passes without the fix would be
 * worthless, which is why it waits before the click rather than racing it.
 */
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>

#define RESULT "MacRW:Regina68k/idle.result"

struct IntuitionBase *IntuitionBase;

static void say(const char *text)
{
    BPTR fh = Open(RESULT, MODE_NEWFILE);
    if (!fh) return;
    Write(fh, (APTR)text, strlen(text));
    Close(fh);
}

int main(void)
{
    struct NewWindow nw;
    struct Window *win;
    ULONG signals;
    int done = 0;

    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary("intuition.library", 0);
    if (!IntuitionBase) {
        say("IDLE-FAIL no intuition.library\n");
        return 20;
    }

    memset(&nw, 0, sizeof nw);
    nw.LeftEdge = 60; nw.TopEdge = 40;
    nw.Width = 400;   nw.Height = 200;
    nw.DetailPen = 0; nw.BlockPen = 1;
    nw.IDCMPFlags = IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW | IDCMP_RAWKEY;
    nw.Flags = WFLG_DEPTHGADGET | WFLG_DRAGBAR | WFLG_CLOSEGADGET |
               WFLG_ACTIVATE;
    nw.Title = (UBYTE *)"idle";
    nw.Type = WBENCHSCREEN;

    win = OpenWindow(&nw);
    if (!win) {
        CloseLibrary((struct Library *)IntuitionBase);
        say("IDLE-FAIL no window\n");
        return 21;
    }

    /* Announced only once the window is really up, so the harness never
     * clicks before there is something to click on. */
    say("IDLE-READY\n");

    while (!done) {
        struct IntuiMessage *msg;

        /* The idle itself: the whole point of the test. */
        signals = Wait(1UL << win->UserPort->mp_SigBit);
        (void)signals;

        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
            ULONG class = msg->Class;
            ReplyMsg((struct Message *)msg);
            if (class == IDCMP_MOUSEBUTTONS) {
                say("IDLE-PASS click delivered after idling\n");
                done = 1;
            } else if (class == IDCMP_CLOSEWINDOW) {
                say("IDLE-PASS close delivered after idling\n");
                done = 1;
            }
        }
    }

    CloseWindow(win);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
