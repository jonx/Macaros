/* Target-side contract gate for a device that refuses writes.
 *
 * The medium is intact and complete; it simply may not be changed, whether
 * because a stick's write-protect switch is on or because the host handed the
 * unit over read-only. Reads must work exactly as on a writable volume, the
 * volume must report itself write-protected, and every mutation must be
 * refused with that same reason rather than failing somewhere lower down.
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/dos.h>

#include <string.h>

#define VOLUME   "EXFAT18:"
#define PAYLOAD  "EXFAT18:Existing.bin"
#define EXPECTED "read-only device payload\n"

static int fail(const char *name, SIPTR result, SIPTR error)
{
    Printf("[EXFATWP] FAIL %s result %ld error %ld\n", name, result, error);
    return 20;
}

/* Every mutation must be refused for the same stated reason. */
static int refused(const char *name, LONG ok, SIPTR error)
{
    if (ok)
        return fail(name, DOSTRUE, error);
    if (error != ERROR_DISK_WRITE_PROTECTED)
        return fail(name, DOSFALSE, error);
    return 0;
}

int main(void)
{
    UBYTE buffer[sizeof(EXPECTED)];
    struct InfoData info;
    BPTR file, lock;
    LONG length;
    int rc;

    /* The volume reads. */
    file = Open((CONST_STRPTR)PAYLOAD, MODE_OLDFILE);
    if (file == BNULL)
        return fail("open payload", DOSFALSE, IoErr());
    length = Read(file, buffer, sizeof(buffer) - 1);
    Close(file);
    if (length != (LONG)(sizeof(EXPECTED) - 1))
        return fail("payload length", length, IoErr());
    buffer[length] = '\0';
    if (strcmp((char *)buffer, EXPECTED) != 0)
        return fail("payload contents", DOSFALSE, 0);

    /* And says why it cannot be written. */
    lock = Lock((CONST_STRPTR)VOLUME, SHARED_LOCK);
    if (lock == BNULL)
        return fail("lock volume", DOSFALSE, IoErr());
    memset(&info, 0, sizeof(info));
    if (!Info(lock, &info))
    {
        UnLock(lock);
        return fail("Info", DOSFALSE, IoErr());
    }
    UnLock(lock);
    if (info.id_DiskState != ID_WRITE_PROTECTED)
        return fail("disk state", info.id_DiskState, 0);

    SetIoErr(0);
    file = Open((CONST_STRPTR)PAYLOAD, MODE_READWRITE);
    if (file != BNULL)
        Close(file);
    rc = refused("update handle", file != BNULL, IoErr());
    if (rc) return rc;

    SetIoErr(0);
    file = Open((CONST_STRPTR)"EXFAT18:Created.bin", MODE_NEWFILE);
    if (file != BNULL)
        Close(file);
    rc = refused("create", file != BNULL, IoErr());
    if (rc) return rc;

    SetIoErr(0);
    rc = refused("delete", DeleteFile((CONST_STRPTR)PAYLOAD), IoErr());
    if (rc) return rc;

    SetIoErr(0);
    rc = refused("rename", Rename((CONST_STRPTR)PAYLOAD,
                                  (CONST_STRPTR)"EXFAT18:Renamed.bin"), IoErr());
    if (rc) return rc;

    SetIoErr(0);
    lock = CreateDir((CONST_STRPTR)"EXFAT18:NewDir");
    if (lock != BNULL)
        UnLock(lock);
    rc = refused("create directory", lock != BNULL, IoErr());
    if (rc) return rc;

    SetIoErr(0);
    rc = refused("set protection",
                 SetProtection((CONST_STRPTR)PAYLOAD, FIBF_READ), IoErr());
    if (rc) return rc;

    SetIoErr(0);
    rc = refused("relabel", Relabel((CONST_STRPTR)VOLUME,
                                    (CONST_STRPTR)"RENAMED"), IoErr());
    if (rc) return rc;

    Printf("[EXFATWP] PASS write-protected device is readable and refuses "
           "every mutation as write-protected\n");
    return 0;
}
