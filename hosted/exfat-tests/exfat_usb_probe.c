/* Target-side mutation gate for a byte-exact physical USB capture. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

static const UBYTE payload[] = "AROS exFAT USB round-trip\n";

static void stage(const char *name)
{
    Printf("[EXFATUSB] %s\n", name);
    Flush(Output());
}

static int fail(const char *name, SIPTR result, SIPTR error)
{
    Printf("[EXFATUSB] FAIL %s result %ld error %ld\n",
        name, result, error);
    Flush(Output());
    return 20;
}

static int make_path(char *path, size_t capacity, const char *volume,
    const char *relative)
{
    int length = snprintf(path, capacity, "%s%s", volume, relative);

    return length >= 0 && (size_t)length < capacity;
}

int main(int argc, char **argv)
{
    UBYTE readback[sizeof(payload) - 1];
    const char *volume = argc == 2 ? argv[1] : "EXFAT14:";
    char created[96], original[96], renamed[96], deleted[96];
    BPTR file, lock;
    LONG result;

    if (argc > 2 || volume[0] == '\0'
        || volume[strlen(volume) - 1] != ':'
        || !make_path(created, sizeof(created), volume,
            "AROSUSB/AROS-Written.txt")
        || !make_path(original, sizeof(original), volume,
            "AROSUSB/Handover.txt")
        || !make_path(renamed, sizeof(renamed), volume,
            "AROSUSB/Renamed.txt")
        || !make_path(deleted, sizeof(deleted), volume,
            "AROSUSB/Nested/Source.c"))
        return fail("usage: EXFATUSBProbe [VOLUME:]", DOSFALSE, 0);

    stage("before create open");
    file = Open(created, MODE_NEWFILE);
    if (file == BNULL)
        return fail("create open", DOSFALSE, IoErr());
    stage("after create open");

    result = Write(file, payload, sizeof(payload) - 1);
    if (result != (LONG)(sizeof(payload) - 1))
    {
        SIPTR error = IoErr();
        Close(file);
        return fail("write", result, error);
    }
    stage("after write");
    if (!Close(file))
        return fail("close commit", DOSFALSE, IoErr());
    stage("after close commit");

    file = Open(created, MODE_OLDFILE);
    if (file == BNULL)
        return fail("reopen", DOSFALSE, IoErr());
    result = Read(file, readback, sizeof(readback));
    if (result != (LONG)sizeof(readback)
        || memcmp(readback, payload, sizeof(readback)) != 0)
    {
        SIPTR error = IoErr();
        Close(file);
        return fail("readback", result, error);
    }
    Close(file);
    stage("after committed readback");

    if (!Rename(original, renamed))
        return fail("rename", DOSFALSE, IoErr());
    stage("after rename");
    if (!DeleteFile(deleted))
        return fail("delete", DOSFALSE, IoErr());
    stage("after delete");

    SetIoErr(0);
    lock = Lock(original, SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("old rename lookup", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail("old rename lookup", DOSFALSE, IoErr());
    SetIoErr(0);
    lock = Lock(deleted, SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("deleted lookup", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail("deleted lookup", DOSFALSE, IoErr());

    stage("PASS");
    return 0;
}
