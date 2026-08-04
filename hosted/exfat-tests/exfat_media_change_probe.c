/* Target-side removable-media lifecycle probe for exfat-handler. */
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

#define TEST_UNIT 13

static int fail(CONST_STRPTR stage, SIPTR value, SIPTR error)
{
    Printf("[EXFATMEDIA] FAIL %s value %ld error %ld\n",
        stage, value, error);
    return 20;
}

static LONG eject_or_load(struct IOExtTD *io, BOOL eject)
{
    io->iotd_Req.io_Command = TD_EJECT;
    io->iotd_Req.io_Length = eject ? 1 : 0;
    io->iotd_Req.io_Data = NULL;
    io->iotd_Req.io_Offset = 0;
    return DoIO((struct IORequest *)io);
}

static BOOL marker_is(CONST_STRPTR expected)
{
    char marker[16];
    LONG got;
    BPTR file = Open("EXFAT13:Marker.txt", MODE_OLDFILE);

    if (file == BNULL)
        return FALSE;
    memset(marker, 0, sizeof(marker));
    got = Read(file, marker, (LONG)(sizeof(marker) - 1));
    Close(file);
    return got == (LONG)strlen(expected)
        && memcmp(marker, expected, (size_t)got) == 0;
}

int main(void)
{
    struct MsgPort *port = NULL;
    struct IOExtTD *io = NULL;
    struct FileInfoBlock fib;
    struct InfoData info;
    BPTR stale_root = BNULL;
    BPTR stale_file = BNULL;
    BPTR new_root = BNULL;
    SIPTR error = 0;
    ULONG attempt;
    BOOL offline = FALSE;
    int result = 20;

    if (!marker_is("OLD\n"))
        return fail("initial marker", 0, IoErr());

    stale_root = Lock("EXFAT13:", ACCESS_READ);
    if (stale_root == BNULL)
        return fail("initial root lock", 0, IoErr());
    stale_file = Open("EXFAT13:Marker.txt", MODE_OLDFILE);
    if (stale_file == BNULL)
    {
        error = IoErr();
        UnLock(stale_root);
        return fail("initial file handle", 0, error);
    }

    port = CreateMsgPort();
    if (port == NULL)
        goto cleanup;
    io = (struct IOExtTD *)CreateIORequest(port, sizeof(*io));
    if (io == NULL)
        goto cleanup;
    error = OpenDevice("fdsk.device", TEST_UNIT, (struct IORequest *)io, 0);
    if (error != 0)
        goto cleanup;

    error = eject_or_load(io, TRUE);
    if (error != 0)
        goto cleanup_device;

    for (attempt = 0; attempt < 100; attempt++)
    {
        SetIoErr(0);
        if (!Examine(stale_root, &fib)
            && IoErr() == ERROR_DEVICE_NOT_MOUNTED)
        {
            offline = TRUE;
            break;
        }
        Delay(1);
    }
    if (!offline)
    {
        error = IoErr();
        goto cleanup_device;
    }

    /* ACTION_END and ACTION_FREE_LOCK must remain usable for stale objects. */
    if (!Close(stale_file))
    {
        stale_file = BNULL;
        error = IoErr();
        goto cleanup_device;
    }
    stale_file = BNULL;

    if (!Rename("FDSK:Unit13", "FDSK:Unit13.old"))
    {
        error = IoErr();
        goto cleanup_device;
    }
    if (!Rename("FDSK:Unit13.new", "FDSK:Unit13"))
    {
        error = IoErr();
        (void)Rename("FDSK:Unit13.old", "FDSK:Unit13");
        goto cleanup_device;
    }

    error = eject_or_load(io, FALSE);
    if (error != 0)
        goto cleanup_device;

    /* Keep the old same-label volume live long enough for the first remount
       attempt to collide with it, then release the last stale lock. */
    Delay(5);
    UnLock(stale_root);
    stale_root = BNULL;

    for (attempt = 0; attempt < 100; attempt++)
    {
        new_root = Lock("EXFAT13:", ACCESS_READ);
        if (new_root != BNULL)
            break;
        Delay(1);
    }
    if (new_root == BNULL)
    {
        error = IoErr();
        goto cleanup_device;
    }
    if (!Info(new_root, &info) || info.id_DiskState != ID_VALIDATED)
    {
        error = IoErr();
        goto cleanup_device;
    }
    if (!marker_is("NEW\n"))
    {
        error = IoErr();
        goto cleanup_device;
    }

    Printf("[EXFATMEDIA] PASS stale close, same-label replacement, remount\n");
    result = 0;

cleanup_device:
    CloseDevice((struct IORequest *)io);
cleanup:
    if (new_root != BNULL)
        UnLock(new_root);
    if (stale_file != BNULL)
        Close(stale_file);
    if (stale_root != BNULL)
        UnLock(stale_root);
    if (io != NULL)
        DeleteIORequest((struct IORequest *)io);
    if (port != NULL)
        DeleteMsgPort(port);
    if (result != 0)
        return fail("lifecycle", result, error != 0 ? error : IoErr());
    return 0;
}
