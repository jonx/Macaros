/* Handwritten utility.library semantics over guest-owned memory. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TAG_DONE   0u
#define TAG_IGNORE 1u
#define TAG_MORE   2u
#define TAG_SKIP   3u
#define ANO_NAMESPACE 4000u
#define ANO_USERSPACE 4001u
#define ANO_PRIORITY  4002u
#define ANO_FLAGS     4003u
#define NSF_NODUPS    1u
#define NSF_CASE      2u

static int span_ok(j4_sandbox *sb, uint32_t p, uint32_t n)
{
    return p >= sb->sandbox_origin &&
           (uint64_t)p + n <= (uint64_t)sb->sandbox_origin + sb->size;
}

/* Advance a local cursor through the complete TAG_MORE/TAG_SKIP language. */
static uint32_t next_item(j4_sandbox *sb, uint32_t *cursor)
{
    uint32_t p = *cursor;
    for (unsigned guard = 0; p && guard < 65536u; guard++) {
        uint32_t tag, data;
        if (!span_ok(sb, p, 8u)) break;
        tag = emu68k_gread32(sb, p);
        data = emu68k_gread32(sb, p + 4u);
        if (tag == TAG_DONE) { p = 0; break; }
        if (tag == TAG_IGNORE) { p += 8u; continue; }
        if (tag == TAG_MORE) { p = data; continue; }
        if (tag == TAG_SKIP) {
            if (data > 0x1fffffffu) { p = 0; break; }
            p += 8u * (data + 1u);
            continue;
        }
        *cursor = p + 8u;
        return p;
    }
    *cursor = 0;
    return 0;
}

static uint32_t utility_next_tag(j4_sandbox *sb, uint32_t statep)
{
    uint32_t cursor, item;
    if (!span_ok(sb, statep, 4u)) return 0;
    cursor = emu68k_gread32(sb, statep);
    item = next_item(sb, &cursor);
    emu68k_gwrite32(sb, statep, cursor);
    return item;
}

static uint32_t utility_find_tag(j4_sandbox *sb, uint32_t wanted, uint32_t list)
{
    uint32_t item;
    while ((item = next_item(sb, &list)) != 0)
        if (emu68k_gread32(sb, item) == wanted) return item;
    return 0;
}

static int tag_in_array(j4_sandbox *sb, uint32_t wanted, uint32_t array)
{
    for (unsigned guard = 0; array && guard < 65536u; guard++, array += 4u) {
        uint32_t tag;
        if (!span_ok(sb, array, 4u)) return 0;
        tag = emu68k_gread32(sb, array);
        if (tag == TAG_DONE) return 0;
        if (tag == wanted) return 1;
    }
    return 0;
}

static int named_index(struct emu68k_run *r, uint32_t object)
{
    if (!object) return -1;
    for (int i = 0; i < 128; i++)
        if (r->named[i].live && r->named[i].object == object) return i;
    return -1;
}

static int named_equal(const char *a, const char *b, uint8_t flags)
{
    return flags & NSF_CASE ? strcmp(a, b) == 0 : strcasecmp(a, b) == 0;
}

static uint8_t named_namespace_flags(struct emu68k_run *r, uint32_t parent)
{
    int i = named_index(r, parent);
    return i >= 0 ? r->named[i].flags : NSF_NODUPS;
}

static void named_remove_if_ready(struct emu68k_run *r, int i)
{
    if (i >= 0 && r->named[i].live && r->named[i].pending_remove &&
        r->named[i].refs == 0) {
        r->named[i].added = 0;
        r->named[i].parent = 0;
        r->named[i].pending_remove = 0;
    }
}

static int leap_year(uint32_t year)
{
    return (year % 400u == 0u) || (year % 4u == 0u && year % 100u != 0u);
}

static unsigned month_days(uint32_t year, unsigned month)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1u] + (month == 2u && leap_year(year));
}

static uint32_t clock_read(j4_sandbox *sb, uint32_t p, uint32_t off)
{
    return emu68k_gread16(sb, p + off);
}

static void clock_write(j4_sandbox *sb, uint32_t p, uint32_t off, uint32_t v)
{
    emu68k_gwrite16(sb, p + off, v);
}

static uint32_t date_to_amiga(j4_sandbox *sb, uint32_t p, int validate)
{
    uint32_t sec, min, hour, mday, month, year, wday, days = 0;
    if (!span_ok(sb, p, M68K_ClockData_SIZEOF)) return 0;
    sec = clock_read(sb, p, M68K_ClockData_sec);
    min = clock_read(sb, p, M68K_ClockData_min);
    hour = clock_read(sb, p, M68K_ClockData_hour);
    mday = clock_read(sb, p, M68K_ClockData_mday);
    month = clock_read(sb, p, M68K_ClockData_month);
    year = clock_read(sb, p, M68K_ClockData_year);
    wday = clock_read(sb, p, M68K_ClockData_wday);
    if (validate && (sec > 60u || min > 59u || hour > 23u || year < 1978u ||
                     wday > 6u || month < 1u || month > 12u || mday < 1u ||
                     mday > month_days(year, month))) return 0;
    for (uint32_t y = 1978; y < year; y++) days += leap_year(y) ? 366u : 365u;
    for (uint32_t m = 1; m < month; m++) days += month_days(year, m);
    days += mday - 1u;
    return days * 86400u + hour * 3600u + min * 60u + sec;
}

static void amiga_to_date(j4_sandbox *sb, uint32_t seconds, uint32_t p)
{
    uint32_t days = seconds / 86400u, rem = seconds % 86400u;
    uint32_t year = 1978u, month = 1u, original_days = days;
    if (!span_ok(sb, p, M68K_ClockData_SIZEOF)) return;
    while (days >= (uint32_t)(leap_year(year) ? 366u : 365u))
        days -= leap_year(year++) ? 366u : 365u;
    while (days >= month_days(year, month)) days -= month_days(year, month++);
    clock_write(sb, p, M68K_ClockData_sec, rem % 60u);
    clock_write(sb, p, M68K_ClockData_min, (rem / 60u) % 60u);
    clock_write(sb, p, M68K_ClockData_hour, rem / 3600u);
    clock_write(sb, p, M68K_ClockData_mday, days + 1u);
    clock_write(sb, p, M68K_ClockData_month, month);
    clock_write(sb, p, M68K_ClockData_year, year);
    clock_write(sb, p, M68K_ClockData_wday, original_days % 7u);
}

static int pack_structure(j4_sandbox *sb, uint32_t pack, uint32_t table,
                          uint32_t tags, int unpack, char *e, unsigned el)
{
    uint32_t tagbase, count = 0;
    if (!span_ok(sb, table, 4u)) goto bad;
    tagbase = emu68k_gread32(sb, table); table += 4u;
    for (unsigned guard = 0; guard < 65536u; guard++, table += 4u) {
        uint32_t entry, tag, data, memoff, bitoff, control, item;
        if (!span_ok(sb, table, 4u)) goto bad;
        entry = emu68k_gread32(sb, table);
        if (!entry) return (int)count;
        if (entry == UINT32_MAX) {
            table += 4u;
            if (!span_ok(sb, table, 4u)) goto bad;
            tagbase = emu68k_gread32(sb, table);
            continue;
        }
        if ((!unpack && (entry & 0x20000000u)) ||
            (unpack && (entry & 0x40000000u))) continue;
        tag = tagbase + ((entry >> 16) & 0x3ffu);
        item = utility_find_tag(sb, tag, tags);
        if (!item) continue;
        data = emu68k_gread32(sb, item + 4u);
        memoff = entry & 0x1fffu;
        bitoff = (entry >> 13) & 7u;
        control = entry & 0x98000000u;
        if (!unpack) {
            uint8_t b;
            if (!span_ok(sb, pack + memoff,
                         control == 0x10000000u || control == 0x90000000u ? 4u :
                         control == 0x08000000u || control == 0x88000000u ? 2u : 1u))
                goto bad;
            if ((entry & 0x1c000000u) == 0x1c000000u) {
                b = emu68k_gread8(sb, pack + memoff);
                b = entry & 0x80000000u ? b & ~(1u << bitoff) : b | (1u << bitoff);
                emu68k_gwrite8(sb, pack + memoff, b); count++; continue;
            }
            switch (control) {
            case 0x10000000u: case 0x90000000u:
                emu68k_gwrite32(sb, pack + memoff, data); break;
            case 0x08000000u: case 0x88000000u:
                emu68k_gwrite16(sb, pack + memoff, data); break;
            case 0x00000000u: case 0x80000000u:
                emu68k_gwrite8(sb, pack + memoff, (uint8_t)data); break;
            case 0x18000000u: case 0x98000000u:
                b = emu68k_gread8(sb, pack + memoff);
                if (!!data == (control == 0x18000000u)) b |= 1u << bitoff;
                else b &= ~(1u << bitoff);
                emu68k_gwrite8(sb, pack + memoff, b); break;
            default: continue;
            }
        } else {
            uint32_t out, target = data;
            if (!span_ok(sb, target, 4u)) goto bad;
            switch (control) {
            case 0x10000000u: out = emu68k_gread32(sb, pack + memoff); break;
            case 0x90000000u: out = (uint32_t)(int32_t)emu68k_gread32(sb, pack + memoff); break;
            case 0x08000000u: out = emu68k_gread16(sb, pack + memoff); break;
            case 0x88000000u: out = (uint32_t)(int32_t)(int16_t)emu68k_gread16(sb, pack + memoff); break;
            case 0x00000000u: out = emu68k_gread8(sb, pack + memoff); break;
            case 0x80000000u: out = (uint32_t)(int32_t)(int8_t)emu68k_gread8(sb, pack + memoff); break;
            case 0x18000000u: out = !!(emu68k_gread8(sb, pack + memoff) & (1u << bitoff)); break;
            case 0x98000000u: out = !(emu68k_gread8(sb, pack + memoff) & (1u << bitoff)); break;
            default: continue;
            }
            emu68k_gwrite32(sb, target, out);
        }
        count++;
    }
bad:
    if (e && el) snprintf(e, el, "utility pack table leaves guest memory");
    return -1;
}

struct fmt_output {
    j4_sandbox *sb;
    uint32_t buffer;
    uint32_t room;
    uint32_t count;
};

static void fmt_put(struct fmt_output *out, uint8_t ch)
{
    if (out->buffer && out->room && out->count < out->room - 1u)
        emu68k_gwrite8(out->sb, out->buffer + out->count, ch);
    out->count++;
}

static void fmt_repeat(struct fmt_output *out, uint8_t ch, uint32_t count)
{
    while (count--) fmt_put(out, ch);
}

static void fmt_field(struct fmt_output *out, const char *text, uint32_t len,
                      uint32_t width, int left, int zero, int negative)
{
    uint32_t total = len + (negative ? 1u : 0u);
    uint32_t pad = width > total ? width - total : 0u;
    if (!left && !zero) fmt_repeat(out, ' ', pad);
    if (negative) fmt_put(out, '-');
    if (!left && zero) fmt_repeat(out, '0', pad);
    for (uint32_t i = 0; i < len; i++) fmt_put(out, (uint8_t)text[i]);
    if (left) fmt_repeat(out, ' ', pad);
}

int emu68k_guest_format(j4_sandbox *sb, uint32_t buffer, uint32_t room,
                        uint32_t format, uint32_t args, uint32_t *required,
                        char *e, unsigned el)
{
    struct fmt_output out = {sb, buffer, room, 0};
    uint32_t fp = format, ap = args;
    int terminated = 0;
    if (!format || !span_ok(sb, format, 1u) ||
        (buffer && room && !span_ok(sb, buffer, room))) goto bad;
    for (unsigned guard = 0; guard < 65536u; guard++) {
        uint8_t ch;
        if (!span_ok(sb, fp, 1u)) goto bad;
        ch = emu68k_gread8(sb, fp++);
        if (!ch) { terminated = 1; break; }
        if (ch != '%') { fmt_put(&out, ch); continue; }
        {
            int left = 0, zero = 0, is_long = 0;
            uint32_t width = 0, limit = UINT32_MAX;
            char digits[16];
            uint32_t len = 0, value, magnitude;
            for (;;) {
                if (!span_ok(sb, fp, 1u)) goto bad;
                ch = emu68k_gread8(sb, fp);
                if (ch == '-') { left = 1; fp++; continue; }
                if (ch == '0') { zero = 1; fp++; continue; }
                break;
            }
            while (span_ok(sb, fp, 1u) &&
                   (ch = emu68k_gread8(sb, fp)) >= '0' && ch <= '9') {
                if (width < 100000u) width = width * 10u + ch - '0';
                fp++;
            }
            if (!span_ok(sb, fp, 1u)) goto bad;
            if (emu68k_gread8(sb, fp) == '.') {
                limit = 0; fp++;
                while (span_ok(sb, fp, 1u) &&
                       (ch = emu68k_gread8(sb, fp)) >= '0' && ch <= '9') {
                    if (limit < 100000u) limit = limit * 10u + ch - '0';
                    fp++;
                }
            }
            if (!span_ok(sb, fp, 1u)) goto bad;
            if (emu68k_gread8(sb, fp) == 'l') { is_long = 1; fp++; }
            if (!span_ok(sb, fp, 1u)) goto bad;
            ch = emu68k_gread8(sb, fp++);
            if (!ch) break;
            if (ch == 's' || ch == 'b') {
                uint32_t p;
                if (!span_ok(sb, ap, 4u)) goto bad;
                p = emu68k_gread32(sb, ap); ap += 4u;
                if (ch == 'b') {
                    p <<= 2;
                    if (!span_ok(sb, p, 1u)) goto bad;
                    len = emu68k_gread8(sb, p++);
                    if (!span_ok(sb, p, len)) goto bad;
                } else {
                    if (!p || !span_ok(sb, p, 1u)) goto bad;
                    while (len < 65536u && span_ok(sb, p + len, 1u) &&
                           emu68k_gread8(sb, p + len)) len++;
                    if (len == 65536u || !span_ok(sb, p + len, 1u)) goto bad;
                }
                if (limit != UINT32_MAX && len > limit) len = limit;
                if (!left && width > len) fmt_repeat(&out, zero ? '0' : ' ', width - len);
                for (uint32_t i = 0; i < len; i++) fmt_put(&out, emu68k_gread8(sb, p + i));
                if (left && width > len) fmt_repeat(&out, ' ', width - len);
                continue;
            }
            if (ch == 'c' || ch == 'd' || ch == 'u' || ch == 'x' || ch == 'X') {
                unsigned base = (ch == 'x' || ch == 'X') ? 16u : 10u;
                int negative = 0;
                uint32_t bytes = is_long ? 4u : 2u;
                if (!span_ok(sb, ap, bytes)) goto bad;
                value = is_long ? emu68k_gread32(sb, ap) : emu68k_gread16(sb, ap);
                ap += bytes;
                if (ch == 'c') { fmt_put(&out, (uint8_t)value); continue; }
                if (ch == 'd') {
                    int32_t signed_value = is_long ? (int32_t)value : (int16_t)value;
                    negative = signed_value < 0;
                    magnitude = negative ? (uint32_t)(-(int64_t)signed_value)
                                         : (uint32_t)signed_value;
                } else magnitude = value;
                do {
                    uint32_t digit = magnitude % base;
                    digits[len++] = (char)(digit < 10u ? '0' + digit : 'a' + digit - 10u);
                    magnitude /= base;
                } while (magnitude && len < sizeof digits);
                for (uint32_t i = 0; i < len / 2u; i++) {
                    char swap = digits[i]; digits[i] = digits[len - i - 1u];
                    digits[len - i - 1u] = swap;
                }
                fmt_field(&out, digits, len, width, left, zero, negative);
                continue;
            }
            /* RawDoFmt emits %% and unknown directives literally. */
            fmt_put(&out, ch);
        }
    }
    if (!terminated) goto bad;
    if (buffer && room)
        emu68k_gwrite8(sb, buffer + (out.count < room ? out.count : room - 1u), 0);
    *required = out.count + 1u; /* VSNPrintf includes its trailing NUL. */
    return 0;
bad:
    if (e && el) snprintf(e, el, "utility.VSNPrintf leaves guest memory");
    return 1;
}

int emu68k_utility_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                        struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case UTILITY_LVO_ALLOCNAMEDOBJECTA: {
        const char *name = emu68k_guest_cstr(sb, st->a[0]);
        uint32_t cursor = st->a[1], item, userspace = 0;
        uint32_t has_namespace = 0, flags = 0, priority = 0;
        uint32_t object, data, guest_name;
        int slot = -1;
        if (!name || strlen(name) >= sizeof r->named[0].name) {
            st->d[0] = 0; return 0;
        }
        while ((item = next_item(sb, &cursor)) != 0) {
            uint32_t tag = emu68k_gread32(sb, item);
            data = emu68k_gread32(sb, item + 4u);
            if (tag == ANO_NAMESPACE) has_namespace = !!data;
            else if (tag == ANO_USERSPACE) userspace = data;
            else if (tag == ANO_PRIORITY) priority = data;
            else if (tag == ANO_FLAGS) flags = data;
        }
        if (userspace > 16u * 1024u * 1024u) { st->d[0] = 0; return 0; }
        for (int i = 0; i < 128; i++) if (!r->named[i].live) { slot = i; break; }
        object = slot >= 0 ? emu68k_guest_alloc(r, 4u) : 0;
        data = userspace ? emu68k_guest_alloc(r, userspace) : 0;
        guest_name = object ? emu68k_guest_strdup(r, name, strlen(name)) : 0;
        if (!object || (userspace && !data) || !guest_name) {
            st->d[0] = 0; return 0;
        }
        memset(j4_sandbox_host(sb, object), 0, 4u);
        if (data) memset(j4_sandbox_host(sb, data), 0, userspace);
        emu68k_gwrite32(sb, object, data); /* public NamedObject.no_Object */
        memset(&r->named[slot], 0, sizeof r->named[slot]);
        r->named[slot].object = object;
        r->named[slot].userspace = data;
        r->named[slot].user_size = userspace;
        r->named[slot].guest_name = guest_name;
        r->named[slot].refs = 1;
        r->named[slot].priority = (int8_t)priority;
        r->named[slot].flags = (uint8_t)flags;
        r->named[slot].has_namespace = (uint8_t)has_namespace;
        r->named[slot].live = 1;
        memcpy(r->named[slot].name, name, strlen(name) + 1u);
        st->d[0] = object;
        return 0;
    }
    case UTILITY_LVO_ADDNAMEDOBJECT: {
        int object = named_index(r, st->a[1]);
        int parent = st->a[0] ? named_index(r, st->a[0]) : -1;
        uint8_t flags;
        if (object < 0 || (st->a[0] &&
            (parent < 0 || !r->named[parent].has_namespace)) ||
            r->named[object].added) { st->d[0] = 0; return 0; }
        flags = named_namespace_flags(r, st->a[0]);
        if (flags & NSF_NODUPS)
            for (int i = 0; i < 128; i++)
                if (r->named[i].live && r->named[i].added &&
                    r->named[i].parent == st->a[0] &&
                    named_equal(r->named[i].name, r->named[object].name, flags))
                { st->d[0] = 0; return 0; }
        r->named[object].parent = st->a[0];
        r->named[object].added = 1;
        st->d[0] = UINT32_MAX;
        return 0;
    }
    case UTILITY_LVO_FINDNAMEDOBJECT: {
        const char *name = st->a[1] ? emu68k_guest_cstr(sb, st->a[1]) : NULL;
        int parent = st->a[0] ? named_index(r, st->a[0]) : -1;
        int last = st->a[2] ? named_index(r, st->a[2]) : -1;
        uint8_t flags;
        if ((st->a[0] && (parent < 0 || !r->named[parent].has_namespace)) ||
            (st->a[1] && !name) || (st->a[2] && last < 0))
            goto named_bad;
        flags = named_namespace_flags(r, st->a[0]);
        for (int i = last + 1; i < 128; i++)
            if (r->named[i].live && r->named[i].added &&
                r->named[i].parent == st->a[0] &&
                (!name || named_equal(r->named[i].name, name, flags))) {
                r->named[i].refs++;
                st->d[0] = r->named[i].object;
                return 0;
            }
        st->d[0] = 0;
        return 0;
    }
    case UTILITY_LVO_NAMEDOBJECTNAME: {
        int i = named_index(r, st->a[0]);
        if (i < 0 && st->a[0]) goto named_bad;
        st->d[0] = i < 0 ? 0 : r->named[i].guest_name;
        return 0;
    }
    case UTILITY_LVO_RELEASENAMEDOBJECT: {
        int i = named_index(r, st->a[0]);
        if (i < 0 && st->a[0]) goto named_bad;
        if (i >= 0 && r->named[i].refs > 0) r->named[i].refs--;
        named_remove_if_ready(r, i);
        return 0;
    }
    case UTILITY_LVO_ATTEMPTREMNAMEDOBJECT: {
        int i = named_index(r, st->a[0]);
        if (i < 0) goto named_bad;
        if (r->named[i].refs > 1) { st->d[0] = 0; return 0; }
        r->named[i].pending_remove = 1;
        if (r->named[i].refs > 0) r->named[i].refs--;
        named_remove_if_ready(r, i);
        st->d[0] = UINT32_MAX;
        return 0;
    }
    case UTILITY_LVO_REMNAMEDOBJECT: {
        int i = named_index(r, st->a[0]);
        if (i < 0) goto named_bad;
        r->named[i].pending_remove = 1;
        if (r->named[i].refs > 0) r->named[i].refs--;
        named_remove_if_ready(r, i);
        if (st->a[1]) {
            struct j5d_m68k_state reply = *st;
            if (!span_ok(sb, st->a[1], M68K_Message_SIZEOF)) goto named_bad;
            emu68k_gwrite32(sb, st->a[1] + M68K_Message_mn_Node_ln_Name,
                            st->a[0]);
            reply.a[1] = st->a[1];
            if (emu68k_exec_call(r, sb, LVO_REPLYMSG, &reply, e, el) != 0)
                return 1;
        }
        return 0;
    }
    case UTILITY_LVO_FREENAMEDOBJECT: {
        int i = named_index(r, st->a[0]);
        if (i < 0 && st->a[0]) goto named_bad;
        if (i >= 0 && !r->named[i].added)
            memset(&r->named[i], 0, sizeof r->named[i]);
        return 0;
    }
    case UTILITY_LVO_FINDTAGITEM:
        st->d[0] = utility_find_tag(sb, st->d[0], st->a[0]); return 0;
    case UTILITY_LVO_GETTAGDATA: {
        uint32_t item = utility_find_tag(sb, st->d[0], st->a[0]);
        st->d[0] = item ? emu68k_gread32(sb, item + 4u) : st->d[1]; return 0;
    }
    case UTILITY_LVO_PACKBOOLTAGS: {
        uint32_t flags = st->d[0], cursor = st->a[0], item;
        while ((item = next_item(sb, &cursor)) != 0) {
            uint32_t map = utility_find_tag(sb, emu68k_gread32(sb, item), st->a[1]);
            if (map) {
                uint32_t mask = emu68k_gread32(sb, map + 4u);
                flags = emu68k_gread32(sb, item + 4u) ? flags | mask : flags & ~mask;
            }
        }
        st->d[0] = flags; return 0;
    }
    case UTILITY_LVO_NEXTTAGITEM:
        st->d[0] = utility_next_tag(sb, st->a[0]); return 0;
    case UTILITY_LVO_FILTERTAGCHANGES: {
        uint32_t cursor = st->a[0], item;
        while ((item = next_item(sb, &cursor)) != 0) {
            uint32_t orig = utility_find_tag(sb, emu68k_gread32(sb, item), st->a[1]);
            if (!orig) continue;
            if (emu68k_gread32(sb, item + 4u) == emu68k_gread32(sb, orig + 4u))
                emu68k_gwrite32(sb, item, TAG_IGNORE);
            else if (st->d[0])
                emu68k_gwrite32(sb, orig + 4u, emu68k_gread32(sb, item + 4u));
        }
        return 0;
    }
    case UTILITY_LVO_MAPTAGS: {
        uint32_t cursor = st->a[0], item;
        while ((item = next_item(sb, &cursor)) != 0) {
            uint32_t map = utility_find_tag(sb, emu68k_gread32(sb, item), st->a[1]);
            if (map) {
                uint32_t replacement = emu68k_gread32(sb, map + 4u);
                emu68k_gwrite32(sb, item, replacement ? replacement : TAG_IGNORE);
            } else if (st->d[0] == 0) emu68k_gwrite32(sb, item, TAG_IGNORE);
        }
        return 0;
    }
    case UTILITY_LVO_ALLOCATETAGITEMS:
        st->d[0] = st->d[0] && st->d[0] <= UINT32_MAX / 8u
                 ? emu68k_guest_alloc(r, st->d[0] * 8u) : 0; return 0;
    case UTILITY_LVO_CLONETAGITEMS: {
        uint32_t cursor = st->a[0], item, count = 1, out, dst;
        while ((item = next_item(sb, &cursor)) != 0) count++;
        out = emu68k_guest_alloc(r, count * 8u); dst = out; cursor = st->a[0];
        while (out && (item = next_item(sb, &cursor)) != 0) {
            emu68k_gwrite32(sb, dst, emu68k_gread32(sb, item));
            emu68k_gwrite32(sb, dst + 4u, emu68k_gread32(sb, item + 4u)); dst += 8u;
        }
        st->d[0] = out; return 0;
    }
    case UTILITY_LVO_FREETAGITEMS:
        return 0;
    case UTILITY_LVO_REFRESHTAGITEMCLONES: {
        uint32_t dst = st->a[0], cursor = st->a[1], item;
        if (!dst) return 0;
        while ((item = next_item(sb, &cursor)) != 0) {
            if (!span_ok(sb, dst, 8u)) break;
            emu68k_gwrite32(sb, dst, emu68k_gread32(sb, item));
            emu68k_gwrite32(sb, dst + 4u, emu68k_gread32(sb, item + 4u)); dst += 8u;
        }
        if (span_ok(sb, dst, 8u)) { emu68k_gwrite32(sb, dst, 0); emu68k_gwrite32(sb, dst + 4u, 0); }
        return 0;
    }
    case UTILITY_LVO_TAGINARRAY:
        st->d[0] = tag_in_array(sb, st->d[0], st->a[0]); return 0;
    case UTILITY_LVO_FILTERTAGITEMS: {
        uint32_t cursor = st->a[0], item, valid = 0;
        while ((item = next_item(sb, &cursor)) != 0) {
            int found = tag_in_array(sb, emu68k_gread32(sb, item), st->a[1]);
            if ((st->d[0] == 0 && found) || (st->d[0] == 1 && !found)) valid++;
            else emu68k_gwrite32(sb, item, TAG_IGNORE);
        }
        st->d[0] = valid; return 0;
    }
    case UTILITY_LVO_AMIGA2DATE:
        amiga_to_date(sb, st->d[0], st->a[0]); return 0;
    case UTILITY_LVO_DATE2AMIGA:
        st->d[0] = date_to_amiga(sb, st->a[0], 0); return 0;
    case UTILITY_LVO_CHECKDATE:
        st->d[0] = date_to_amiga(sb, st->a[0], 1); return 0;
    case UTILITY_LVO_SDIVMOD32: {
        int32_t a = (int32_t)st->d[0], b = (int32_t)st->d[1];
        if (!b) { if (e && el) snprintf(e, el, "utility.SDivMod32 divides by zero"); return 1; }
        if (a == INT32_MIN && b == -1) { st->d[0] = (uint32_t)INT32_MIN; st->d[1] = 0; }
        else { st->d[0] = (uint32_t)(a / b); st->d[1] = (uint32_t)(a % b); }
        return 0;
    }
    case UTILITY_LVO_UDIVMOD32:
        if (!st->d[1]) { if (e && el) snprintf(e, el, "utility.UDivMod32 divides by zero"); return 1; }
        { uint32_t a = st->d[0], b = st->d[1]; st->d[0] = a / b; st->d[1] = a % b; }
        return 0;
    case UTILITY_LVO_APPLYTAGCHANGES: {
        uint32_t cursor = st->a[0], item;
        while ((item = next_item(sb, &cursor)) != 0) {
            uint32_t change = utility_find_tag(sb, emu68k_gread32(sb, item), st->a[1]);
            if (change) emu68k_gwrite32(sb, item + 4u, emu68k_gread32(sb, change + 4u));
        }
        return 0;
    }
    case UTILITY_LVO_SMULT64: {
        int64_t product = (int64_t)(int32_t)st->d[0] * (int32_t)st->d[1];
        st->d[0] = (uint32_t)((uint64_t)product >> 32); st->d[1] = (uint32_t)product; return 0;
    }
    case UTILITY_LVO_UMULT64: {
        uint64_t product = (uint64_t)st->d[0] * st->d[1];
        st->d[0] = (uint32_t)(product >> 32); st->d[1] = (uint32_t)product; return 0;
    }
    case UTILITY_LVO_PACKSTRUCTURETAGS: {
        int count = pack_structure(sb, st->a[0], st->a[1], st->a[2], 0, e, el);
        if (count < 0) return 1;
        st->d[0] = (uint32_t)count;
        return 0;
    }
    case UTILITY_LVO_UNPACKSTRUCTURETAGS: {
        int count = pack_structure(sb, st->a[0], st->a[1], st->a[2], 1, e, el);
        if (count < 0) return 1;
        st->d[0] = (uint32_t)count;
        return 0;
    }
    case UTILITY_LVO_SETMEM:
        if (!span_ok(sb, st->a[0], st->d[1])) {
            if (e && el) snprintf(e, el, "utility.SetMem leaves guest memory"); return 1;
        }
        memset(j4_sandbox_host(sb, st->a[0]), (uint8_t)st->d[0], st->d[1]);
        st->d[0] = st->a[0]; return 0;
    case UTILITY_LVO_VSNPRINTF: {
        uint32_t required;
        if (emu68k_guest_format(sb, st->a[0], st->d[0], st->a[1], st->a[2],
                               &required, e, el) != 0) return 1;
        st->d[0] = required;
        return 0;
    }
    default:
        return 1;
    }

named_bad:
    if (e && el) snprintf(e, el, "utility NamedObject is stale or outside the guest namespace");
    return 1;
}
