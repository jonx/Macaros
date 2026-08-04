/*
 * Read-only target acceptance probe for a physically hotplugged AROSEX stick.
 * Run only after the USB stack has automounted it; no Mountlist is used.
 */
#include <dos/dos.h>
#include <dos/exfat.h>
#include <proto/dos.h>

#include <string.h>

struct expected_file
{
    const char *name;
    ULONG size;
    ULONG crc32;
};

static const struct expected_file expected[] = {
    { "Renamed.txt",      13884, 0xe14a4cedUL },
    { "Handler.bin",      84984, 0x3a7ac8abUL },
    { "AROS-Written.txt",    26, 0x8c6438cfUL }
};

static int fail(const char *what, SIPTR result, SIPTR error)
{
    Printf("[EXFATHOTPLUG] FAIL %s result %ld error %ld\n",
        what, result, error);
    return RETURN_FAIL;
}

static ULONG crc32_update(ULONG crc, const UBYTE *data, ULONG length)
{
    ULONG i, bit;

    for (i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
        {
            ULONG mask = (ULONG)-(LONG)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320UL & mask);
        }
    }
    return crc;
}

static int check_file(const char *path, const struct expected_file *want)
{
    UBYTE buffer[4096];
    BPTR file;
    ULONG crc = 0xffffffffUL, total = 0;
    LONG got;

    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
        return fail(path, DOSFALSE, IoErr());
    while ((got = Read(file, buffer, sizeof(buffer))) > 0)
    {
        crc = crc32_update(crc, buffer, (ULONG)got);
        total += (ULONG)got;
    }
    if (got < 0)
    {
        SIPTR error = IoErr();
        Close(file);
        return fail(path, got, error);
    }
    if (!Close(file))
        return fail(path, DOSFALSE, IoErr());
    crc ^= 0xffffffffUL;
    if (total != want->size || crc != want->crc32)
    {
        Printf("[EXFATHOTPLUG] FAIL %s size %lu/%lu crc %08lx/%08lx\n",
            path, total, want->size, crc, want->crc32);
        return RETURN_FAIL;
    }
    Printf("[EXFATHOTPLUG] OK %s size %lu crc %08lx\n",
        path, total, crc);
    return RETURN_OK;
}

static int expect_absent(const char *path)
{
    BPTR lock;

    SetIoErr(0);
    lock = Lock(path, SHARED_LOCK);
    if (lock != BNULL)
    {
        UnLock(lock);
        return fail(path, DOSTRUE, 0);
    }
    if (IoErr() != ERROR_OBJECT_NOT_FOUND)
        return fail(path, DOSFALSE, IoErr());
    return RETURN_OK;
}

int main(int argc, char **argv)
{
    const char *device = argc == 2 ? argv[1] : "AROSEX:";
    char root[256], path[320];
    struct InfoData info;
    BPTR lock;
    ULONG i, length;

    if (argc > 2)
        return fail("usage: EXFATHotplugProbe [DEVICE:]", argc, 0);
    length = (ULONG)strlen(device);
    if (length == 0)
        return fail("device name", length, ERROR_LINE_TOO_LONG);
    if (length + (device[length - 1] == ':' ? 0U : 1U)
            + sizeof("AROSUSB/") > sizeof(root))
        return fail("device name", length, ERROR_LINE_TOO_LONG);
    strcpy(root, device);
    if (root[length - 1] != ':')
    {
        root[length++] = ':';
        root[length] = '\0';
    }

    lock = Lock(root, SHARED_LOCK);
    if (lock == BNULL)
        return fail("automounted root", DOSFALSE, IoErr());
    memset(&info, 0, sizeof(info));
    if (!Info(lock, &info))
    {
        SIPTR error = IoErr();
        UnLock(lock);
        return fail("Info", DOSFALSE, error);
    }
    UnLock(lock);
    if (info.id_DiskType != ID_EXFAT_DISK)
        return fail("FATX DiskType", info.id_DiskType, ERROR_NOT_A_DOS_DISK);

    strcpy(root + length, "AROSUSB/");
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        strcpy(path, root);
        strcat(path, expected[i].name);
        if (check_file(path, &expected[i]) != RETURN_OK)
            return RETURN_FAIL;
    }

    strcpy(path, root);
    strcat(path, "Handover.txt");
    if (expect_absent(path) != RETURN_OK)
        return RETURN_FAIL;
    strcpy(path, root);
    strcat(path, "Nested/Source.c");
    if (expect_absent(path) != RETURN_OK)
        return RETURN_FAIL;

    Printf("[EXFATHOTPLUG] PASS %s mounted as FATX with exact payloads\n",
        device);
    return RETURN_OK;
}
