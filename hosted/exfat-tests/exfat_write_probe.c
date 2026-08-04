/* Target-side gate for bounded, in-place exFAT writes. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <string.h>

#define WRITE_OFFSET 500

static const UBYTE marker[] =
    "AROS-exFAT-in-place-write-crosses-sector";
static const UBYTE contiguous_marker[] = "AROS-contiguous-growth";
static const UBYTE fallback_marker[] = "AROS-FAT-fallback-growth";
static const UBYTE created_payload[] = "AROS-created-file-payload\n";
static const UBYTE directory_payload[] = "AROS-created-directory-payload\n";
static const UBYTE zero_extension[4096];

static int fail(const char *name, SIPTR result, SIPTR error)
{
    Printf("[EXFATWRITE] FAIL %s result %ld error %ld\n",
        name, result, error);
    return 20;
}

static int verify(BPTR file, const char *stage)
{
    UBYTE data[sizeof(marker) + 8];
    ULONG i;
    LONG result;

    if (Seek(file, WRITE_OFFSET - 4, OFFSET_BEGINNING) < 0)
        return fail(stage, -1, IoErr());
    result = Read(file, data, sizeof(data));
    if (result != (LONG)sizeof(data))
        return fail(stage, result, IoErr());
    for (i = 0; i < 4; i++)
        if (data[i] != 0)
            return fail(stage, data[i], 0);
    if (memcmp(data + 4, marker, sizeof(marker) - 1) != 0)
        return fail(stage, DOSFALSE, 0);
    for (i = sizeof(marker) + 3; i < sizeof(data); i++)
        if (data[i] != 0)
            return fail(stage, data[i], 0);
    return 0;
}

static int append_and_verify(const char *path, const UBYTE *data,
    ULONG length)
{
    struct FileInfoBlock before, after;
    static UBYTE readback[4096];
    BPTR file;
    LONG result;
    int status;

    if (length > sizeof(readback))
        return fail("append test vector", length, 0);
    file = Open(path, MODE_READWRITE);
    if (file == BNULL)
        return fail("append open", DOSFALSE, IoErr());
    if (!ExamineFH(file, &before) || Seek(file, 0, OFFSET_END) < 0)
    {
        status = fail("append seek", -1, IoErr());
        Close(file);
        return status;
    }
    result = Write(file, data, length);
    if (result != (LONG)length)
    {
        status = fail("append write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("append commit", DOSFALSE, IoErr());

    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
        return fail("append reopen", DOSFALSE, IoErr());
    if (!ExamineFH(file, &after)
        || before.fib_Size < 0 || after.fib_Size < 0
        || (UQUAD)(ULONG)after.fib_Size
            != (UQUAD)(ULONG)before.fib_Size + length
        || Seek(file, -(LONG)length, OFFSET_END) < 0
        || Read(file, readback, length) != (LONG)length
        || memcmp(readback, data, length) != 0)
    {
        status = fail("append readback", after.fib_Size, IoErr());
        Close(file);
        return status;
    }
    Close(file);
    return 0;
}

static int resize_and_verify(const char *path, LONG new_size)
{
    struct FileInfoBlock info;
    BPTR file;
    LONG result;
    int status;

    file = Open(path, MODE_READWRITE);
    if (file == BNULL)
        return fail("resize open", DOSFALSE, IoErr());
    result = SetFileSize(file, new_size, OFFSET_BEGINNING);
    if (result != new_size)
    {
        status = fail("SetFileSize", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("resize commit", DOSFALSE, IoErr());

    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
        return fail("resize reopen", DOSFALSE, IoErr());
    if (!ExamineFH(file, &info) || info.fib_Size != new_size)
    {
        status = fail("resize readback", info.fib_Size, IoErr());
        Close(file);
        return status;
    }
    Close(file);
    return 0;
}

static int create_and_verify(void)
{
    struct FileInfoBlock info;
    UBYTE readback[sizeof(created_payload) - 1];
    BPTR file;
    LONG result;
    int status;

    file = Open("EXFAT4:Created.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("create open", DOSFALSE, IoErr());
    result = Write(file, created_payload, sizeof(created_payload) - 1);
    if (result != (LONG)(sizeof(created_payload) - 1))
    {
        status = fail("create write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("create commit", DOSFALSE, IoErr());
    file = Open("EXFAT4:Created.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("create reopen", DOSFALSE, IoErr());
    if (Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, created_payload, sizeof(readback)) != 0)
    {
        status = fail("create readback", DOSFALSE, IoErr());
        Close(file);
        return status;
    }
    Close(file);

    file = Open("EXFAT4:OutputTruncate.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("FINDOUTPUT truncate", DOSFALSE, IoErr());
    if (!Close(file))
        return fail("FINDOUTPUT truncate commit", DOSFALSE, IoErr());
    file = Open("EXFAT4:OutputTruncate.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("truncated reopen", DOSFALSE, IoErr());
    if (!ExamineFH(file, &info) || info.fib_Size != 0)
    {
        status = fail("truncated size", info.fib_Size, IoErr());
        Close(file);
        return status;
    }
    Close(file);
    return 0;
}

static int delete_and_verify(void)
{
    BPTR lock;

    if (!DeleteFile("EXFAT4:DeleteMe.bin"))
        return fail("DeleteFile", DOSFALSE, IoErr());
    SetIoErr(0);
    lock = Lock("EXFAT4:DeleteMe.bin", SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("deleted lookup", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail("deleted lookup", DOSFALSE, IoErr());
    return 0;
}

static int create_directory_and_verify(void)
{
    UBYTE readback[sizeof(directory_payload) - 1];
    char grow_path[] = "EXFAT4:CreatedDir/Grow00.bin";
    BPTR lock, file;
    ULONG i;
    LONG result;
    int status;

    lock = CreateDir("EXFAT4:CreatedDir");
    if (lock == BNULL)
        return fail("CreateDir", DOSFALSE, IoErr());
    UnLock(lock);

    file = Open("EXFAT4:CreatedDir/Inside.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("nested create", DOSFALSE, IoErr());
    result = Write(file, directory_payload, sizeof(directory_payload) - 1);
    if (result != (LONG)(sizeof(directory_payload) - 1))
    {
        status = fail("nested write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("nested commit", DOSFALSE, IoErr());
    file = Open("EXFAT4:CreatedDir/Inside.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("nested reopen", DOSFALSE, IoErr());
    if (Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, directory_payload, sizeof(readback)) != 0)
    {
        status = fail("nested readback", DOSFALSE, IoErr());
        Close(file);
        return status;
    }
    Close(file);

    for (i = 0; i < 50; i++)
    {
        grow_path[22] = (char)('0' + i / 10);
        grow_path[23] = (char)('0' + i % 10);
        file = Open(grow_path, MODE_NEWFILE);
        if (file == BNULL)
            return fail("directory growth create", DOSFALSE, IoErr());
        if (i == 49)
        {
            result = Write(file, directory_payload,
                sizeof(directory_payload) - 1);
            if (result != (LONG)(sizeof(directory_payload) - 1))
            {
                status = fail("directory growth write", result, IoErr());
                Close(file);
                return status;
            }
        }
        if (!Close(file))
            return fail("directory growth commit", DOSFALSE, IoErr());
    }

    SetIoErr(0);
    if (DeleteFile("EXFAT4:CreatedDir")
        || IoErr() != ERROR_DIRECTORY_NOT_EMPTY)
        return fail("delete nonempty directory", DOSFALSE, IoErr());

    lock = CreateDir("EXFAT4:DeleteDir");
    if (lock == BNULL)
        return fail("CreateDir for deletion", DOSFALSE, IoErr());
    UnLock(lock);
    if (!DeleteFile("EXFAT4:DeleteDir"))
        return fail("delete empty directory", DOSFALSE, IoErr());
    SetIoErr(0);
    lock = Lock("EXFAT4:DeleteDir", SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("deleted directory lookup", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail("deleted directory lookup", DOSFALSE, IoErr());
    return 0;
}

static int rename_and_verify(void)
{
    UBYTE readback[sizeof(created_payload) - 1];
    BPTR lock, file;
    LONG result;
    int status;

    file = Open("EXFAT4:RenameMe.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("rename source create", DOSFALSE, IoErr());
    result = Write(file, created_payload, sizeof(created_payload) - 1);
    if (result != (LONG)(sizeof(created_payload) - 1))
    {
        status = fail("rename source write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("rename source commit", DOSFALSE, IoErr());
    if (!Rename("EXFAT4:RenameMe.bin", "EXFAT4:Renamed.bin"))
        return fail("same-directory rename", DOSFALSE, IoErr());
    if (!Rename("EXFAT4:Renamed.bin", "EXFAT4:CreatedDir/Moved.bin"))
        return fail("cross-directory rename", DOSFALSE, IoErr());
    file = Open("EXFAT4:CreatedDir/Moved.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("renamed file reopen", DOSFALSE, IoErr());
    if (Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, created_payload, sizeof(readback)) != 0)
    {
        status = fail("renamed file readback", DOSFALSE, IoErr());
        Close(file);
        return status;
    }
    Close(file);

    lock = CreateDir("EXFAT4:RenameDir");
    if (lock == BNULL)
        return fail("rename directory create", DOSFALSE, IoErr());
    UnLock(lock);
    file = Open("EXFAT4:RenameDir/Child.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("rename directory child", DOSFALSE, IoErr());
    result = Write(file, created_payload, sizeof(created_payload) - 1);
    if (result != (LONG)(sizeof(created_payload) - 1))
    {
        status = fail("rename directory child write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("rename directory child commit", DOSFALSE, IoErr());
    if (!Rename("EXFAT4:RenameDir", "EXFAT4:CreatedDir/MovedDir"))
        return fail("directory rename", DOSFALSE, IoErr());
    file = Open("EXFAT4:CreatedDir/MovedDir/Child.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("renamed directory child reopen", DOSFALSE, IoErr());
    Close(file);
    return 0;
}

static int protection_and_verify(void)
{
    struct FileInfoBlock info;
    BPTR lock, file;

    if (!SetProtection("EXFAT4:Created.bin", FIBF_WRITE | FIBF_DELETE))
        return fail("set read-only protection", DOSFALSE, IoErr());
    lock = Lock("EXFAT4:Created.bin", SHARED_LOCK);
    if (lock == BNULL)
        return fail("protected lock", DOSFALSE, IoErr());
    if (!Examine(lock, &info)
        || (info.fib_Protection & (FIBF_WRITE | FIBF_DELETE))
            != (FIBF_WRITE | FIBF_DELETE))
    {
        UnLock(lock);
        return fail("protected examine", info.fib_Protection, IoErr());
    }
    UnLock(lock);
    SetIoErr(0);
    file = Open("EXFAT4:Created.bin", MODE_READWRITE);
    if (file != BNULL)
    {
        Close(file);
        return fail("protected update", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_DISK_WRITE_PROTECTED)
        return fail("protected update", DOSFALSE, IoErr());
    if (!SetProtection("EXFAT4:Created.bin", FIBF_ARCHIVE))
        return fail("clear read-only protection", DOSFALSE, IoErr());
    file = Open("EXFAT4:Created.bin", MODE_READWRITE);
    if (file == BNULL)
        return fail("cleared protection update", DOSFALSE, IoErr());
    if (!Close(file))
        return fail("cleared protection close", DOSFALSE, IoErr());
    return 0;
}

static int collision_and_verify(void)
{
    UBYTE readback[sizeof(created_payload) - 1];
    BPTR lock, file;
    LONG result;
    SIPTR error;
    int status;

    SetIoErr(0);
    lock = CreateDir("EXFAT4:cReAtEdDiR");
    error = IoErr();
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail("case-folded directory duplicate", DOSTRUE, error);
    }
    if (error != ERROR_OBJECT_EXISTS)
        return fail("case-folded directory duplicate", DOSFALSE, error);

    file = Open("EXFAT4:CollisionSource.bin", MODE_NEWFILE);
    if (file == BNULL)
        return fail("collision source create", DOSFALSE, IoErr());
    result = Write(file, created_payload, sizeof(created_payload) - 1);
    if (result != (LONG)(sizeof(created_payload) - 1))
    {
        status = fail("collision source write", result, IoErr());
        Close(file);
        return status;
    }
    if (!Close(file))
        return fail("collision source commit", DOSFALSE, IoErr());

    SetIoErr(0);
    result = Rename("EXFAT4:CollisionSource.bin", "EXFAT4:cReAtEd.BIN");
    error = IoErr();
    if (result || error != ERROR_OBJECT_EXISTS)
        return fail("case-folded rename collision", result, error);

    file = Open("EXFAT4:CollisionSource.bin", MODE_OLDFILE);
    if (file == BNULL
        || Read(file, readback, sizeof(readback)) != (LONG)sizeof(readback)
        || memcmp(readback, created_payload, sizeof(readback)) != 0)
    {
        error = IoErr();
        if (file != BNULL)
            Close(file);
        return fail("rename collision source intact", DOSFALSE, error);
    }
    Close(file);
    if (!DeleteFile("EXFAT4:CollisionSource.bin"))
        return fail("collision source cleanup", DOSFALSE, IoErr());
    return 0;
}

static int date_and_verify(void)
{
    struct DateStamp wanted;
    struct FileInfoBlock info;
    BPTR lock, file;
    UBYTE first;

    wanted.ds_Days = 735;
    wanted.ds_Minute = 13 * 60 + 7;
    wanted.ds_Tick = 5 * TICKS_PER_SECOND + 3;
    if (!SetFileDate("EXFAT4:Created.bin", &wanted))
        return fail("SetFileDate", DOSFALSE, IoErr());
    lock = Lock("EXFAT4:Created.bin", SHARED_LOCK);
    if (lock == BNULL)
        return fail("dated lock", DOSFALSE, IoErr());
    if (!Examine(lock, &info)
        || info.fib_Date.ds_Days != wanted.ds_Days
        || info.fib_Date.ds_Minute != wanted.ds_Minute
        || info.fib_Date.ds_Tick != wanted.ds_Tick)
    {
        UnLock(lock);
        return fail("dated examine", info.fib_Date.ds_Days, IoErr());
    }
    UnLock(lock);

    /* A content write must replace the explicit old modification time and
       update LastAccessedTimestamp as required by exFAT.  Write the existing
       first byte so the payload oracle remains unchanged. */
    file = Open("EXFAT4:Created.bin", MODE_READWRITE);
    if (file == BNULL || Read(file, &first, 1) != 1
        || Seek(file, 0, OFFSET_BEGINNING) < 0
        || Write(file, &first, 1) != 1)
    {
        if (file != BNULL)
            Close(file);
        return fail("automatic modification date write", DOSFALSE, IoErr());
    }
    if (!Close(file))
        return fail("automatic modification date commit", DOSFALSE, IoErr());
    lock = Lock("EXFAT4:Created.bin", SHARED_LOCK);
    if (lock == BNULL)
        return fail("automatic modification date lock", DOSFALSE, IoErr());
    if (!Examine(lock, &info)
        || (info.fib_Date.ds_Days == wanted.ds_Days
            && info.fib_Date.ds_Minute == wanted.ds_Minute
            && info.fib_Date.ds_Tick == wanted.ds_Tick))
    {
        UnLock(lock);
        return fail("automatic modification date", info.fib_Date.ds_Days,
            IoErr());
    }
    UnLock(lock);

    /* Leave a deterministic, caller-supplied modification time behind for
       the raw-image oracle.  Unlike an unset machine clock, this has a known
       UTC relationship and must therefore carry OffsetValid. */
    if (!SetFileDate("EXFAT4:Created.bin", &wanted))
        return fail("final SetFileDate", DOSFALSE, IoErr());
    return 0;
}

static int relabel_and_verify(void)
{
    struct FileInfoBlock info;
    BPTR lock;

    if (!Relabel("EXFAT4:", "AROSRENAMED"))
        return fail("Relabel", DOSFALSE, IoErr());
    lock = Lock("EXFAT4:", SHARED_LOCK);
    if (lock == BNULL)
        return fail("relabeled root lock", DOSFALSE, IoErr());
    if (!Examine(lock, &info)
        || strcmp(info.fib_FileName, "AROSRENAMED") != 0)
    {
        UnLock(lock);
        return fail("relabeled root examine", info.fib_FileName[0], IoErr());
    }
    UnLock(lock);
    return 0;
}

int main(void)
{
    BPTR file, second;
    struct FileInfoBlock info;
    UBYTE before[sizeof(marker) - 1];
    UBYTE byte = 0x5a;
    LONG result;
    int status;

    file = Open("EXFAT4:Writable.bin", MODE_READWRITE);
    if (file == BNULL)
        return fail("open update", DOSFALSE, IoErr());
    if (!ExamineFH(file, &info)
        || (info.fib_Protection & FIBF_WRITE) != 0)
    {
        status = fail("writable protection", DOSFALSE, IoErr());
        Close(file);
        return status;
    }

    SetIoErr(0);
    second = Open("EXFAT4:Writable.bin", MODE_READWRITE);
    if (second != BNULL)
    {
        Close(second);
        Close(file);
        return fail("exclusive update", DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_IN_USE)
    {
        status = fail("exclusive update", DOSFALSE, IoErr());
        Close(file);
        return status;
    }

    if (Seek(file, WRITE_OFFSET, OFFSET_BEGINNING) < 0
        || Read(file, before, sizeof(before)) != (LONG)sizeof(before))
    {
        status = fail("read preimage", DOSFALSE, IoErr());
        Close(file);
        return status;
    }
    if (memcmp(before, "\0\0\0\0\0\0\0\0\0\0\0\0", 12) != 0)
    {
        status = fail("zero preimage", DOSFALSE, 0);
        Close(file);
        return status;
    }

    if (Seek(file, WRITE_OFFSET, OFFSET_BEGINNING) < 0)
    {
        status = fail("seek write", -1, IoErr());
        Close(file);
        return status;
    }
    result = Write(file, marker, sizeof(marker) - 1);
    if (result != (LONG)(sizeof(marker) - 1))
    {
        status = fail("cross-sector write", result, IoErr());
        Close(file);
        return status;
    }
    status = verify(file, "cached readback");
    if (status != 0)
    {
        Close(file);
        return status;
    }

    if (Seek(file, 0, OFFSET_END) < 0)
    {
        status = fail("seek end", -1, IoErr());
        Close(file);
        return status;
    }
    SetIoErr(0);
    result = Write(file, &byte, 1);
    if (result != 1)
    {
        status = fail("DataLength growth", result, IoErr());
        Close(file);
        return status;
    }

    if (!Close(file))
        return fail("commit close", DOSFALSE, IoErr());

    file = Open("EXFAT4:Writable.bin", MODE_OLDFILE);
    if (file == BNULL)
        return fail("reopen", DOSFALSE, IoErr());
    status = verify(file, "committed readback");
    if (status == 0 && (Seek(file, -1, OFFSET_END) < 0
            || Read(file, before, 1) != 1 || before[0] != byte))
        status = fail("grown tail", before[0], IoErr());
    Close(file);
    if (status != 0)
        return status;

    status = append_and_verify("EXFAT4:Blocker.bin", contiguous_marker,
        sizeof(contiguous_marker) - 1);
    if (status != 0)
        return status;
    status = append_and_verify("EXFAT4:GrowFallback.bin", fallback_marker,
        sizeof(fallback_marker) - 1);
    if (status != 0)
        return status;
    status = append_and_verify("EXFAT4:GrowFallback.bin", zero_extension,
        sizeof(zero_extension));
    if (status != 0)
        return status;
    status = resize_and_verify("EXFAT4:GrowFallback.bin",
        1048576 + (LONG)sizeof(fallback_marker) - 1);
    if (status != 0)
        return status;
    status = resize_and_verify("EXFAT4:Truncate.bin", 524288);
    if (status != 0)
        return status;
    status = create_and_verify();
    if (status != 0)
        return status;
    status = delete_and_verify();
    if (status != 0)
        return status;
    status = create_directory_and_verify();
    if (status != 0)
        return status;
    status = rename_and_verify();
    if (status != 0)
        return status;
    status = collision_and_verify();
    if (status != 0)
        return status;
    status = protection_and_verify();
    if (status != 0)
        return status;
    status = date_and_verify();
    if (status != 0)
        return status;
    status = relabel_and_verify();
    if (status != 0)
        return status;

    Printf("[EXFATWRITE] PASS create, resize, rename, collision, metadata, relabel, directory, and delete committed\n");
    return 0;
}
