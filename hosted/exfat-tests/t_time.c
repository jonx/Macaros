/* Portable exFAT timestamp and UTC-offset tests against production code. */
#include <stdio.h>
#include <stdint.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t UBYTE;

#include "exfat_time.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    ULONG packed = 0;
    UBYTE ten_ms = 0, raw;
    LONG days, minute, tick, offset, adjustment;

    puts("calendar packing and validation");
    check("Amiga epoch predates exFAT and is refused",
        !exfat_time_pack(0, 0, 0, 50, &packed, &ten_ms));
    check("1980 epoch has the canonical packed value",
        exfat_time_pack(730, 0, 0, 50, &packed, &ten_ms)
            && packed == UINT32_C(0x00210000) && ten_ms == 0);
    check("leap day and maximum sub-minute tick round-trip",
        exfat_time_pack(8094, 23 * 60 + 59, 59 * 50 + 49, 50,
            &packed, &ten_ms)
            && exfat_time_unpack(packed, ten_ms, 50,
                &days, &minute, &tick)
            && days == 8094 && minute == 23 * 60 + 59
            && tick == 59 * 50 + 49 && ten_ms == 198);
    check("invalid February day is rejected",
        !exfat_time_unpack((UINT32_C(20) << 25) | (UINT32_C(2) << 21)
                | (UINT32_C(31) << 16), 0, 50,
            &days, &minute, &tick));
    check("invalid double-second and 10ms fields are rejected",
        !exfat_time_unpack(UINT32_C(0x0021001f), 0, 50,
            &days, &minute, &tick)
            && !exfat_time_unpack(UINT32_C(0x00210000), 200, 50,
                &days, &minute, &tick));

    puts("UTC offset encoding");
    check("positive and negative signed seven-bit offsets decode",
        exfat_time_decode_utc(0x84, &offset) && offset == 60
            && exfat_time_decode_utc(0xec, &offset) && offset == -300
            && exfat_time_decode_utc(0xc0, &offset) && offset == -960);
    check("OffsetValid clear ignores even malformed low bits",
        !exfat_time_decode_utc(0x00, &offset)
            && !exfat_time_decode_utc(0x01, &offset));
    raw = exfat_time_encode_utc(-60, &adjustment);
    check("AROS local-to-GMT sign is reversed for exFAT",
        raw == 0x84 && adjustment == 0);
    raw = exfat_time_encode_utc(300, &adjustment);
    check("western AROS locale encodes a negative exFAT offset",
        raw == 0xec && adjustment == 0);
    raw = exfat_time_encode_utc(-62, &adjustment);
    check("non-quarter-hour locale stores an adjusted UTC timestamp",
        raw == 0x80 && adjustment == -62);
    raw = exfat_time_encode_utc(1000, &adjustment);
    check("out-of-range locale stores an adjusted UTC timestamp",
        raw == 0x80 && adjustment == 1000);
    check("stored New York time converts to current Berlin time",
        exfat_time_display_adjustment(0xec, -60, &adjustment)
            && adjustment == 360);
    check("unknown stored offset retains its local wall time",
        !exfat_time_display_adjustment(0x00, -60, &adjustment));

    puts("day-boundary adjustment");
    days = 1000;
    minute = 15;
    check("negative adjustment crosses to the previous day",
        exfat_time_adjust_minutes(&days, &minute, -30)
            && days == 999 && minute == 1425);
    check("positive adjustment crosses multiple days",
        exfat_time_adjust_minutes(&days, &minute, 2 * 1440 + 30)
            && days == 1002 && minute == 15);
    days = 0;
    minute = 0;
    check("adjustment never underflows the DateStamp epoch",
        !exfat_time_adjust_minutes(&days, &minute, -1));
    days = INT32_MAX;
    minute = 1439;
    check("adjustment never overflows a 32-bit LONG day",
        !exfat_time_adjust_minutes(&days, &minute, 1));

    return failures != 0;
}
