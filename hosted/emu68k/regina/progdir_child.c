/* The child half of the PROGDIR: regression.
 *
 * PROGDIR: means "the drawer THIS program was loaded from".  Every guest
 * context of a run shares one native process, so without per-program tracking
 * a program started by another program resolves PROGDIR: to its LAUNCHER's
 * drawer and silently reads the wrong files - or none.  This program lives in
 * its own directory next to its own data file and does nothing but prove it
 * can find it.
 */
#include <proto/dos.h>
#include <string.h>

#define RESULT "MacRW:Regina68k/progdir.result"
#define WANT   "progdir-marker"

static void say(const char *text)
{
    BPTR fh = Open(RESULT, MODE_NEWFILE);
    if (!fh) return;
    Write(fh, (APTR)text, strlen(text));
    Close(fh);
}

int main(void)
{
    BPTR fh;
    char buf[64];
    LONG n;

    fh = Open("PROGDIR:progdirchild.data", MODE_OLDFILE);
    if (!fh) {
        say("PROGDIR-FAIL could not open PROGDIR:progdirchild.data\n");
        return 20;
    }
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (n <= 0) {
        say("PROGDIR-FAIL empty read\n");
        return 21;
    }
    buf[n] = 0;
    if (!strstr(buf, WANT)) {
        say("PROGDIR-FAIL wrong file contents\n");
        return 22;
    }
    say("PROGDIR-PASS child resolved its own drawer\n");
    return 0;
}
