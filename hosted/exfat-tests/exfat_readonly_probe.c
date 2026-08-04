/* Target-side contract gate for read-only handles and unsupported comments. */
#include <dos/dos.h>
#include <dos/dos64.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

struct action_case
{
    LONG action;
    const char *name;
};

static const struct action_case write_actions[] = {
    { ACTION_WRITE, "ACTION_WRITE" },
    { ACTION_SET_FILE_SIZE, "ACTION_SET_FILE_SIZE" },
    { ACTION_CHANGE_FILE_SIZE64, "ACTION_CHANGE_FILE_SIZE64" },
    { ACTION_SET_FILE_SIZE64, "ACTION_SET_FILE_SIZE64" },
};

static int fail(const char *name, SIPTR result, SIPTR error)
{
    Printf("[EXFATRO] FAIL %s result %ld error %ld\n",
           name, result, error);
    return 20;
}

int main(void)
{
    BPTR file = Open("EXFAT4:Hello.txt", MODE_OLDFILE);
    struct FileHandle *fh;
    UBYTE byte = 0;
    ULONG i;
    SIPTR result;
    SIPTR error;

    if (file == BNULL)
        return fail("Open control", DOSFALSE, IoErr());
    fh = BADDR(file);

    for (i = 0; i < sizeof(write_actions) / sizeof(write_actions[0]); i++)
    {
        SetIoErr(0);
        result = DoPkt(fh->fh_Type, write_actions[i].action,
                       (SIPTR)fh->fh_Arg1, 0, 0, 0, 0);
        error = IoErr();
        if ((write_actions[i].action == ACTION_WRITE
                || write_actions[i].action == ACTION_SET_FILE_SIZE
                || write_actions[i].action == ACTION_CHANGE_FILE_SIZE64
                || write_actions[i].action == ACTION_SET_FILE_SIZE64
                ? result != -1 : result != DOSFALSE)
            || error != ERROR_DISK_WRITE_PROTECTED)
        {
            Close(file);
            return fail(write_actions[i].name, result, error);
        }
    }

    SetIoErr(0);
    result = DoPkt(fh->fh_Type, ACTION_SET_COMMENT,
                   (SIPTR)fh->fh_Arg1, 0, 0, 0, 0);
    error = IoErr();
    if (result != DOSFALSE || error != ERROR_ACTION_NOT_KNOWN)
    {
        Close(file);
        return fail("ACTION_SET_COMMENT policy", result, error);
    }

    SetIoErr(0);
    result = DoPkt(fh->fh_Type, 0x6f584652L,
                   (SIPTR)fh->fh_Arg1, 0, 0, 0, 0);
    error = IoErr();
    if (result != DOSFALSE || error != ERROR_ACTION_NOT_KNOWN)
    {
        Close(file);
        return fail("unknown action", result, error);
    }

    if (Seek(file, 0, OFFSET_BEGINNING) < 0 || Read(file, &byte, 1) != 1
        || byte != 'A')
    {
        error = IoErr();
        Close(file);
        return fail("control unchanged", byte, error);
    }
    Close(file);
    Printf("[EXFATRO] PASS 4 protected mutations refused; comments and unknown action classified\n");
    return 0;
}
