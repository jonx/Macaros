/* Target-side preservation/deletion probe for unknown benign secondaries. */
#include <dos/dos.h>
#include <proto/dos.h>

#include <string.h>

static int fail(CONST_STRPTR stage)
{
    Printf("[EXFATEXT] FAIL %s error %ld\n", stage, IoErr());
    return 20;
}

int main(void)
{
    static const char expected[] = "checksum-corruption payload\n";
    struct DateStamp wanted;
    char data[sizeof(expected)];
    BPTR file;
    LONG got;

    if (!SetProtection("EXFAT8:CorruptDir/CorruptMe.txt", FIBF_ARCHIVE))
        return fail("SetProtection");
    wanted.ds_Days = 900;
    wanted.ds_Minute = 9 * 60 + 41;
    wanted.ds_Tick = 17 * TICKS_PER_SECOND + 2;
    if (!SetFileDate("EXFAT8:CorruptDir/CorruptMe.txt", &wanted))
        return fail("SetFileDate");
    if (!Rename("EXFAT8:CorruptDir/CorruptMe.txt",
            "EXFAT8:CorruptDir/Extended.txt"))
        return fail("Rename");

    file = Open("EXFAT8:CorruptDir/Extended.txt", MODE_OLDFILE);
    if (file == BNULL)
        return fail("Open renamed file");
    memset(data, 0, sizeof(data));
    got = Read(file, data, sizeof(expected) - 1);
    if (!Close(file) || got != (LONG)(sizeof(expected) - 1)
        || memcmp(data, expected, sizeof(expected) - 1) != 0)
        return fail("Read renamed file");

    if (!DeleteFile("EXFAT8:CorruptDir/DeleteMe.txt"))
        return fail("DeleteFile");
    if (!DeleteFile("EXFAT8:BenignDir"))
        return fail("Delete benign-primary directory");
    Printf("[EXFATEXT] PASS\n");
    return 0;
}
