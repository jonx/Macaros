/* aros_moonstone_gfx.c -- the Moonstone present+input shim for AROS: open a window,
 * blit the game's 320x200 RGBA SoftwareCanvas to it each frame (cybergraphics
 * WritePixelArray), and read the keyboard (RAWKEY) into a held-state bitmask.
 *
 * This is the whole platform surface for the AROS port (the equivalent of the winit +
 * softbuffer file in bin/moonstone-desktop). Modelled on hosted/ffmpeg/ffview.c, which
 * already blits frames to an AROS window. -ffixed-x18 like the other glues.
 */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>

static struct Window *g_win;
/* held bits (keep in sync with moonstone-aros src/game.rs `mod bits`):
 * left1 right2 up4 down8 fire16 [quit32] endturn64 a128 d256 w512 s1024
 * x2048 tab4096 n8192 */
static int g_held;
static int g_quit;

int aros_ms_open(int w, int h)
{
    g_held = 0; g_quit = 0;
    g_win = OpenWindowTags(NULL,
        WA_Title,       (IPTR)"Moonstone",
        WA_InnerWidth,  (IPTR)w,
        WA_InnerHeight, (IPTR)h,
        WA_Left,        (IPTR)20, WA_Top, (IPTR)20,
        WA_Flags,       (IPTR)(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE),
        WA_IDCMP,       (IPTR)(IDCMP_CLOSEWINDOW | IDCMP_RAWKEY),
        TAG_DONE);
    return g_win ? 0 : -1;
}

void aros_ms_blit(const unsigned char *rgba, int w, int h)
{
    if (g_win)
        WritePixelArray((APTR)rgba, 0, 0, (UWORD)(w * 4), g_win->RPort,
                        g_win->BorderLeft, g_win->BorderTop, (UWORD)w, (UWORD)h,
                        RECTFMT_RGBA);
}

/* Drain pending window events into the held-key state; return the bitmask
 * (bit5 = quit: Esc or the close gadget). Non-blocking. */
int aros_ms_input(void)
{
    struct IntuiMessage *im;

    if (!g_win)
        return 32;

    while ((im = (struct IntuiMessage *)GetMsg(g_win->UserPort)))
    {
        ULONG cls = im->Class;
        UWORD code = im->Code;
        ReplyMsg((struct Message *)im);

        if (cls == IDCMP_CLOSEWINDOW)
            g_quit = 1;
        else if (cls == IDCMP_RAWKEY)
        {
            int down = !(code & 0x80);
            int key = code & 0x7f;
            int bit = 0;
            switch (key)
            {
                case 0x4F: bit = 1;  break;  /* cursor left  */
                case 0x4E: bit = 2;  break;  /* cursor right */
                case 0x4C: bit = 4;  break;  /* cursor up    */
                case 0x4D: bit = 8;  break;  /* cursor down  */
                case 0x40:                   /* space = fire */
                case 0x63: bit = 16; break;  /* ctrl doubles as fire */
                case 0x12: bit = 64; break;  /* E = end turn (map) */
                /* the letter set: P1 alias out of duels, P2 in them */
                case 0x20: bit = 128;  break; /* A */
                case 0x22: bit = 256;  break; /* D */
                case 0x11: bit = 512;  break; /* W */
                case 0x21: bit = 1024; break; /* S */
                case 0x32: bit = 2048; break; /* X */
                case 0x42: bit = 4096; break; /* Tab = P2 fire */
                case 0x36: bit = 8192; break; /* N = practice next foe */
                case 0x45: if (down) g_quit = 1; break; /* esc */
            }
            if (bit) { if (down) g_held |= bit; else g_held &= ~bit; }
        }
    }
    return g_held | (g_quit ? 32 : 0);
}

void aros_ms_delay(int ticks)   /* dos Delay: 50 ticks/sec (~20ms each) */
{
    if (ticks > 0)
        Delay((ULONG)ticks);
}

void aros_ms_close(void)
{
    if (g_win) { CloseWindow(g_win); g_win = NULL; }
}
