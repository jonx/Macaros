/* Target-side ACTION_FORMAT and immediate writable-remount gate. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define ID_EXFAT_DISK 0x46415458UL

static const UBYTE payload[] = "AROS handler-formatted exFAT payload\n";

static int fail(const char *stage, SIPTR result, SIPTR error)
{
    Printf("[EXFATFORMAT] FAIL %s result %ld error %ld\n",
        stage, result, error);
    return 20;
}

int main(int argc, char **argv)
{
    UBYTE readback[sizeof(payload) - 1];
    const char *device = argc == 2 ? argv[1] : "EXFAT6:";
    char path[64];
    BPTR file;
    LONG result;
    int status;

    if (snprintf(path, sizeof(path), "%sFormatted.bin", device)
            < 0)
        return fail("path", DOSFALSE, 0);
    if (!Format(device, "AROSFORMAT", ID_EXFAT_DISK))
        return fail("Format", DOSFALSE, IoErr());
    file = Open(path, MODE_NEWFILE);
    if (file == BNULL)
        return fail("create", DOSFALSE, IoErr());
    result = Write(file, payload, sizeof(payload) - 1);
    if (result != (LONG)(sizeof(payload) - 1))
    {
        status = fail("write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("commit", DOSFALSE, IoErr());
    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
        return fail("reopen", DOSFALSE, IoErr());
    if (Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, payload, sizeof(readback)) != 0)
    {
        status = fail("readback", DOSFALSE, IoErr());
        Close(file);
        return status;
    }
    Close(file);
    Printf("[EXFATFORMAT] PASS format, mount, create, and readback\n");
    return 0;
}
