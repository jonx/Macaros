/* Target-side gate for a volume whose VolumeDirty flag predates the mount. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <string.h>

static const UBYTE payload[] = "pre-existing dirty payload\n";

static int fail(const char *stage, SIPTR result, SIPTR error)
{
    Printf("[EXFATDIRTY] FAIL %s result %ld error %ld\n",
        stage, result, error);
    return 20;
}

int main(void)
{
    struct InfoData info;
    UBYTE readback[sizeof(payload) - 1];
    BPTR root, file;
    SIPTR error;

    root = Lock("EXFAT16:", SHARED_LOCK);
    if (root == BNULL)
        return fail("mount/read lock", DOSFALSE, IoErr());
    if (!Info(root, &info))
    {
        error = IoErr();
        UnLock(root);
        return fail("Info", DOSFALSE, error);
    }
    UnLock(root);
    if (info.id_DiskState != ID_WRITE_PROTECTED)
        return fail("dirty Info state", info.id_DiskState, 0);

    file = Open("EXFAT16:Existing.bin", MODE_OLDFILE);
    if (file == BNULL
        || Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, payload, sizeof(readback)) != 0)
    {
        error = IoErr();
        if (file != BNULL)
            Close(file);
        return fail("dirty-volume read", DOSFALSE, error);
    }
    Close(file);

    SetIoErr(0);
    file = Open("EXFAT16:Existing.bin", MODE_READWRITE);
    error = IoErr();
    if (file != BNULL)
    {
        Close(file);
        return fail("update unexpectedly opened", DOSTRUE, error);
    }
    if (error != ERROR_DISK_NOT_VALIDATED)
        return fail("update refusal", DOSFALSE, error);

    SetIoErr(0);
    file = Open("EXFAT16:Created.bin", MODE_NEWFILE);
    error = IoErr();
    if (file != BNULL)
    {
        Close(file);
        return fail("create unexpectedly opened", DOSTRUE, error);
    }
    if (error != ERROR_DISK_NOT_VALIDATED)
        return fail("create refusal", DOSFALSE, error);

    Printf("[EXFATDIRTY] PASS pre-dirty volume readable, non-validated and immutable\n");
    return 0;
}
