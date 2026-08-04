/* Target-side ordered-flush failure injector for the test-only handler. */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

#include <stdlib.h>

#define ACTION_EXFAT_ARM_FAILPOINT 0x58464650L
#define EXFAT_FAILPOINT_COOKIE     0x45584654UL

static const UBYTE payload[] = "-new-state-after-ordered-flushes";

static int fail(const char *stage, SIPTR result, SIPTR error)
{
    Printf("[EXFATFAIL] FAIL %s result %ld error %ld\n",
        stage, result, error);
    return 20;
}

int main(int argc, char **argv)
{
    struct FileHandle *fh;
    BPTR control, file;
    LONG point, wrote;
    BOOL close_result;
    SIPTR result;

    if (argc != 2)
        return fail("argument", argc, 0);
    point = (LONG)atol(argv[1]);
    if (point <= 0)
        return fail("point", point, 0);

    control = Open("EXFAT5:FailGrow.bin", MODE_OLDFILE);
    if (control == BNULL)
        return fail("control open", DOSFALSE, IoErr());
    fh = BADDR(control);
    SetIoErr(0);
    result = DoPkt(fh->fh_Type, ACTION_EXFAT_ARM_FAILPOINT,
        EXFAT_FAILPOINT_COOKIE, point, 0, 0, 0);
    if (result != DOSTRUE)
    {
        SIPTR error = IoErr();
        Close(control);
        return fail("arm", result, error);
    }
    Close(control);

    file = Open("EXFAT5:FailGrow.bin", MODE_READWRITE);
    if (file == BNULL)
    {
        Printf("[EXFATFAIL] PASS point %ld failed during open (%ld)\n",
            point, IoErr());
        return 0;
    }
    if (Seek(file, 0, OFFSET_END) < 0)
    {
        SIPTR error = IoErr();
        Close(file);
        return fail("seek", -1, error);
    }
    SetIoErr(0);
    wrote = Write(file, payload, sizeof(payload) - 1);
    close_result = Close(file);
    if (wrote == (LONG)(sizeof(payload) - 1) && close_result)
        return fail("mutation unexpectedly committed", wrote, 0);
    Printf("[EXFATFAIL] PASS point %ld injected write %ld close %ld error %ld\n",
        point, wrote, close_result, IoErr());
    return 0;
}
