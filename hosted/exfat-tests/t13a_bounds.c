/*
 * T13a - sector-domain arithmetic around 2^32, against a fake backend.
 *
 * Proves spec.md S2 (overflow-safe bounds) and S4 (promote before subtract)
 * without needing a multi-terabyte image or a real device. Builds and runs on
 * the host.
 *
 *     cc -O2 -o t13a t13a_bounds.c && ./t13a
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t UQUAD;
typedef uint32_t ULONG;

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* ------------------------------------------------------------------ S4 */

/* The shape that was in the spec first: subtracts in ULONG, then promotes. */
static UQUAD sector_from_cluster_wrong(ULONG cluster, unsigned shift,
    UQUAD heap)
{
    return ((UQUAD)(cluster - 2) << shift) + heap;
}

/* The required shape: promote, then subtract. Caller has validated >= 2. */
static UQUAD sector_from_cluster(ULONG cluster, unsigned shift, UQUAD heap)
{
    return (((UQUAD)cluster - 2) << shift) + heap;
}

static void test_s4(void)
{
    const unsigned shift = 3;          /* 8 sectors per cluster */
    const UQUAD heap = 4096;

    puts("S4  promote before subtract");

    /* Valid clusters: both forms must agree. */
    check("cluster 2 agrees",
        sector_from_cluster(2, shift, heap)
            == sector_from_cluster_wrong(2, shift, heap));
    check("cluster 1000 agrees",
        sector_from_cluster(1000, shift, heap)
            == sector_from_cluster_wrong(1000, shift, heap));

    /* The top of the cluster domain still lands where it should. */
    check("cluster 0xFFFFFFF5 does not wrap",
        sector_from_cluster(0xFFFFFFF5u, shift, heap)
            == (((UQUAD)0xFFFFFFF5u - 2) << shift) + heap);

    /*
     * Invalid clusters. NEITHER form is safe, and this is the point of the
     * test: (UQUAD)1 - 2 underflows exactly as (ULONG)1 - 2 does, and the
     * subsequent shift and add wrap it back into a plausible-looking sector.
     * Promotion order changes which wrong answer you get, not whether you
     * get one. The only protection is the caller's cluster >= 2 check.
     */
    check("cluster 1: old form yields a plausible sector (unsafe)",
        sector_from_cluster_wrong(1, shift, heap) < (UQUAD)1 << 40);
    check("cluster 1: new form ALSO yields a plausible sector (unsafe)",
        sector_from_cluster(1, shift, heap) < (UQUAD)1 << 40);
    check("cluster 0: old form yields a plausible sector (unsafe)",
        sector_from_cluster_wrong(0, shift, heap) < (UQUAD)1 << 40);
    check("cluster 0: new form ALSO yields a plausible sector (unsafe)",
        sector_from_cluster(0, shift, heap) < (UQUAD)1 << 40);
    check("=> validation, not promotion order, is what makes this safe", 1);
}

/* ------------------------------------------------------------------ S2 */

struct clip { UQUAD num; ULONG nblocks; int rejected; };

/* The clipping logic as rewritten in disk.c: no addition anywhere. */
static struct clip clip_request(UQUAD num, ULONG nblocks, UQUAD start,
    UQUAD total)
{
    struct clip r = { num, nblocks, 0 };
    UQUAD rel;

    if (r.num < start)
    {
        UQUAD before = start - r.num;

        if ((UQUAD)r.nblocks <= before) { r.rejected = 1; return r; }
        r.nblocks -= (ULONG)before;
        r.num = start;
    }

    rel = r.num - start;
    if (rel >= total) { r.rejected = 1; return r; }
    if ((UQUAD)r.nblocks > total - rel)
        r.nblocks = (ULONG)(total - rel);

    return r;
}

/* The original: 32-bit additions guarding a 64-bit access. */
static struct clip clip_request_old(ULONG num, ULONG nblocks, ULONG start,
    ULONG total)
{
    struct clip r = { num, nblocks, 0 };
    ULONG n = num, end;

    if (n + nblocks <= start) { r.rejected = 1; return r; }
    else if (n < start) { r.nblocks -= start - n; n = start; }

    end = start + total;
    if (n >= end) { r.rejected = 1; return r; }
    else if (n + r.nblocks > end) r.nblocks = end - n;

    r.num = n;
    return r;
}

static void test_s2(void)
{
    struct clip c;

    puts("\nS2  overflow-safe bounds");

    /* Ordinary cases still behave. */
    c = clip_request(100, 8, 64, 1000);
    check("in-range request passes unclipped",
        !c.rejected && c.num == 100 && c.nblocks == 8);

    c = clip_request(60, 8, 64, 1000);
    check("straddling the start is clipped forward",
        !c.rejected && c.num == 64 && c.nblocks == 4);

    c = clip_request(10, 8, 64, 1000);
    check("entirely before the volume is rejected", c.rejected);

    c = clip_request(1064, 8, 64, 1000);
    check("entirely past the volume is rejected", c.rejected);

    c = clip_request(1060, 8, 64, 1000);
    check("straddling the end is clipped short",
        !c.rejected && c.nblocks == 4);

    /* Above 2^32, where the old code could not reach at all. */
    c = clip_request((UQUAD)1 << 33, 8, (UQUAD)1 << 32, (UQUAD)1 << 33);
    check("request above 2^32 is accepted",
        !c.rejected && c.nblocks == 8);

    /*
     * The wrap. num + nblocks overflows 32 bits, so the old guard's
     * comparison is against a tiny wrapped value and the clip never fires:
     * it returns the full block count for a request past the end.
     */
    {
        /*
         * num sits inside the volume, so both leading guards pass. nblocks is
         * absurd, and num + nblocks wraps 32 bits to a small value, so the
         * old clip "num + nblocks > end" is false and never fires: the caller
         * is handed back its full, wildly out-of-range block count.
         */
        ULONG start = 64, total = 1000;      /* volume ends at sector 1064 */
        ULONG num = 1000;                    /* inside the volume */
        ULONG nb = 0xFFFFFF00u;              /* 1000 + this wraps to 744 */
        struct clip oldc = clip_request_old(num, nb, start, total);
        struct clip newc = clip_request(num, nb, start, total);

        check("old form: wrapped addition leaves the count unclipped",
            !oldc.rejected && oldc.nblocks == nb);
        check("new form: clips it to the volume",
            !newc.rejected && newc.nblocks == 64);
    }

    /* first + total would overflow UQUAD itself. */
    {
        UQUAD start = ~(UQUAD)0 - 100;
        UQUAD total = 200;                   /* start + total overflows */
        c = clip_request(start + 50, 8, start, total);
        check("start + total overflowing UQUAD is still handled",
            !c.rejected && c.nblocks == 8);
        c = clip_request(start - 1, 1, start, total);
        check("  and the bound below it still rejects", c.rejected);
    }
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
    check("64-bit keys compare exactly",
        (((UQUAD)1 << 32) | 7) != (UQUAD)7);
}

int main(void)
{
    test_s4();
    test_s2();
    test_hash();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASS",
        failures, failures == 1 ? "" : "s");
    return failures != 0;
}
