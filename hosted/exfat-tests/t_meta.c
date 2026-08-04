/* Metadata edge tests against the production exfat_meta.h. */
#include <stdio.h>
#include <stdint.h>

typedef uint64_t UQUAD;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;

#include "exfat_boot.h"
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
    ULONG written = 0;
    UWORD out[4] = {0};
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
