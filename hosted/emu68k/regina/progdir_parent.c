/* The parent half of the PROGDIR: regression: one 68k program starting
 * another that lives in a DIFFERENT drawer.  The parent deliberately does not
 * change directory, so a child that inherits the launcher's PROGDIR: fails
 * and a child with its own passes.  Both run as contexts of one emu68k run
 * inside one native process, which is exactly the case that used to break.
 */
#include <dos/dostags.h>
#include <proto/dos.h>
#include <string.h>

#define CHILD  "MacRW:Regina68k/progdir/progdirchild"
#define RESULT "MacRW:Regina68k/progdir.result"

static int result_complete(void)
{
    BPTR fh;
    char buf[128];
    LONG n;

    fh = Open(RESULT, MODE_OLDFILE);
    if (!fh) return 0;
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "PASS") != 0 || strstr(buf, "FAIL") != 0;
}

int main(void)
{
    unsigned turns;

    if (SystemTags(CHILD, SYS_Asynch, TRUE, TAG_DONE) == -1) {
        PutStr("PROGDIR-FAIL child could not be launched\n");
        return 20;
    }
    for (turns = 0; turns < 4000u; turns++) {
        if (result_complete()) return 0;
        Delay(5);
    }
    PutStr("PROGDIR-FAIL child wrote no result\n");
    return 21;
}
