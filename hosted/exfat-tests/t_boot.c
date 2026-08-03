/*
 * Boot sector validation tests, against the production exfat_boot.h.
 *
 * Covers spec.md 3.1 (accepted/rejected conditions, G1 ordering) and 3.2
 * (boot region checksum, including B3: every repeated word).
 *
 *     ./build.sh && ./t_boot
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t UQUAD;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t  UBYTE;

#include "exfat_boot.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static void wr16(UBYTE *p, unsigned off, UWORD v)
{
    p[off] = (UBYTE)(v & 0xFF); p[off + 1] = (UBYTE)(v >> 8);
}

static void wr32(UBYTE *p, unsigned off, ULONG v)
{
    unsigned i;
    for (i = 0; i < 4; i++) p[off + i] = (UBYTE)((v >> (i * 8)) & 0xFF);
}

static void wr64(UBYTE *p, unsigned off, UQUAD v)
{
    unsigned i;
    for (i = 0; i < 8; i++) p[off + i] = (UBYTE)((v >> (i * 8)) & 0xFF);
}

/*
 * A minimal but self-consistent 64 MiB volume: 512-byte sectors, 8 sectors
 * per cluster, FAT at sector 24.
 */
static void make_good(UBYTE *b)
{
    ULONG volume_sectors = 131072;      /* 64 MiB */
    ULONG heap = 1024;
    ULONG clusters = (volume_sectors - heap) >> 3;

    memset(b, 0, 512);
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    memcpy(b + EXFAT_BOOT_FSNAME, "EXFAT   ", 8);
    wr64(b, EXFAT_BOOT_VOLUMELENGTH, volume_sectors);
    wr32(b, EXFAT_BOOT_FATOFFSET,    24);
    wr32(b, EXFAT_BOOT_FATLENGTH,    heap - 24);
    wr32(b, EXFAT_BOOT_HEAPOFFSET,   heap);
    wr32(b, EXFAT_BOOT_CLUSTERCOUNT, clusters);
    wr32(b, EXFAT_BOOT_ROOTCLUSTER,  2);
    wr16(b, EXFAT_BOOT_REVISION,     0x0100);
    b[EXFAT_BOOT_SECTORSHIFT]  = 9;
    b[EXFAT_BOOT_CLUSTERSHIFT] = 3;
    b[EXFAT_BOOT_NUMBEROFFATS] = 1;
    b[EXFAT_BOOT_PERCENTINUSE] = 50;
    wr16(b, EXFAT_BOOT_SIGNATURE, 0xAA55);
}

#define EXPECT(desc, mutate, want)                                       \
    do {                                                                 \
        UBYTE b[512]; struct exfat_geometry g;                           \
        make_good(b); { mutate; }                                        \
        check(desc, exfat_validate_boot(b, sizeof b, &g) == (want));     \
    } while (0)

static void test_accept(void)
{
    UBYTE b[512];
    struct exfat_geometry g;

    puts("3.1  a well-formed volume");
    make_good(b);
    check("accepted", exfat_validate_boot(b, sizeof b, &g) == EXFAT_BOOT_OK);
    check("geometry read back correctly",
        g.sector_size == 512 && g.cluster_shift == 3
        && g.volume_length == 131072 && g.heap_offset == 1024
        && g.root_cluster == 2 && g.percent_in_use == 50);
    {
        /* 4096-byte sectors: the FAT covers four times as many entries per
           sector, so the same geometry is still self-consistent. */
        UBYTE c[512];
        struct exfat_geometry g2;

        make_good(c);
        c[EXFAT_BOOT_SECTORSHIFT] = 12;
        check("4096-byte sectors accepted",
            exfat_validate_boot(c, sizeof c, &g2) == EXFAT_BOOT_OK
            && g2.sector_size == 4096);
    }
}

static void test_identity(void)
{
    puts("\n3.1  identity");
    EXPECT("wrong JumpBoot rejected", b[1] = 0x00, EXFAT_BOOT_NOT_EXFAT);
    EXPECT("wrong FileSystemName rejected",
        memcpy(b + EXFAT_BOOT_FSNAME, "NTFS    ", 8), EXFAT_BOOT_NOT_EXFAT);
    EXPECT("FileSystemName without the trailing spaces rejected",
        memcpy(b + EXFAT_BOOT_FSNAME, "EXFAT\0\0\0", 8), EXFAT_BOOT_NOT_EXFAT);
    EXPECT("non-zero MustBeZero rejected",
        b[EXFAT_BOOT_MUSTBEZERO + 20] = 1, EXFAT_BOOT_NOT_EXFAT);
    EXPECT("missing boot signature rejected",
        wr16(b, EXFAT_BOOT_SIGNATURE, 0), EXFAT_BOOT_NOT_EXFAT);
    {
        UBYTE b[512];
        struct exfat_geometry g;

        make_good(b);
        check("buffer shorter than a sector rejected",
            exfat_validate_boot(b, 511, &g) == EXFAT_BOOT_NOT_EXFAT);
    }
}

static void test_version_and_texfat(void)
{
    puts("\n1.1, 1.2  revision and TexFAT");
    EXPECT("revision 2.00 rejected",
        wr16(b, EXFAT_BOOT_REVISION, 0x0200), EXFAT_BOOT_WRONG_VERSION);
    EXPECT("revision 1.01 rejected (exact 1.00 policy)",
        wr16(b, EXFAT_BOOT_REVISION, 0x0101), EXFAT_BOOT_WRONG_VERSION);
    EXPECT("NumberOfFats = 2 is TexFAT, refused",
        b[EXFAT_BOOT_NUMBEROFFATS] = 2, EXFAT_BOOT_TEXFAT);
    EXPECT("NumberOfFats = 0 rejected",
        b[EXFAT_BOOT_NUMBEROFFATS] = 0, EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("ActiveFat set with a single FAT rejected",
        wr16(b, EXFAT_BOOT_VOLUMEFLAGS, EXFAT_VOLUMEFLAG_ACTIVEFAT),
        EXFAT_BOOT_WRONG_VERSION);
}

static void test_geometry(void)
{
    puts("\n3.1  geometry bounds");
    EXPECT("sector shift 8 rejected",
        b[EXFAT_BOOT_SECTORSHIFT] = 8, EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("sector shift 13 rejected",
        b[EXFAT_BOOT_SECTORSHIFT] = 13, EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("cluster larger than 32 MiB rejected",
        b[EXFAT_BOOT_CLUSTERSHIFT] = 17, EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("PercentInUse 101 rejected",
        b[EXFAT_BOOT_PERCENTINUSE] = 101, EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("PercentInUse 0xFF accepted as unknown",
        b[EXFAT_BOOT_PERCENTINUSE] = 0xFF, EXFAT_BOOT_OK);
    EXPECT("volume smaller than 1 MiB rejected",
        wr64(b, EXFAT_BOOT_VOLUMELENGTH, 100), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("FAT starting inside the boot region rejected",
        wr32(b, EXFAT_BOOT_FATOFFSET, 23), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("heap before the FAT rejected",
        wr32(b, EXFAT_BOOT_HEAPOFFSET, 20), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("FAT overlapping the heap rejected",
        wr32(b, EXFAT_BOOT_FATLENGTH, 2000), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("heap beyond the volume rejected",
        wr32(b, EXFAT_BOOT_HEAPOFFSET, 200000), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("FAT too short for ClusterCount rejected",
        wr32(b, EXFAT_BOOT_FATLENGTH, 1), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("ClusterCount disagreeing with the heap rejected",
        wr32(b, EXFAT_BOOT_CLUSTERCOUNT, 100), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("root cluster 1 rejected",
        wr32(b, EXFAT_BOOT_ROOTCLUSTER, 1), EXFAT_BOOT_BAD_GEOMETRY);
    EXPECT("root cluster past the heap rejected",
        wr32(b, EXFAT_BOOT_ROOTCLUSTER, 0xFFFFFF), EXFAT_BOOT_BAD_GEOMETRY);
}

static void test_ordering(void)
{
    UBYTE b[512];
    struct exfat_geometry g;

    puts("\nG1  validation order");

    /*
     * A volume that is both not-exFAT and has impossible geometry must report
     * the identity failure, not the geometry one: everything below the
     * identity check is meaningless if this is not an exFAT volume at all.
     */
    make_good(b);
    memcpy(b + EXFAT_BOOT_FSNAME, "NTFS    ", 8);
    b[EXFAT_BOOT_SECTORSHIFT] = 99;
    check("identity is checked before geometry",
        exfat_validate_boot(b, sizeof b, &g) == EXFAT_BOOT_NOT_EXFAT);

    /* Likewise TexFAT is reported before any geometry complaint. */
    make_good(b);
    b[EXFAT_BOOT_NUMBEROFFATS] = 2;
    wr32(b, EXFAT_BOOT_ROOTCLUSTER, 0);
    check("TexFAT is reported before geometry",
        exfat_validate_boot(b, sizeof b, &g) == EXFAT_BOOT_TEXFAT);
}

/* Sectors 1..10 of the boot region: not the boot sector, so no exclusions. */
static void make_filler(UBYTE *sec, int n)
{
    int i;
    for (i = 0; i < 512; i++)
        sec[i] = (UBYTE)((i * 7 + n * 31) & 0xFF);
}

static ULONG boot_region_sum(const UBYTE *sector0)
{
    UBYTE filler[512];
    ULONG sum = 0;
    int i;

    sum = exfat_boot_checksum(sum, sector0, 512, 1);
    for (i = 1; i < 11; i++)
    {
        make_filler(filler, i);
        sum = exfat_boot_checksum(sum, filler, 512, 0);
    }
    return sum;
}

static void test_checksum(void)
{
    UBYTE sec[512];
    ULONG base;

    puts("\n3.2  boot region checksum");

    make_good(sec);
    base = boot_region_sum(sec);

    /* VolumeFlags and PercentInUse mutate in normal use and are excluded. */
    make_good(sec);
    wr16(sec, EXFAT_BOOT_VOLUMEFLAGS, 0x0002);
    sec[EXFAT_BOOT_PERCENTINUSE] = 99;
    check("VolumeFlags and PercentInUse are excluded",
        boot_region_sum(sec) == base);

    /* The byte immediately after the excluded PercentInUse is not excluded. */
    make_good(sec);
    sec[EXFAT_BOOT_PERCENTINUSE + 1] ^= 0xFF;
    check("the byte after PercentInUse is still covered",
        boot_region_sum(sec) != base);

    /* Any other change must be caught. */
    make_good(sec);
    sec[EXFAT_BOOT_VOLUMESERIAL] ^= 0xFF;
    check("a change elsewhere in sector 0 is caught",
        boot_region_sum(sec) != base);

    /* A change in one of the following sectors must be caught too. */
    {
        UBYTE filler[512];
        ULONG sum = 0;
        int i;

        make_good(sec);
        sum = exfat_boot_checksum(sum, sec, 512, 1);
        for (i = 1; i < 11; i++)
        {
            make_filler(filler, i);
            if (i == 5)
                filler[100] ^= 0xFF;
            sum = exfat_boot_checksum(sum, filler, 512, 0);
        }
        check("a change in sector 5 is caught", sum != base);
    }

    /* The exclusion is positional and applies only to sector 0. */
    {
        UBYTE filler[512];
        ULONG sum = 0;
        int i;

        make_good(sec);
        sum = exfat_boot_checksum(sum, sec, 512, 1);
        for (i = 1; i < 11; i++)
        {
            make_filler(filler, i);
            if (i == 3)
                filler[EXFAT_BOOT_PERCENTINUSE] ^= 0xFF;
            sum = exfat_boot_checksum(sum, filler, 512, 0);
        }
        check("the same offset in a later sector is NOT excluded", sum != base);
    }
}

int main(void)
{
    test_accept();
    test_identity();
    test_version_and_texfat();
    test_geometry();
    test_ordering();
    test_checksum();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASS",
        failures, failures == 1 ? "" : "s");
    return failures != 0;
}
