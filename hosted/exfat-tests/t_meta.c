/* Metadata edge tests against the production exfat_meta.h. */
#include <stdio.h>
#include <stdint.h>

typedef uint64_t UQUAD;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;

#include "exfat_boot.h"
#include "exfat_cache_bits.h"
#include "exfat_meta.h"

static int failures;
static void check(const char *what, int ok)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    UQUAD n;
    ULONG byte_index;
    ULONG written = 0;
    ULONG i;
    UWORD out[4] = {0};
    UWORD name[] = {'T', 'e', 's', 't', '.', 'b', 'i', 'n'};
    static UWORD upcase[256];
    UBYTE mask;
    ULONG first, count, dirty;
    UBYTE set[96];
    UBYTE encoded[16] = {0};
    UBYTE bitmap[4] = {0};
    const UBYTE table[] = {0x41,0x00, 0xff,0xff, 0x02,0x00, 0xff,0xff};
    const UBYTE overrun[] = {0xff,0xff, 0x05,0x00};

    puts("metadata arithmetic");
    check("zero-length stream has zero clusters",
        exfat_stream_cluster_count(0, 4096, &n) && n == 0);
    check("exact cluster length does not round up",
        exfat_stream_cluster_count(4096, 4096, &n) && n == 1);
    check("one byte over rounds up",
        exfat_stream_cluster_count(4097, 4096, &n) && n == 2);
    check("UQUAD maximum rounds without addition overflow",
        exfat_stream_cluster_count(UINT64_MAX, 4096, &n)
            && n == UINT64_C(4503599627370496));
    check("zero cluster size is refused",
        !exfat_stream_cluster_count(1, 0, &n));
    check("directory growth may reach exactly 256 MiB",
        exfat_directory_can_grow(EXFAT_MAX_DIRECTORY_BYTES - 4096,
            4096, 1));
    check("directory growth beyond 256 MiB is refused",
        !exfat_directory_can_grow(EXFAT_MAX_DIRECTORY_BYTES, 4096, 1)
            && !exfat_directory_can_grow(EXFAT_MAX_DIRECTORY_BYTES + 1,
                4096, 1));
    check("directory growth rejects zero and oversized operands",
        !exfat_directory_can_grow(0, 0, 1)
            && !exfat_directory_can_grow(0, 4096, 0)
            && !exfat_directory_can_grow(0, 0xffffffffUL,
                0xffffffffUL));

    puts("portable metadata writers");
    exfat_wr16(encoded, 1, 0x1234U);
    exfat_wr32(encoded, 3, 0x89abcdefUL);
    exfat_wr64(encoded, 7, UINT64_C(0x0123456789abcdef));
    check("unaligned little-endian writers round-trip",
        exfat_rd16(encoded, 1) == 0x1234U
            && exfat_rd32(encoded, 3) == 0x89abcdefUL
            && exfat_rd64(encoded, 7) == UINT64_C(0x0123456789abcdef));

    for (i = 0; i < sizeof(set); i++)
        set[i] = (UBYTE)i;
    check("three-entry checksum matches independent vector",
        exfat_entry_set_checksum(set, 2) == 0x8086U);

    for (i = 0; i < 256; i++)
        upcase[i] = (UWORD)i;
    for (i = 'a'; i <= 'z'; i++)
        upcase[i] = (UWORD)(i - 'a' + 'A');
    check("name hash uses the supplied volume up-case table",
        exfat_name_hash(upcase, name, sizeof(name) / sizeof(name[0]))
            == 0xc362U);

    check("bitmap maps cluster 2 to bit zero",
        exfat_bitmap_address(2, 17, &byte_index, &mask)
            && byte_index == 0 && mask == 1);
    check("bitmap maps the final cluster without wrapping",
        exfat_bitmap_address(18, 17, &byte_index, &mask)
            && byte_index == 2 && mask == 1);
    check("bitmap rejects clusters outside the heap",
        !exfat_bitmap_address(1, 17, &byte_index, &mask)
            && !exfat_bitmap_address(19, 17, &byte_index, &mask));
    check("bitmap range validation rejects zero and heap overflow",
        !exfat_bitmap_range_valid(2, 0, 17)
            && exfat_bitmap_range_valid(2, 17, 17)
            && !exfat_bitmap_range_valid(2, 18, 17)
            && !exfat_bitmap_range_valid(1, 1, 17));
    exfat_bitmap_set_range(bitmap, 4, 3, 1);
    check("bitmap range mutation changes exactly the requested clusters",
        exfat_bitmap_range_is(bitmap, 4, 3, 17, 1)
            && exfat_bitmap_range_is(bitmap, 2, 2, 17, 0)
            && exfat_bitmap_range_is(bitmap, 7, 12, 17, 0));
    check("free-run planner starts at its hint",
        exfat_bitmap_find_free_run(bitmap, 17, 4, 4, &first)
            && first == 7);
    exfat_bitmap_set_range(bitmap, 7, 12, 1);
    check("free-run planner wraps without joining heap ends",
        exfat_bitmap_find_free_run(bitmap, 17, 15, 2, &first)
            && first == 2
            && !exfat_bitmap_find_free_run(bitmap, 17, 15, 3, &first));
    exfat_bitmap_set_range(bitmap, 4, 3, 0);
    exfat_bitmap_set_range(bitmap, 7, 12, 0);
    check("free-run planner accepts the largest whole-heap run",
        exfat_bitmap_find_free_run(bitmap, 17, 2, 17, &first)
            && first == 2);
    check("PercentInUse is overflow-safe and rounded down",
        exfat_percent_in_use(17, 17) == 0
            && exfat_percent_in_use(8, 17) == 52
            && exfat_percent_in_use(0, 0xffffffffUL) == 100
            && exfat_percent_in_use(18, 17) == 0xff);

    puts("cache write ordering");
    check("a fully dirty cache line is one bounded 32-sector span",
        exfat_dirty_span(0xffffffffUL, &first, &count)
            && first == 0 && count == 32
            && exfat_dirty_span_mask(first, count) == 0xffffffffUL);
    dirty = 0x8000000dUL;
    check("disjoint dirty sectors are not merged across clean sectors",
        exfat_dirty_span(dirty, &first, &count)
            && first == 0 && count == 1
            && (dirty &= ~exfat_dirty_span_mask(first, count)) == 0x8000000cUL
            && exfat_dirty_span(dirty, &first, &count)
            && first == 2 && count == 2
            && (dirty &= ~exfat_dirty_span_mask(first, count)) == 0x80000000UL
            && exfat_dirty_span(dirty, &first, &count)
            && first == 31 && count == 1
            && (dirty &= ~exfat_dirty_span_mask(first, count)) == 0);
    check("a clean cache line yields no write span",
        !exfat_dirty_span(0, &first, &count));

    puts("up-case decompression");
    check("identity run plus terminal U+FFFF expands",
        exfat_expand_upcase(table, sizeof(table), out, 4, &written)
            && written == 4 && out[0] == 0x41 && out[1] == 1
            && out[2] == 2 && out[3] == 0xffff);
    check("odd byte length is refused",
        !exfat_expand_upcase(table, sizeof(table) - 1, out, 4, &written));
    check("identity run beyond table capacity is refused",
        !exfat_expand_upcase(overrun, sizeof(overrun), out, 4, &written));

    return failures != 0;
}
