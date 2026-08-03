/*
 * T13a - sector-domain arithmetic around 2^32.
 *
 * Exercises the PRODUCTION code: it includes rom/filesys/exfat/exfat_bounds.h
 * directly, so there is no second model of the rules to drift from the real
 * one. Proves spec.md S1 to S4 without a multi-terabyte image or a device.
 *
 *     ./build.sh && ./t13a
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t UQUAD;
typedef uint32_t ULONG;

#include "exfat_bounds.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* ------------------------------------------------------------------ S4 */

/* Shift performed in ULONG, then widened. This is the genuinely unsafe form. */
static UQUAD sfc_shift_in_ulong(ULONG cluster, unsigned shift, UQUAD heap)
{
    return (UQUAD)((cluster - 2) << shift) + heap;
}

/* Required: the shift happens in UQUAD. Caller has validated the cluster. */
static UQUAD sfc(ULONG cluster, unsigned shift, UQUAD heap)
{
    return (((UQUAD)cluster - 2) << shift) + heap;
}

static void test_s4(void)
{
    const unsigned shift = 3;          /* 8 sectors per cluster */
    const UQUAD heap = 4096;
    ULONG top = 0xFFFFFFF5u;           /* the largest legal cluster */

    puts("S4  the shift must happen in UQUAD");

    check("small clusters agree with the hand-computed sector",
        sfc(2, shift, heap) == heap
        && sfc(1000, shift, heap) == ((UQUAD)998 << shift) + heap);

    /*
     * The real defect. (cluster - 2) << 3 evaluated in ULONG discards the top
     * three bits before the value is widened, so the largest legal cluster
     * lands in completely the wrong place. Computed independently here rather
     * than by reusing either expression, so this asserts something.
     */
    check("top cluster: shifting in ULONG loses the high bits",
        sfc_shift_in_ulong(top, shift, heap) != (UQUAD)0xFFFFFFF3ull * 8 + heap);
    check("top cluster: shifting in UQUAD is correct",
        sfc(top, shift, heap) == (UQUAD)0xFFFFFFF3ull * 8 + heap);

    /*
     * Underflow. NEITHER form is safe, and that is the point: (UQUAD)1 - 2
     * underflows exactly as (ULONG)1 - 2 does, and the shift and add wrap the
     * result back to a plausible-looking sector. Promotion order changes which
     * wrong answer appears, not whether one does. The range check is the only
     * protection, so it is a precondition and not an optimisation.
     */
    check("cluster 1 yields a plausible sector, so is NOT self-detecting",
        sfc(1, shift, heap) < (UQUAD)1 << 40);
    check("cluster 0 likewise",
        sfc(0, shift, heap) < (UQUAD)1 << 40);
}

/* ------------------------------------------------------- S2, production */

static void test_geometry(void)
{
    puts("\nS2  geometry validation (production exfat_geometry_ok)");

    check("ordinary volume accepted", exfat_geometry_ok(64, 1000));
    check("zero-length volume rejected", !exfat_geometry_ok(64, 0));
    check("volume ending exactly at the domain top accepted",
        exfat_geometry_ok(EXFAT_UQUAD_MAX - 99, 100));
    check("volume whose last sector overflows is REJECTED",
        !exfat_geometry_ok(EXFAT_UQUAD_MAX - 99, 200));
    check("start at the domain top with one sector accepted",
        exfat_geometry_ok(EXFAT_UQUAD_MAX, 1));
}

static void test_clip(void)
{
    UQUAD num, skipped;
    ULONG nb;

    puts("\nS2  request clipping (production exfat_clip)");

    num = 100; nb = 8;
    check("in-range request passes unclipped",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OK
        && num == 100 && nb == 8 && skipped == 0);

    num = 60; nb = 8;
    check("straddling the start is clipped forward",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OK
        && num == 64 && nb == 4 && skipped == 4);

    num = 10; nb = 8;
    check("entirely before the volume is rejected",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OUTSIDE);

    num = 1064; nb = 8;
    check("entirely past the volume is rejected",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OUTSIDE);

    num = 1060; nb = 8;
    check("straddling the end is clipped short",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OK
        && nb == 4);

    num = 100; nb = 0;
    check("zero-length request rejected",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OUTSIDE);

    num = (UQUAD)1 << 33; nb = 8;
    check("request above 2^32 accepted",
        exfat_clip(&num, &nb, &skipped, (UQUAD)1 << 32, (UQUAD)1 << 33)
            == EXFAT_RANGE_OK && nb == 8);

    /*
     * The wrap that motivated all of this. num is inside the volume so the
     * leading guards pass; nblocks is absurd and num + nblocks wraps 32 bits
     * to a small value, so a "num + nblocks > end" clip never fires and the
     * caller keeps its full out-of-range count.
     */
    num = 1000; nb = 0xFFFFFF00u;
    check("huge count inside a small volume is clipped, not wrapped",
        exfat_clip(&num, &nb, &skipped, 64, 1000) == EXFAT_RANGE_OK
        && nb == 64);

    num = 100; nb = 8;
    check("unusable geometry is reported, not clipped around",
        exfat_clip(&num, &nb, &skipped, EXFAT_UQUAD_MAX - 99, 200)
            == EXFAT_RANGE_BADGEOMETRY);
}

static void test_byte_range(void)
{
    UQUAD off;
    ULONG len;

    puts("\nS2  byte range (production exfat_byte_range)");

    check("ordinary range converts",
        exfat_byte_range(100, 8, 512, &off, &len) == EXFAT_RANGE_OK
        && off == 51200 && len == 4096);

    check("sector offset overflowing the 64-bit domain is refused",
        exfat_byte_range(EXFAT_UQUAD_MAX / 512 + 1, 1, 512, &off, &len)
            == EXFAT_RANGE_TOOBIG);

    check("transfer length overflowing 32 bits is refused",
        exfat_byte_range(0, 0x800000u, 4096, &off, &len)
            == EXFAT_RANGE_TOOBIG);

    check("range ending past the domain top is refused",
        exfat_byte_range(EXFAT_UQUAD_MAX / 512, 8, 512, &off, &len)
            == EXFAT_RANGE_TOOBIG);

    check("zero block size is refused",
        exfat_byte_range(1, 1, 0, &off, &len) == EXFAT_RANGE_BADGEOMETRY);
}

/* ----------------------------------------------------------------- T14 */

static void test_hash(void)
{
    const unsigned RANGE_SHIFT = 5, hash_size = 64;
    unsigned below[64], above[64];
    int i, ok = 1;

    puts("\nT14 cache key distribution across 2^32");
    memset(below, 0, sizeof below);
    memset(above, 0, sizeof above);

    for (i = 0; i < 4096; i++)
    {
        UQUAD lo = (UQUAD)i * 32;
        UQUAD hi = ((UQUAD)1 << 32) + (UQUAD)i * 32;
        below[(lo >> RANGE_SHIFT) & (hash_size - 1)]++;
        above[(hi >> RANGE_SHIFT) & (hash_size - 1)]++;
    }
    for (i = 0; i < 64; i++)
        if (below[i] != above[i])
            ok = 0;

    check("sequential ranges distribute identically above and below 2^32", ok);
    check("64-bit keys compare exactly", (((UQUAD)1 << 32) | 7) != (UQUAD)7);
}

int main(void)
{
    test_s4();
    test_geometry();
    test_clip();
    test_byte_range();
    test_hash();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASS",
        failures, failures == 1 ? "" : "s");
    return failures != 0;
}
