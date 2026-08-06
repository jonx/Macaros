/* Handwritten dos.library semantics whose values must remain guest-addressable. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"
#include "bridge_lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITEM_EQUAL_U    0xfffffffeu
#define ITEM_ERROR_U    0xffffffffu
#define ITEM_NOTHING_U  0u
#define ITEM_UNQUOTED_U 1u
#define ITEM_QUOTED_U   2u
#define DOS_ERROR_ACTION_NOT_KNOWN 209u
#define DOS_ERROR_NOT_IMPLEMENTED  236u
#define DOS_ERROR_BUFFER_OVERFLOW  303u
#define DOS_ERROR_BAD_NUMBER       115u

#define NP_ENTRY      0x800003ebu
#define NP_STACKSIZE  0x800003f3u
#define NP_NAME       0x800003f4u
#define NP_PRIORITY   0x800003f5u
#define NP_INPUT      0x800003ecu
#define NP_OUTPUT     0x800003edu
#define NP_ERROR      0x800003f0u
#define NP_CLI        0x800003fau
#define NP_ARGUMENTS  0x800003fdu
#define GSLI_68KHUNK  0x80000fa5u

static int dos_span(j4_sandbox *sb, uint32_t p, uint32_t n)
{
    return p >= sb->sandbox_origin &&
           (uint64_t)p + n <= (uint64_t)sb->sandbox_origin + sb->size;
}

static uint32_t dos_current_process(struct emu68k_run *r)
{
    return emu68k_ctx_task(r);
}

static struct guestseg_live *dos_guest_segment(struct emu68k_run *r,
                                                uint32_t bptr)
{
    for (int i = 0; i < GUESTSEG_MAX; i++)
        if (r->guestseg[i].live && r->guestseg[i].bptr == bptr)
            return &r->guestseg[i];
    return NULL;
}

static int dos_exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                         struct j5d_m68k_state *st, char *e, unsigned el)
{
    return emu68k_exec_call(r, sb, lvo, st, e, el);
}

static int dos_native_call(struct emu68k_run *r, int lvo,
                           struct j5d_m68k_state *st, char *e, unsigned el)
{
    if (!emu68k_oscall) return 1;
    return emu68k_oscall("dos.library", lvo, st, r->reserve,
                         emu68k_oscall_user, e, el);
}

static uint32_t dos_exall_fib(struct emu68k_run *r, uint32_t control)
{
    int free_slot = -1;
    for (int i = 0; i < 8; i++) {
        if (r->exall[i].control == control) return r->exall[i].fib;
        if (!r->exall[i].control && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;
    r->exall[free_slot].fib = emu68k_guest_alloc(r, M68K_FileInfoBlock_SIZEOF);
    if (!r->exall[free_slot].fib) return 0;
    r->exall[free_slot].control = control;
    return r->exall[free_slot].fib;
}

static void dos_exall_end(struct emu68k_run *r, uint32_t control)
{
    for (int i = 0; i < 8; i++)
        if (r->exall[i].control == control) {
            r->exall[i].control = 0;
            r->exall[i].fib = 0;
            return;
        }
}

static int dos_match_pattern(struct emu68k_run *r, uint32_t pattern,
                             uint32_t string, struct j5d_m68k_state *basis,
                             char *e, unsigned el)
{
    struct j5d_m68k_state call = *basis;
    if (!pattern) return 1;
    call.d[1] = pattern;
    call.d[2] = string;
    if (dos_native_call(r, DOS_LVO_MATCHPATTERNNOCASE, &call, e, el) != 0)
        return -1;
    return call.d[0] != 0;
}

static int dos_exall(struct emu68k_run *r, j4_sandbox *sb,
                     struct j5d_m68k_state *st, char *e, unsigned el)
{
    static const uint8_t fixed_size[8] = { 0, 8, 12, 16, 20, 32, 36, 40 };
    uint32_t lock = st->d[1], buffer = st->d[2], size = st->d[3];
    uint32_t level = st->d[4], control = st->d[5], fib;
    uint32_t cursor = buffer, end, last = 0, entries = 0;
    uint32_t lastkey, pattern, hook;
    struct j5d_m68k_state call = *st;
    int reached_end = 0;

    if (level > 7u) {
        r->last_ioerr = DOS_ERROR_BAD_NUMBER; st->d[0] = 0; return 0;
    }
    if (!dos_span(sb, control, M68K_ExAllControl_SIZEOF) ||
        !dos_span(sb, buffer, size)) goto invalid;
    end = buffer + size;
    fib = dos_exall_fib(r, control);
    if (!fib) { r->last_ioerr = 103; st->d[0] = 0; return 0; }
    lastkey = emu68k_gread32(sb, control + M68K_ExAllControl_eac_LastKey);
    pattern = emu68k_gread32(sb, control + M68K_ExAllControl_eac_MatchString);
    hook = emu68k_gread32(sb, control + M68K_ExAllControl_eac_MatchFunc);
    emu68k_gwrite32(sb, control + M68K_ExAllControl_eac_Entries, 0);
    memset(j4_sandbox_host(sb, fib), 0, M68K_FileInfoBlock_SIZEOF);
    if (!lastkey) {
        call.d[1] = lock; call.d[2] = fib;
        if (dos_native_call(r, DOS_LVO_EXAMINE, &call, e, el) != 0) return 1;
        if (!call.d[0]) { st->d[0] = 0; return 0; }
        if ((int32_t)emu68k_gread32(sb, fib +
                M68K_FileInfoBlock_fib_DirEntryType) <= 0) {
            r->last_ioerr = 212; st->d[0] = 0; return 0;
        }
    } else {
        emu68k_gwrite32(sb, fib + M68K_FileInfoBlock_fib_DiskKey, lastkey);
    }

    for (;;) {
        uint32_t name = fib + M68K_FileInfoBlock_fib_FileName;
        uint32_t comment = fib + M68K_FileInfoBlock_fib_Comment;
        uint32_t name_len, comment_len, next;
        int match;
        call = *st; call.d[1] = lock; call.d[2] = fib;
        if (dos_native_call(r, DOS_LVO_EXNEXT, &call, e, el) != 0) return 1;
        if (!call.d[0]) { reached_end = 1; break; }
        lastkey = emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_DiskKey);
        emu68k_gwrite32(sb, control + M68K_ExAllControl_eac_LastKey, lastkey);
        match = dos_match_pattern(r, pattern, name, st, e, el);
        if (match < 0) return 1;
        if (!match) continue;
        name_len = (uint32_t)strnlen((const char *)j4_sandbox_host(sb, name), 108) + 1u;
        comment_len = level >= 6u ?
            (uint32_t)strnlen((const char *)j4_sandbox_host(sb, comment), 80) + 1u : 0u;
        next = cursor + fixed_size[level] + name_len + comment_len;
        next = (next + 3u) & ~3u;
        if (next > end || cursor + fixed_size[level] > end) {
            if (!entries) {
                r->last_ioerr = DOS_ERROR_BUFFER_OVERFLOW;
                st->d[0] = 0;
                return 0;
            }
            break;
        }
        memset(j4_sandbox_host(sb, cursor), 0, next - cursor);
        {
            uint32_t strings = cursor + fixed_size[level];
            if (level >= 1u) {
                emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Name, strings);
                memcpy(j4_sandbox_host(sb, strings), j4_sandbox_host(sb, name), name_len);
                strings += name_len;
            }
            if (level >= 2u) emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Type,
                emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_DirEntryType));
            if (level >= 3u) emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Size,
                emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_Size));
            if (level >= 4u) emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Prot,
                emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_Protection));
            if (level >= 5u) {
                emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Days,
                    emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_Date_ds_Days));
                emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Mins,
                    emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_Date_ds_Minute));
                emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Ticks,
                    emu68k_gread32(sb, fib + M68K_FileInfoBlock_fib_Date_ds_Tick));
            }
            if (level >= 6u) {
                emu68k_gwrite32(sb, cursor + M68K_ExAllData_ed_Comment, strings);
                memcpy(j4_sandbox_host(sb, strings), j4_sandbox_host(sb, comment),
                       comment_len);
            }
            if (level >= 7u) {
                emu68k_gwrite16(sb, cursor + M68K_ExAllData_ed_OwnerUID,
                    emu68k_gread16(sb, fib + M68K_FileInfoBlock_fib_OwnerUID));
                emu68k_gwrite16(sb, cursor + M68K_ExAllData_ed_OwnerGID,
                    emu68k_gread16(sb, fib + M68K_FileInfoBlock_fib_OwnerGID));
            }
        }
        if (hook) {
            struct j5d_m68k_state hookcall = *st;
            uint32_t entry, levelp, accepted = 0;
            if (!dos_span(sb, hook, M68K_Hook_SIZEOF)) goto invalid;
            entry = emu68k_gread32(sb, hook + M68K_Hook_h_Entry);
            levelp = emu68k_guest_alloc(r, 4);
            if (!levelp) return 1;
            emu68k_gwrite32(sb, levelp, level);
            memset(&hookcall, 0, sizeof hookcall);
            hookcall.a[0] = hook; hookcall.a[1] = cursor; hookcall.a[2] = levelp;
            if (emu68k_run_guest_subroutine(r, entry, &hookcall, 0,
                                            &accepted, e, el) != 0)
                return 1;
            if (!accepted) continue;
        }
        if (last) emu68k_gwrite32(sb, last + M68K_ExAllData_ed_Next, cursor);
        last = cursor;
        cursor = next;
        entries++;
    }
    if (last) emu68k_gwrite32(sb, last + M68K_ExAllData_ed_Next, 0);
    else if (size >= 4u) emu68k_gwrite32(sb, buffer, 0);
    emu68k_gwrite32(sb, control + M68K_ExAllControl_eac_Entries, entries);
    r->last_ioerr = reached_end ? 232u : 0u;
    st->d[0] = reached_end ? 0u : 0xffffffffu;
    return 0;
invalid:
    if (e && el) snprintf(e, el, "ExAll guest buffer/control is invalid");
    return 1;
}

static uint32_t dos_current_cli(struct emu68k_run *r, j4_sandbox *sb)
{
    uint32_t process = dos_current_process(r);
    uint32_t bptr;
    if (!dos_span(sb, process + CLASSIC_PR_CLI, 4)) return 0;
    bptr = emu68k_gread32(sb, process + CLASSIC_PR_CLI);
    return bptr << 2;
}

static int dos_set_cli_bstr(struct emu68k_run *r, j4_sandbox *sb,
                            uint32_t field, uint32_t text)
{
    const char *s = emu68k_guest_cstr(sb, text);
    uint32_t cli = dos_current_cli(r, sb), bstr, n;
    if (!cli || !s) return 0;
    n = (uint32_t)strlen(s);
    if (n > 255u) return 0;
    bstr = emu68k_gread32(sb, cli + field) << 2;
    if (!bstr) {
        bstr = emu68k_guest_alloc(r, 256);
        if (!bstr) return 0;
        emu68k_gwrite32(sb, cli + field, bstr >> 2);
    }
    if (!dos_span(sb, bstr, 256)) return 0;
    emu68k_gwrite8(sb, bstr, (uint8_t)n);
    if (n) memcpy(j4_sandbox_host(sb, bstr + 1), s, n);
    return 1;
}

static int dos_csource_get(j4_sandbox *sb, uint32_t source, int *out)
{
    uint32_t buffer, length, cursor;
    if (!dos_span(sb, source, M68K_CSource_SIZEOF)) return -1;
    buffer = emu68k_gread32(sb, source + M68K_CSource_CS_Buffer);
    length = emu68k_gread32(sb, source + M68K_CSource_CS_Length);
    cursor = emu68k_gread32(sb, source + M68K_CSource_CS_CurChr);
    if (cursor >= length) { *out = -1; return 0; }
    if (!dos_span(sb, buffer, length)) return -1;
    *out = emu68k_gread8(sb, buffer + cursor);
    emu68k_gwrite32(sb, source + M68K_CSource_CS_CurChr, cursor + 1u);
    return 0;
}

static void dos_csource_unget(j4_sandbox *sb, uint32_t source)
{
    uint32_t cursor = emu68k_gread32(sb, source + M68K_CSource_CS_CurChr);
    if (cursor) emu68k_gwrite32(sb, source + M68K_CSource_CS_CurChr,
                                cursor - 1u);
}

/* The deliberately quirky AmigaOS ReadItem behavior, operating directly on
 * the guest CSource so its retained buffer and cursor never cross natively. */
static int dos_read_item(j4_sandbox *sb, uint32_t buffer, int32_t maxchars,
                         uint32_t source, uint32_t *result,
                         char *e, unsigned el)
{
    uint32_t out = buffer;
    int c;
#define GETC() do { if (dos_csource_get(sb, source, &c) < 0) goto bad; } while (0)
#define UNGETC() dos_csource_unget(sb, source)
    if (!buffer) { *result = ITEM_NOTHING_U; return 0; }
    if (!dos_span(sb, buffer, 1)) goto bad;
    if (maxchars <= 0) {
        emu68k_gwrite8(sb, buffer, 0); *result = ITEM_NOTHING_U; return 0;
    }
    if (!source) {
        if (e && el) snprintf(e, el,
            "ReadItem without CSource needs guest Input() stream state");
        return 1;
    }
    do { GETC(); } while (c == ' ' || c == '\t');
    if (c <= 0 || c == '\n' || c == ';') {
        emu68k_gwrite8(sb, out, 0);
        if (c != -1) UNGETC();
        *result = ITEM_NOTHING_U; return 0;
    }
    if (c == '=') {
        emu68k_gwrite8(sb, out, 0); *result = ITEM_EQUAL_U; return 0;
    }
    if (c == '"') {
        for (;;) {
            if (--maxchars <= 0) {
                emu68k_gwrite8(sb, out ? out - 1u : out, 0);
                *result = ITEM_NOTHING_U; return 0;
            }
            GETC();
            if (c == '*') {
                GETC();
                if (c <= 0 || c == '\n') {
                    UNGETC(); emu68k_gwrite8(sb, out, 0);
                    *result = ITEM_ERROR_U; return 0;
                }
                if (c == 'n' || c == 'N') c = '\n';
                else if (c == 'e' || c == 'E') c = 0x1b;
            } else if (c <= 0 || c == '\n') {
                UNGETC(); emu68k_gwrite8(sb, out, 0);
                *result = ITEM_ERROR_U; return 0;
            } else if (c == '"') {
                emu68k_gwrite8(sb, out, 0);
                *result = ITEM_QUOTED_U; return 0;
            }
            if (!dos_span(sb, out, 1)) goto bad;
            emu68k_gwrite8(sb, out++, (uint8_t)c);
        }
    }
    if (--maxchars <= 0) {
        emu68k_gwrite8(sb, out, 0); *result = ITEM_ERROR_U; return 0;
    }
    emu68k_gwrite8(sb, out++, (uint8_t)c);
    for (;;) {
        if (--maxchars <= 0) {
            emu68k_gwrite8(sb, out - 1u, 0);
            *result = ITEM_ERROR_U; return 0;
        }
        GETC();
        if (c <= 0 || c == ' ' || c == '\t' || c == '\n' || c == '=') {
            if (c != '=' && c != ' ' && c != '\t') UNGETC();
            emu68k_gwrite8(sb, out, 0);
            *result = ITEM_UNQUOTED_U; return 0;
        }
        if (!dos_span(sb, out, 1)) goto bad;
        emu68k_gwrite8(sb, out++, (uint8_t)c);
    }
bad:
    if (e && el) snprintf(e, el, "ReadItem guest buffer or CSource is invalid");
    return 1;
#undef GETC
#undef UNGETC
}

static int dos_vfprintf(struct emu68k_run *r, j4_sandbox *sb,
                        struct j5d_m68k_state *st, char *e, unsigned el)
{
    uint32_t required, out;
    struct j5d_m68k_state write_call;
    if (emu68k_guest_format(sb, 0, 0, st->d[2], st->d[3],
                           &required, e, el) != 0)
        return 1;
    if (!required || required > 65536u) {
        if (e && el) snprintf(e, el, "dos.VFPrintf output is too large");
        return 1;
    }
    out = emu68k_guest_alloc(r, required);
    if (!out || emu68k_guest_format(sb, out, required, st->d[2], st->d[3],
                                    &required, e, el) != 0)
        return 1;
    write_call = *st;
    write_call.d[1] = st->d[1];
    write_call.d[2] = out;
    write_call.d[3] = required - 1u;
    if (!emu68k_oscall || emu68k_oscall("dos.library", DOS_LVO_WRITE,
            &write_call, r->reserve, emu68k_oscall_user, e, el) != 0)
        return 1;
    st->d[0] = write_call.d[0];
    return 0;
}

static int dos_vfwritef(struct emu68k_run *r, j4_sandbox *sb,
                        struct j5d_m68k_state *st, char *e, unsigned el)
{
    const uint32_t limit = 65536u;
    uint32_t fmt = st->d[2], args = st->d[3], guest_out;
    char *out = malloc(limit);
    uint32_t used = 0;
    struct j5d_m68k_state write_call;
#define PUTCH(ch) do { if (used >= limit) goto too_large; out[used++] = (char)(ch); } while (0)
#define NEXTARG(dst) do { if (!dos_span(sb, args, 4)) goto invalid; \
                          (dst) = emu68k_gread32(sb, args); args += 4; } while (0)
    if (!out) { st->d[0] = 0xffffffffu; return 0; }
    for (uint32_t guard = 0; guard < limit; guard++, fmt++) {
        uint32_t value;
        int c;
        if (!dos_span(sb, fmt, 1)) goto invalid;
        c = emu68k_gread8(sb, fmt);
        if (!c) break;
        if (c != '%') { PUTCH(c); continue; }
        if (!dos_span(sb, ++fmt, 1)) goto invalid;
        c = emu68k_gread8(sb, fmt);
        if (!c) break;
        switch (c) {
        case 'S': case 's': {
            const char *s;
            NEXTARG(value);
            if (!value || !(s = emu68k_guest_cstr(sb, value))) goto format_error;
            while (*s) PUTCH(*s++);
            break;
        }
        case 'T': case 't': {
            uint32_t bstr, n, width;
            if (!dos_span(sb, ++fmt, 1)) goto invalid;
            width = (uint32_t)(emu68k_gread8(sb, fmt) - '0');
            NEXTARG(value); bstr = value << 2;
            if (!value || !dos_span(sb, bstr, 1)) goto format_error;
            n = emu68k_gread8(sb, bstr);
            if (!dos_span(sb, bstr + 1u, n)) goto invalid;
            for (uint32_t i = 0; i < n; i++) PUTCH(emu68k_gread8(sb, bstr + 1u + i));
            while (n++ < width) PUTCH(' ');
            break;
        }
        case 'C': case 'c':
            NEXTARG(value); PUTCH(value); break;
        case 'O': case 'o': case 'X': case 'x': case 'I': case 'i':
        case 'U': case 'u': {
            char number[48];
            uint32_t width, n;
            int base = (c == 'O' || c == 'o') ? 8 :
                       (c == 'X' || c == 'x') ? 16 : 10;
            int uns = c == 'U' || c == 'u';
            if (!dos_span(sb, ++fmt, 1)) goto invalid;
            width = (uint32_t)(emu68k_gread8(sb, fmt) - '0');
            if (width > 9u) width = 0;
            NEXTARG(value);
            if (base == 16) snprintf(number, sizeof number, "%x", value);
            else if (base == 8) snprintf(number, sizeof number, "%o", value);
            else if (uns) snprintf(number, sizeof number, "%u", value);
            else snprintf(number, sizeof number, "%d", (int32_t)value);
            n = (uint32_t)strlen(number);
            if (n > width) n = width;
            for (uint32_t i = 0; i < n; i++) PUTCH(number[i]);
            break;
        }
        case 'N': case 'n': {
            char number[48];
            NEXTARG(value);
            snprintf(number, sizeof number, "%d", (int32_t)value);
            for (const char *s = number; *s; s++) PUTCH(*s);
            break;
        }
        case '$': NEXTARG(value); break;
        default: PUTCH(c); break;
        }
    }
    guest_out = emu68k_guest_alloc(r, used ? used : 1u);
    if (!guest_out) goto format_error;
    if (used) memcpy(j4_sandbox_host(sb, guest_out), out, used);
    free(out);
    write_call = *st;
    write_call.d[1] = st->d[1]; write_call.d[2] = guest_out; write_call.d[3] = used;
    if (dos_native_call(r, DOS_LVO_WRITE, &write_call, e, el) != 0) return 1;
    st->d[0] = write_call.d[0];
    return 0;
too_large:
    if (e && el) snprintf(e, el, "VFWritef output exceeds 65536 bytes");
    free(out); return 1;
invalid:
    if (e && el) snprintf(e, el, "VFWritef format or argument stream is invalid");
    free(out); return 1;
format_error:
    free(out); st->d[0] = 0xffffffffu; return 0;
#undef PUTCH
#undef NEXTARG
}

static int dos_system(struct emu68k_run *r, j4_sandbox *sb,
                      struct j5d_m68k_state *st, char *e, unsigned el)
{
    const char *command = emu68k_guest_cstr(sb, st->d[1]);
    const char *p, *start;
    char quote = 0;
    uint32_t name, args, name_len, args_len, tags = st->d[2], async = 0;
    struct j5d_m68k_state call = *st;
    if (!command) { st->d[0] = 0xffffffffu; return 0; }
    p = command; while (*p == ' ' || *p == '\t') p++;
    if (*p == '"' || *p == '\'') {
        quote = *p++;
        start = p;
        while (*p && *p != quote) p++;
    } else {
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    name_len = (uint32_t)(p - start);
    if (!name_len || name_len >= 256u || strpbrk(command, "<>|")) {
        st->d[0] = 0xffffffffu; return 0;
    }
    if (name_len > 2u && start[1] == ':' &&
        (start[0] == 'C' || start[0] == 'c')) { start += 2; name_len -= 2; }
    if (quote && *p == quote) p++;
    name = emu68k_guest_alloc(r, name_len + 1u);
    if (!name) { st->d[0] = 0xffffffffu; return 0; }
    memcpy(j4_sandbox_host(sb, name), start, name_len);
    emu68k_gwrite8(sb, name + name_len, 0);
    while (*p == ' ' || *p == '\t') p++;
    args_len = (uint32_t)strlen(p);
    args = emu68k_guest_alloc(r, args_len + 2u);
    if (!args) { st->d[0] = 0xffffffffu; return 0; }
    if (args_len) memcpy(j4_sandbox_host(sb, args), p, args_len);
    emu68k_gwrite8(sb, args + args_len, '\n');
    emu68k_gwrite8(sb, args + args_len + 1u, 0);
    for (unsigned guard = 0; tags && guard < 4096u; guard++, tags += 8u) {
        uint32_t tag, data;
        if (!dos_span(sb, tags, 8)) goto invalid_system;
        tag = emu68k_gread32(sb, tags); data = emu68k_gread32(sb, tags + 4);
        if (!tag) break;
        if (tag == 1u) continue;
        if (tag == 2u) { tags = data - 8u; continue; }
        if (tag == 3u) { tags += data * 8u; continue; }
        if (tag == 0x80000023u) async = data != 0; /* SYS_Asynch */
    }
    call.d[1] = name;
    if (emu68k_dos_loadseg(r, sb, &call, e, el) != 0) return 1;
    if (!call.d[0]) { st->d[0] = 0xffffffffu; return 0; }
    if (async) {
        struct guestseg_live *seg = dos_guest_segment(r, call.d[0]);
        uint32_t parent = dos_current_process(r);
        uint32_t ptags = emu68k_guest_alloc(r, 9u * 8u);
        if (!seg || !ptags) { st->d[0] = 0xffffffffu; return 0; }
        emu68k_gwrite32(sb, ptags, NP_ENTRY);
        emu68k_gwrite32(sb, ptags + 4, seg->seg.entry);
        emu68k_gwrite32(sb, ptags + 8, NP_STACKSIZE);
        emu68k_gwrite32(sb, ptags + 12, 16384);
        emu68k_gwrite32(sb, ptags + 16, NP_NAME);
        emu68k_gwrite32(sb, ptags + 20, name);
        /* Async commands inherit the invoking CLI's streams.  RX and other
         * shell tools legitimately read/write these handles while their
         * parent remains in a different guest process; leaving them zero
         * turns normal I/O into a tight failed-read loop. */
        emu68k_gwrite32(sb, ptags + 24, NP_INPUT);
        emu68k_gwrite32(sb, ptags + 28,
                        emu68k_gread32(sb, parent + CLASSIC_PR_CIS));
        emu68k_gwrite32(sb, ptags + 32, NP_OUTPUT);
        emu68k_gwrite32(sb, ptags + 36,
                        emu68k_gread32(sb, parent + CLASSIC_PR_COS));
        emu68k_gwrite32(sb, ptags + 40, NP_ERROR);
        emu68k_gwrite32(sb, ptags + 44,
                        emu68k_gread32(sb, parent + CLASSIC_PR_CES));
        /* A loaded executable enters the ordinary C startup, which chooses
         * Shell versus Workbench from pr_CLI and reads pr_Arguments. Without
         * these tags it waits forever for a Workbench startup message. */
        emu68k_gwrite32(sb, ptags + 48, NP_CLI);
        emu68k_gwrite32(sb, ptags + 52, 1);
        emu68k_gwrite32(sb, ptags + 56, NP_ARGUMENTS);
        emu68k_gwrite32(sb, ptags + 60, args);
        emu68k_gwrite32(sb, ptags + 64, 0);
        emu68k_gwrite32(sb, ptags + 68, 0);
        call.d[1] = ptags;
        if (emu68k_dos_create_new_proc(r, sb, &call, e, el) != 0) return 1;
        st->d[0] = call.d[0] ? 0 : 0xffffffffu;
        return 0;
    }
    {
        uint32_t seglist = call.d[0];
        call = *st; call.d[1] = seglist; call.d[2] = 16384;
        call.d[3] = args; call.d[4] = args_len + 1u;
        if (emu68k_dos_call(r, sb, DOS_LVO_RUNCOMMAND, &call, e, el) != 0)
            return 1;
        st->d[0] = call.d[0];
        call.d[1] = seglist;
        (void)emu68k_dos_unloadseg(r, &call);
        return 0;
    }
invalid_system:
    if (e && el) snprintf(e, el, "SystemTagList taglist is invalid");
    return 1;
}

int emu68k_dos_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                    struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case DOS_LVO_DELAY:
        /* Hosted guest tasks have no native tick scheduler to sleep on.  A
         * cooperative handoff is the useful Delay contract here: it lets
         * sibling processes (RexxMast, TurboCalc, RX) make progress without
         * blocking the host thread.  Preserve the guest's tick argument,
         * while bounding a single call so a damaged program cannot monopolise
         * the hosted scheduler with an unbounded delay. */
        {
            uint32_t ticks = st->d[1];
            unsigned rounds = ticks > 512u ? 512u : (unsigned)ticks;
            if (!rounds) rounds = 1u;
            for (unsigned i = 0; i < rounds; i++)
                if (emu68k_reschedule_siblings(r, sb, "Delay", st->pc,
                                               e, el) != 0)
                    return 1;
            return 0;
        }
    case DOS_LVO_CREATEPROC: {
        struct guestseg_live *seg = dos_guest_segment(r, st->d[3]);
        struct j5d_m68k_state call = *st;
        uint32_t tags, process;
        if (!seg) { st->d[0] = 0; r->last_ioerr = 205; return 0; }
        tags = emu68k_guest_alloc(r, 5u * 8u);
        if (!tags) { st->d[0] = 0; r->last_ioerr = 103; return 0; }
        emu68k_gwrite32(sb, tags + 0, NP_ENTRY);
        emu68k_gwrite32(sb, tags + 4, seg->seg.entry);
        emu68k_gwrite32(sb, tags + 8, NP_STACKSIZE);
        emu68k_gwrite32(sb, tags + 12, st->d[4]);
        emu68k_gwrite32(sb, tags + 16, NP_NAME);
        emu68k_gwrite32(sb, tags + 20, st->d[1]);
        emu68k_gwrite32(sb, tags + 24, NP_PRIORITY);
        emu68k_gwrite32(sb, tags + 28, st->d[2]);
        emu68k_gwrite32(sb, tags + 32, 0);
        emu68k_gwrite32(sb, tags + 36, 0);
        call.d[1] = tags;
        if (emu68k_dos_create_new_proc(r, sb, &call, e, el) != 0) return 1;
        process = call.d[0];
        st->d[0] = process ? process + M68K_Process_pr_MsgPort_mp_Node_ln_Succ : 0;
        return 0;
    }
    case DOS_LVO_EXIT:
        bl_event(BL_RUNTIME, r->cur_ctx, dos_current_process(r), st->pc,
                 "process.exit", "\"status\":%d", (int32_t)st->d[1]);
        if (r->command_can_unwind) {
            r->command_return = st->d[1];
            longjmp(r->command_unwind, 1);
        }
        for (int i = 0; i < r->nctx; i++) {
            struct emu68k_ctx *ctx = &r->ctx[i];
            if (!ctx->live || ctx->task != dos_current_process(r)) continue;
            ctx->finished = 1;
            ctx->blocked = 0;
            if (ctx->can_unwind) longjmp(ctx->unwind, 1);
            st->pc = 0;
            return J5D_LVO_REDIRECT;
        }
        st->pc = 0;
        return J5D_LVO_REDIRECT;
    case DOS_LVO_SENDPKT: {
        uint32_t packet = st->d[1], port = st->d[2], reply = st->d[3], msg;
        struct j5d_m68k_state call = *st;
        if (!dos_span(sb, packet, M68K_DosPacket_SIZEOF) ||
            !dos_span(sb, port, M68K_MsgPort_SIZEOF) ||
            !dos_span(sb, reply, M68K_MsgPort_SIZEOF)) goto bad;
        msg = emu68k_gread32(sb, packet + M68K_DosPacket_dp_Link);
        if (!dos_span(sb, msg, M68K_Message_SIZEOF)) goto bad;
        emu68k_gwrite32(sb, packet + M68K_DosPacket_dp_Port, reply);
        emu68k_gwrite32(sb, msg + M68K_Message_mn_ReplyPort, reply);
        call.a[0] = port; call.a[1] = msg;
        return dos_exec_call(r, sb, LVO_PUTMSG, &call, e, el);
    }
    case DOS_LVO_WAITPKT: {
        uint32_t port = dos_current_process(r) +
                        M68K_Process_pr_MsgPort_mp_Node_ln_Succ;
        struct j5d_m68k_state call = *st;
        call.a[0] = port;
        if (dos_exec_call(r, sb, LVO_WAITPORT, &call, e, el) != 0) return 1;
        call.a[0] = port;
        if (dos_exec_call(r, sb, LVO_GETMSG, &call, e, el) != 0) return 1;
        st->d[0] = call.d[0] ? emu68k_gread32(sb, call.d[0] +
                                  M68K_Message_mn_Node_ln_Name) : 0;
        return 0;
    }
    case DOS_LVO_REPLYPKT: {
        uint32_t packet = st->d[1], msg, destination;
        struct j5d_m68k_state call = *st;
        if (!dos_span(sb, packet, M68K_DosPacket_SIZEOF)) goto bad;
        msg = emu68k_gread32(sb, packet + M68K_DosPacket_dp_Link);
        destination = emu68k_gread32(sb, packet + M68K_DosPacket_dp_Port);
        if (!dos_span(sb, msg, M68K_Message_SIZEOF) ||
            !dos_span(sb, destination, M68K_MsgPort_SIZEOF)) goto bad;
        emu68k_gwrite32(sb, msg + M68K_Message_mn_Node_ln_Name, packet);
        emu68k_gwrite32(sb, packet + M68K_DosPacket_dp_Port,
            dos_current_process(r) + M68K_Process_pr_MsgPort_mp_Node_ln_Succ);
        emu68k_gwrite32(sb, packet + M68K_DosPacket_dp_Res1, st->d[2]);
        emu68k_gwrite32(sb, packet + M68K_DosPacket_dp_Res2, st->d[3]);
        call.a[0] = destination; call.a[1] = msg;
        return dos_exec_call(r, sb, LVO_PUTMSG, &call, e, el);
    }
    case DOS_LVO_ABORTPKT:
        /* AROS itself documents this compatibility entry point as a no-op. */
        return 0;
    case DOS_LVO_CLI:
        st->d[0] = dos_current_cli(r, sb);
        return 0;
    case DOS_LVO_GETARGSTR:
        st->d[0] = emu68k_gread32(sb, dos_current_process(r) +
                                      CLASSIC_PR_ARGUMENTS);
        return 0;
    case DOS_LVO_SETARGSTR: {
        uint32_t p = dos_current_process(r);
        uint32_t old = emu68k_gread32(sb, p + CLASSIC_PR_ARGUMENTS);
        if (st->d[1] && !emu68k_guest_cstr(sb, st->d[1])) goto bad;
        emu68k_gwrite32(sb, p + CLASSIC_PR_ARGUMENTS, st->d[1]);
        st->d[0] = old;
        return 0;
    }
    case DOS_LVO_FINDCLIPROC:
        st->d[0] = 0;
        for (int i = 0; i < r->nctx; i++)
            if (r->ctx[i].live && !r->ctx[i].finished &&
                emu68k_gread32(sb, r->ctx[i].task + CLASSIC_PR_TASKNUM) ==
                    st->d[1]) {
                st->d[0] = r->ctx[i].task;
                break;
            }
        if (!r->nctx && st->d[1] == 1u) st->d[0] = GUEST_PROCESS;
        return 0;
    case DOS_LVO_MAXCLI:
        st->d[0] = r->nctx ? (uint32_t)r->nctx : 1u;
        return 0;
    case DOS_LVO_SETCURRENTDIRNAME:
        st->d[0] = dos_set_cli_bstr(r, sb,
            M68K_CommandLineInterface_cli_SetName, st->d[1]) ? 0xffffffffu : 0;
        return 0;
    case DOS_LVO_SETPROGRAMNAME:
        st->d[0] = dos_set_cli_bstr(r, sb,
            M68K_CommandLineInterface_cli_CommandName, st->d[1]) ? 0xffffffffu : 0;
        return 0;
    case DOS_LVO_SETPROMPT:
        st->d[0] = dos_set_cli_bstr(r, sb,
            M68K_CommandLineInterface_cli_Prompt, st->d[1]) ? 0xffffffffu : 0;
        return 0;
    case DOS_LVO_CHECKSIGNAL: {
        uint32_t task = dos_current_process(r);
        uint32_t got = emu68k_gread32(sb, task + TASK_SIGRECVD_OFF) & st->d[1];
        emu68k_gwrite32(sb, task + TASK_SIGRECVD_OFF,
            emu68k_gread32(sb, task + TASK_SIGRECVD_OFF) & ~st->d[1]);
        st->d[0] = got;
        return 0;
    }
    case DOS_LVO_READITEM:
        return dos_read_item(sb, st->d[1], (int32_t)st->d[2], st->d[3],
                             &st->d[0], e, el);
    case DOS_LVO_VFPRINTF:
        return dos_vfprintf(r, sb, st, e, el);
    case DOS_LVO_VFWRITEF:
        return dos_vfwritef(r, sb, st, e, el);
    case DOS_LVO_EXALL:
        return dos_exall(r, sb, st, e, el);
    case DOS_LVO_EXALLEND:
        dos_exall_end(r, st->d[5]);
        return 0;
    case DOS_LVO_DISPLAYERROR: {
        uint32_t required, out;
        if (emu68k_guest_format(sb, 0, 0, st->a[0], st->a[1],
                               &required, e, el) != 0)
            return 1;
        out = required && required <= 65536u ? emu68k_guest_alloc(r, required) : 0;
        if (!out || emu68k_guest_format(sb, out, required, st->a[0], st->a[1],
                                       &required, e, el) != 0)
            return 1;
        if (r->sink && required > 1u)
            r->sink((const char *)j4_sandbox_host(sb, out),
                    (long)required - 1, r->sink_user);
        st->d[0] = 1; /* hosted policy: requester unavailable/cancelled */
        return 0;
    }
    case DOS_LVO_RUNCOMMAND: {
        struct guestseg_live *seg = dos_guest_segment(r, st->d[1]);
        struct j5d_m68k_state call = *st;
        uint32_t stack_size = st->d[2] < 16384u ? 16384u : st->d[2];
        uint32_t stack, result, process, saved_arguments;
        if (!seg || stack_size > 16u * 1024u * 1024u) {
            st->d[0] = 20; return 0;
        }
        if (st->d[4] && !dos_span(sb, st->d[3], st->d[4])) goto bad;
        stack = emu68k_guest_alloc(r, stack_size);
        if (!stack) { st->d[0] = 20; return 0; }
        memset(&call, 0, sizeof call);
        call.a[0] = st->d[3];
        call.d[0] = st->d[4];
        /* AROS startup receives the command tail in A0/D0, but established
         * commands such as RX also reread pr_Arguments. RunCommand executes in
         * the current Process, so expose this invocation's tail there only for
         * its dynamic extent and restore the caller's tail afterwards. */
        process = dos_current_process(r);
        saved_arguments = emu68k_gread32(sb,
                                         process + CLASSIC_PR_ARGUMENTS);
        emu68k_gwrite32(sb, process + CLASSIC_PR_ARGUMENTS, st->d[3]);
        if (emu68k_run_guest_command(r, seg->seg.entry, &call,
                (stack + stack_size) & ~15u, &result, e, el) != 0) {
            emu68k_gwrite32(sb, process + CLASSIC_PR_ARGUMENTS,
                            saved_arguments);
            return 1;
        }
        emu68k_gwrite32(sb, process + CLASSIC_PR_ARGUMENTS, saved_arguments);
        st->d[0] = result;
        return 0;
    }
    case DOS_LVO_SYSTEMTAGLIST:
        return dos_system(r, sb, st, e, el);
    case DOS_LVO_INTERNALLOADSEG:
        /* This private loader entry is parameterised by arbitrary allocator,
         * reader and callback vectors.  The guest-visible loader cannot reuse
         * native function pointers; report the normal DOS failure contract so
         * callers can fall back to LoadSeg without terminating the run. */
        r->last_ioerr = DOS_ERROR_NOT_IMPLEMENTED;
        st->d[0] = 0;
        return 0;
    case DOS_LVO_INTERNALUNLOADSEG:
        return emu68k_dos_unloadseg(r, st);
    case DOS_LVO_ADDSEGMENT: {
        struct guestseg_live *seg = dos_guest_segment(r, st->d[2]);
        const char *name = emu68k_guest_cstr(sb, st->d[1]);
        if (!seg || !name) { st->d[0] = 0; return 0; }
        snprintf(seg->name, sizeof seg->name, "%s", name);
        st->d[0] = 0xffffffffu;
        return 0;
    }
    case DOS_LVO_GETSEGLISTINFO: {
        struct guestseg_live *seg = dos_guest_segment(r, st->d[0]);
        uint32_t tags = st->a[0], count = 0;
        if (!seg) { st->d[0] = 0; return 0; }
        for (unsigned guard = 0; tags && guard < 4096u; guard++, tags += 8u) {
            uint32_t tag, data;
            if (!dos_span(sb, tags, 8)) goto bad;
            tag = emu68k_gread32(sb, tags);
            data = emu68k_gread32(sb, tags + 4);
            if (!tag) break;
            if (tag == 1u) continue;
            if (tag == 2u) { tags = data - 8u; continue; }
            if (tag == 3u) { tags += data * 8u; continue; }
            if (tag == GSLI_68KHUNK && data && dos_span(sb, data, 4)) {
                emu68k_gwrite32(sb, data, seg->bptr); count++;
            }
        }
        st->d[0] = count;
        return 0;
    }
    case DOS_LVO_FINDVAR:
        /* Guest-local variables are not yet populated; an absent variable is
         * the documented result, unlike exposing the native Process list. */
        st->d[0] = 0;
        return 0;
    case DOS_LVO_SCANVARS:
        st->d[0] = 0; /* the guest-local variable list is empty */
        return 0;
    case DOS_LVO_CLIINIT:
    case DOS_LVO_CLIINITNEWCLI:
    case DOS_LVO_CLIINITRUN:
        st->d[0] = DOS_ERROR_NOT_IMPLEMENTED;
        return 0;
    case DOS_LVO_FORMAT:
    case DOS_LVO_RELABEL:
    case DOS_LVO_INHIBIT:
    case DOS_LVO_ADDBUFFERS:
    case DOS_LVO_SETOWNER:
        /* An isolated hosted guest has no authority to mutate host volume or
         * device state. Return the ordinary DOS failure contract instead of
         * terminating the program at a bridge capability gap. */
        r->last_ioerr = DOS_ERROR_ACTION_NOT_KNOWN;
        st->d[0] = 0;
        return 0;
    case DOS_LVO_LOADSEG:
        return emu68k_dos_loadseg(r, sb, st, e, el);
    case DOS_LVO_NEWLOADSEG:
        if (st->d[2] && (!dos_span(sb, st->d[2], 8) ||
                         emu68k_gread32(sb, st->d[2]) != 0)) {
            if (e && el) snprintf(e, el,
                "NewLoadSeg accepts only an empty taglist in the guest loader");
            return 1;
        }
        return emu68k_dos_loadseg(r, sb, st, e, el);
    case DOS_LVO_UNLOADSEG:
        return emu68k_dos_unloadseg(r, st);
    case DOS_LVO_READARGS:
        return emu68k_dos_readargs(r, sb, st, e, el);
    case DOS_LVO_FREEARGS:
        /* ReadArgs and every result it owns use the run's monotonic guest
         * allocator, so there is no native allocation to release. */
        st->d[0] = 0;
        return 0;
    case DOS_LVO_IOERR:
        if (r->last_ioerr) {
            st->d[0] = r->last_ioerr;
            return 0;
        }
        return 1; /* otherwise ask native DOS for the preceding native call */
    case DOS_LVO_ALLOCDOSOBJECT:
        /* FileInfoBlock is currently the largest public DosObject payload.
         * The caller reads and writes it, so identity must be guest memory. */
        st->d[0] = emu68k_guest_alloc(r, 512);
        return 0;
    case DOS_LVO_FREEDOSOBJECT:
        st->d[0] = 0;
        return 0;
    case DOS_LVO_CREATENEWPROC:
        return emu68k_dos_create_new_proc(r, sb, st, e, el);
    case DOS_LVO_VPRINTF: {
        uint32_t required, out;
        if (emu68k_guest_format(sb, 0, 0, st->d[1], st->d[2],
                               &required, e, el) != 0)
            return 1;
        if (!required || required > 65536u) {
            if (e && el) snprintf(e, el, "dos.VPrintf output is too large");
            return 1;
        }
        out = emu68k_guest_alloc(r, required);
        if (!out || emu68k_guest_format(sb, out, required, st->d[1], st->d[2],
                                       &required, e, el) != 0)
            return 1;
        if (r->sink && required > 1u)
            r->sink((const char *)j4_sandbox_host(sb, out),
                    (long)required - 1, r->sink_user);
        st->d[0] = required - 1u;
        return 0;
    }
    default:
        /* A native DOS operation owns the next IoErr result.  Handwritten
         * operations set this field again when they fail in guest space. */
        r->last_ioerr = 0;
        return 1;
    }
bad:
    if (e && el) snprintf(e, el, "dos guest-memory argument is out of range");
    return 1;
}
