/*
 * Gate 1, m68k half: HEADERS AND CODE GENERATION ONLY.
 *
 * Scope, stated precisely because the previous name overstated it. This
 * compiles exfat_bounds.h and exfat_boot.h for a 32-bit big-endian target
 * with a bare-metal toolchain. It does NOT compile cache.c, disk.c or
 * exfat_fs.h, and it does not use the AROS m68k headers, because no AROS
 * m68k cross-toolchain is installed here. A full m68k source compile remains
 * outstanding and is not claimed by this gate.
 *
 * What it does establish: the pure arithmetic and accessor headers survive
 * 32-bit long, big-endian byte order, and a 68000 that takes an address
 * error rather than a slow path on an odd-address word access.
 *
 * Compile only; there is nothing to run.
 */

typedef unsigned long long UQUAD;
typedef unsigned long      ULONG;
typedef unsigned short     UWORD;
typedef unsigned char      UBYTE;

#include "exfat_bounds.h"
#include "exfat_boot.h"

/* The target really is what we think it is. */
typedef char assert_long_is_32[(sizeof(ULONG) == 4) ? 1 : -1];
typedef char assert_quad_is_64[(sizeof(UQUAD) == 8) ? 1 : -1];
typedef char assert_big_endian[
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) ? 1 : -1];

/*
 * Touch every entry point so none is optimised away before the compiler has
 * had to generate code for it. Returns something derived from all of them so
 * nothing is dead.
 */
ULONG exfat_m68k_check(const UBYTE *boot, const UBYTE *sum_sector,
    UQUAD start, UQUAD total, UQUAD num, ULONG nblocks);

/*
 * Non-inlined probes. The disassembly assertion in build.sh inspects these
 * by name: every memory read through the supplied byte pointer must be a
 * byte load. Inlining them into the function below would let the compiler
 * mix these reads with unrelated ones and make the assertion meaningless.
 */
__attribute__((noinline)) UWORD exfat_probe_rd16(const UBYTE *p, unsigned o);
__attribute__((noinline)) ULONG exfat_probe_rd32(const UBYTE *p, unsigned o);
__attribute__((noinline)) UQUAD exfat_probe_rd64(const UBYTE *p, unsigned o);
__attribute__((noinline)) void exfat_probe_wr16(UBYTE *p, unsigned o, UWORD v);
__attribute__((noinline)) void exfat_probe_wr32(UBYTE *p, unsigned o, ULONG v);
__attribute__((noinline)) void exfat_probe_wr64(UBYTE *p, unsigned o, UQUAD v);

__attribute__((noinline)) UWORD exfat_probe_rd16(const UBYTE *p, unsigned o)
{
    return exfat_rd16(p, o);
}

__attribute__((noinline)) ULONG exfat_probe_rd32(const UBYTE *p, unsigned o)
{
    return exfat_rd32(p, o);
}

__attribute__((noinline)) UQUAD exfat_probe_rd64(const UBYTE *p, unsigned o)
{
    return exfat_rd64(p, o);
}

__attribute__((noinline)) void exfat_probe_wr16(UBYTE *p, unsigned o, UWORD v)
{
    exfat_wr16(p, o, v);
}

__attribute__((noinline)) void exfat_probe_wr32(UBYTE *p, unsigned o, ULONG v)
{
    exfat_wr32(p, o, v);
}

__attribute__((noinline)) void exfat_probe_wr64(UBYTE *p, unsigned o, UQUAD v)
{
    exfat_wr64(p, o, v);
}

ULONG exfat_m68k_check(const UBYTE *boot, const UBYTE *sum_sector,
    UQUAD start, UQUAD total, UQUAD num, ULONG nblocks)
{
    struct exfat_geometry g;
    UQUAD skipped, off;
    ULONG len, sum = 0, i;
    ULONG acc = 0;

    if (!exfat_geometry_ok(start, total))
        return 1;

    acc += (ULONG)(exfat_last_sector(start, total) & 0xFFFFu);

    if (exfat_clip(&num, &nblocks, &skipped, start, total) == EXFAT_RANGE_OK)
        acc += (ULONG)(skipped & 0xFFu) + nblocks;

    if (exfat_byte_range(num, nblocks, 512, &off, &len) == EXFAT_RANGE_OK)
        acc += (ULONG)(off & 0xFFu) + len;

    /*
     * The unaligned-access case. boot is a raw byte buffer whose alignment is
     * whatever the cache allocator gave us, and these offsets are odd on
     * purpose: 3 for the name, and a 64-bit read at 72. Byte-wise accessors
     * must generate byte loads here, not a moveq.l from an odd address.
     */
    acc += exfat_probe_rd16(boot, 3);
    acc += exfat_probe_rd32(boot, 3);
    acc += (ULONG)(exfat_probe_rd64(boot, EXFAT_BOOT_VOLUMELENGTH) & 0xFFFFu);
    acc += (ULONG)(exfat_probe_rd64(boot, 1) & 0xFFu);
    exfat_probe_wr16((UBYTE *)boot, 3, (UWORD)acc);
    exfat_probe_wr32((UBYTE *)boot, 5, acc);
    exfat_probe_wr64((UBYTE *)boot, 9, (UQUAD)acc);

    if (exfat_validate_boot(boot, 512, &g) == EXFAT_BOOT_OK)
        acc += g.cluster_count + g.sector_size;

    for (i = 0; i < EXFAT_BOOT_CHECKSUM_SECTORS; i++)
        sum = exfat_boot_checksum(sum, boot, 512, i);
    acc += sum;

    if (exfat_verify_boot_checksum(sum_sector, 512, sum))
        acc++;

    return acc;
}
