/* Target-side T1 probe: a clean writable volume reports plausible info. */
#include <dos/dos.h>
#include <proto/dos.h>

#define ID_EXFAT_DISK 0x46415458UL

static int fail(CONST_STRPTR what, SIPTR got)
{
    Printf("[EXFATGEO] FAIL %s (%ld, IoErr %ld)\n", what, got, IoErr());
    return 20;
}

int main(void)
{
    struct InfoData info;
    BPTR lock = Lock("EXFATG:", ACCESS_READ);

    if (lock == BNULL)
        return fail("Lock", 0);
    if (!Info(lock, &info))
    {
        UnLock(lock);
        return fail("Info", 0);
    }
    UnLock(lock);

    if (info.id_DiskType != ID_EXFAT_DISK)
        return fail("disk type", info.id_DiskType);
    if (info.id_DiskState != ID_VALIDATED)
        return fail("disk state", info.id_DiskState);
    if (info.id_NumBlocks <= 0 || info.id_NumBlocksUsed <= 0
        || info.id_NumBlocksUsed >= info.id_NumBlocks)
        return fail("block counts", info.id_NumBlocksUsed);
    if (info.id_BytesPerBlock < 512
        || (info.id_BytesPerBlock & (info.id_BytesPerBlock - 1)) != 0)
        return fail("bytes per block", info.id_BytesPerBlock);

    Printf("[EXFATGEO] PASS blocks %ld used %ld bytes-per-block %ld\n",
           info.id_NumBlocks, info.id_NumBlocksUsed, info.id_BytesPerBlock);
    return 0;
}
