/* Target-side raw boot-region probe: reads the MBR and the exFAT boot
 * regions straight from the storage device and validates them on-screen,
 * so a "Not a DOS disk" verdict can be split into "the device returns
 * corrupt data" versus "the data is fine and the discovery logic is
 * wrong" without a serial console.
 *
 * Usage: EXFATBootRegionProbe [device] [unit]   (usbscsi.device 0)
 */
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdlib.h>
#include <string.h>

#define SECTOR          512UL
#define REGION_SECTORS  12UL

static int failures = 0;

static void report(CONST_STRPTR stage, BOOL ok)
{
    Printf("[BOOTPROBE] %s %s\n", ok ? (CONST_STRPTR)"ok  " :
        (CONST_STRPTR)"FAIL", stage);
    if (!ok)
        failures++;
}

static ULONG le32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16)
        | ((ULONG)p[3] << 24);
}

static void hexline(CONST_STRPTR label, const UBYTE *p, ULONG n)
{
    ULONG i;

    Printf("[BOOTPROBE] %s:", label);
    for (i = 0; i < n; i++)
        Printf(" %02lx", (IPTR)p[i]);
    Printf("\n");
}

static LONG read_bytes(struct IOExtTD *io, APTR buf, ULONG offset,
    ULONG length)
{
    LONG rc;

    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data = buf;
    io->iotd_Req.io_Offset = offset;
    io->iotd_Req.io_Length = length;
    rc = DoIO((struct IORequest *)io);
    if (rc == 0 && io->iotd_Req.io_Actual != length)
        rc = -1;
    return rc;
}

/* The exFAT boot checksum: every byte of the region's first 11 sectors
 * except VolumeFlags (106, 107) and PercentInUse (112). */
static ULONG boot_checksum(const UBYTE *region)
{
    ULONG sum = 0;
    ULONG i;

    for (i = 0; i < 11UL * SECTOR; i++)
    {
        if (i == 106 || i == 107 || i == 112)
            continue;
        sum = ((sum & 1) ? 0x80000000UL : 0UL) + (sum >> 1)
            + (ULONG)region[i];
    }
    return sum;
}

static void check_region(struct IOExtTD *io, CONST_STRPTR name,
    ULONG base_byte)
{
    static UBYTE bulk[REGION_SECTORS * SECTOR];
    static UBYTE single[REGION_SECTORS * SECTOR];
    ULONG stored, computed, s;
    BOOL ok = TRUE;

    /* One 12-sector transfer, then the same sectors one at a time: a
     * difference means corruption that depends on transfer length. */
    if (read_bytes(io, bulk, base_byte, REGION_SECTORS * SECTOR) != 0)
    {
        report(name, FALSE);
        return;
    }
    for (s = 0; s < REGION_SECTORS; s++)
        if (read_bytes(io, single + s * SECTOR, base_byte + s * SECTOR,
            SECTOR) != 0)
            ok = FALSE;
    report("region single-sector reads", ok);
    report("bulk read == single reads",
        memcmp(bulk, single, sizeof(bulk)) == 0);

    hexline("vbr[0..15]", single, 16);
    report("jump+EXFAT signature", single[0] == 0xEB && single[1] == 0x76
        && memcmp(single + 3, "EXFAT   ", 8) == 0);
    report("vbr 55 aa", single[510] == 0x55 && single[511] == 0xAA);
    Printf("[BOOTPROBE] FatOffset %lu ClusterHeapOffset %lu "
        "SectorShift %lu ClusterShift %lu\n",
        (IPTR)le32(single + 80), (IPTR)le32(single + 88),
        (IPTR)single[108], (IPTR)single[109]);

    computed = boot_checksum(single);
    stored = le32(single + 11 * SECTOR);
    Printf("[BOOTPROBE] %s checksum computed %08lx stored %08lx\n",
        name, (IPTR)computed, (IPTR)stored);
    report("checksum match", computed == stored);
}

int main(int argc, char **argv)
{
    CONST_STRPTR device = argc > 1 ? (CONST_STRPTR)argv[1]
        : (CONST_STRPTR)"usbscsi.device";
    ULONG unit = argc > 2 ? (ULONG)atoi(argv[2]) : 0;
    struct MsgPort *port = NULL;
    struct IOExtTD *io = NULL;
    static UBYTE mbr[SECTOR];
    static UBYTE vbr2[SECTOR];
    ULONG pstart = 0;
    ULONG i;
    int rc = 20;

    port = CreateMsgPort();
    if (port == NULL)
        return 20;
    io = (struct IOExtTD *)CreateIORequest(port, sizeof(*io));
    if (io == NULL)
        goto out;
    if (OpenDevice(device, unit, (struct IORequest *)io, 0) != 0)
    {
        Printf("[BOOTPROBE] FAIL OpenDevice %s unit %lu\n", device, (IPTR)unit);
        io->iotd_Req.io_Device = NULL;
        goto out;
    }
    Printf("[BOOTPROBE] %s unit %lu\n", device, (IPTR)unit);

    if (read_bytes(io, mbr, 0, SECTOR) != 0)
    {
        report("mbr read", FALSE);
        goto done;
    }
    report("mbr 55 aa", mbr[510] == 0x55 && mbr[511] == 0xAA);
    for (i = 0; i < 4; i++)
    {
        const UBYTE *e = mbr + 446 + i * 16;

        Printf("[BOOTPROBE] part %lu type %02lx start %lu count %lu\n",
            (IPTR)i, (IPTR)e[4], (IPTR)le32(e + 8), (IPTR)le32(e + 12));
        if (pstart == 0 && e[4] != 0)
            pstart = le32(e + 8);
    }
    if (pstart == 0 || pstart > 0xFFFFFFFFUL / SECTOR)
    {
        report("usable partition start", FALSE);
        goto done;
    }

    check_region(io, "main boot region", pstart * SECTOR);
    check_region(io, "backup boot region", (pstart + 12) * SECTOR);

    /* Same sector read twice: a difference means unstable data. */
    if (read_bytes(io, vbr2, pstart * SECTOR, SECTOR) == 0)
    {
        static UBYTE vbr3[SECTOR];

        if (read_bytes(io, vbr3, pstart * SECTOR, SECTOR) == 0)
            report("vbr read is stable", memcmp(vbr2, vbr3, SECTOR) == 0);
    }

done:
    Printf("[BOOTPROBE] %s\n", failures == 0
        ? (CONST_STRPTR)"PASS: device data is byte-exact exFAT"
        : (CONST_STRPTR)"FAIL: see lines above");
    rc = failures == 0 ? 0 : 20;
    CloseDevice((struct IORequest *)io);
out:
    if (io != NULL)
        DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return rc;
}
