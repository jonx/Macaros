/* Target-side ENOSPC rollback gate for a valid zero-free-cluster volume. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <string.h>

static const UBYTE payload[] = "existing payload\n";
static LONG cluster_size;

static int fail(const char *stage, SIPTR result, SIPTR error)
{
    Printf("[EXFATFULL] FAIL %s result %ld error %ld\n",
        stage, result, error);
    return 20;
}

static int check_clean(const char *stage)
{
    struct InfoData info;
    BPTR root = Lock("EXFAT15:", SHARED_LOCK);

    if (root == BNULL)
        return fail(stage, DOSFALSE, IoErr());
    if (!Info(root, &info))
    {
        SIPTR error = IoErr();
        UnLock(root);
        return fail(stage, DOSFALSE, error);
    }
    UnLock(root);
    if (info.id_DiskState != ID_VALIDATED
        || info.id_NumBlocksUsed != info.id_NumBlocks)
        return fail(stage, info.id_DiskState, info.id_NumBlocksUsed);
    if (info.id_BytesPerBlock <= 0)
        return fail(stage, info.id_BytesPerBlock, 0);
    if (cluster_size == 0)
        cluster_size = info.id_BytesPerBlock;
    else if (cluster_size != info.id_BytesPerBlock)
        return fail(stage, info.id_BytesPerBlock, cluster_size);
    return 0;
}

int main(void)
{
    struct FileInfoBlock before, after;
    UBYTE readback[sizeof(payload) - 1];
    BPTR file, lock;
    LONG result;
    SIPTR error;
    int status;

    status = check_clean("initial full-volume state");
    if (status != 0)
        return status;
    file = Open("EXFAT15:Existing.bin", MODE_READWRITE);
    if (file == BNULL || !ExamineFH(file, &before))
    {
        error = IoErr();
        if (file != BNULL)
            Close(file);
        return fail("append setup", DOSFALSE, error);
    }
    SetIoErr(0);
    result = SetFileSize(file, cluster_size + 1, OFFSET_BEGINNING);
    error = IoErr();
    if (result != -1 || error != ERROR_DISK_FULL)
    {
        Close(file);
        return fail("full-volume resize", result, error);
    }
    if (!Close(file))
        return fail("failed append close", DOSFALSE, IoErr());
    status = check_clean("resize rollback kept volume clean");
    if (status != 0)
        return status;

    file = Open("EXFAT15:Existing.bin", MODE_OLDFILE);
    if (file == BNULL || !ExamineFH(file, &after)
        || after.fib_Size != before.fib_Size
        || Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, payload, sizeof(readback)) != 0)
    {
        error = IoErr();
        if (file != BNULL)
            Close(file);
        return fail("resize rollback payload", after.fib_Size, error);
    }
    Close(file);

    SetIoErr(0);
    lock = CreateDir("EXFAT15:NoRoomDir");
    error = IoErr();
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("full-volume CreateDir", DOSTRUE, error);
    }
    if (error != ERROR_DISK_FULL)
        return fail("full-volume CreateDir", DOSFALSE, error);
    status = check_clean("CreateDir rollback kept volume clean");
    if (status != 0)
        return status;
    SetIoErr(0);
    lock = Lock("EXFAT15:NoRoomDir", SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("failed directory remained visible", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail("failed directory lookup", DOSFALSE, IoErr());

    /* Directory entries need no data cluster, so a zero-length file remains
       a legal metadata-only operation even when the allocation bitmap is
       completely full. */
    file = Open("EXFAT15:EmptyAllowed.bin", MODE_NEWFILE);
    if (file == BNULL || !Close(file))
        return fail("zero-length create", DOSFALSE, IoErr());
    file = Open("EXFAT15:EmptyAllowed.bin", MODE_OLDFILE);
    if (file == BNULL || !ExamineFH(file, &after) || after.fib_Size != 0)
    {
        error = IoErr();
        if (file != BNULL)
            Close(file);
        return fail("zero-length verify", after.fib_Size, error);
    }
    Close(file);
    if (!DeleteFile("EXFAT15:EmptyAllowed.bin"))
        return fail("zero-length delete", DOSFALSE, IoErr());
    status = check_clean("metadata-only operations kept volume clean");
    if (status != 0)
        return status;

    Printf("[EXFATFULL] PASS ENOSPC rolled back without dirtying or publishing\n");
    return 0;
}
