/* hv_charset.c — AROS Latin-1 <-> host UTF-8 filename glue (R-CHARSET/R-ESCAPE).
 *
 * Implemented clean-room from docs/features/host-volume/spec.md (§Charset,
 * Requirements R-CHARSET and R-ESCAPE). No GPL emulator source (UAE family or
 * vAmiga) was read, searched, or consulted. ISO-8859-1 + UTF-8 are published
 * standards [PUB]; the reversible hex escape is the standard percent/hex escape
 * of un-representable code points [PUB]. The marker byte ('%'), the "%uXXXX"
 * format, and the self-escaping-of-the-marker rule are OURS (restated in the
 * spec), copied from no reference.
 *
 * AROS/AmigaOS filenames are conventionally ISO-8859-1; macOS paths are UTF-8.
 *   Latin-1 -> UTF-8 : total for raw bytes, AND it DECODES our "%uXXXX" escapes
 *                      back to the real code point (so the transform genuinely
 *                      round-trips — an un-mappable host name escaped on the way
 *                      in is reconstructed byte-identically on the way out).
 *   UTF-8 -> Latin-1 : partial; code points > U+00FF have no Latin-1 byte, so
 *                      we emit a FIXED-WIDTH, self-delimiting reversible escape:
 *                      "%uXXXX" (exactly 4 uppercase hex) for a BMP code point
 *                      (<= U+FFFF) and "%UXXXXXX" (exactly 6 hex) for an astral
 *                      one. The marker byte '%' is itself escaped (as "%u0025")
 *                      on the way IN so the transform is symmetric and an AROS
 *                      name containing '%' survives a host round-trip. Fixed
 *                      width is essential: a variable-length hex run would let a
 *                      hex digit in the following text (e.g. the 'd' in "done")
 *                      bleed into the escape, breaking the decode.
 *
 * Ambiguity policy (OURS): the encode (UTF-8 -> Latin-1) ALWAYS escapes a literal
 * '%' as "%u0025", so any "%u" + 4 hex (or "%U" + 6 hex) that appears in an AROS
 * name is unambiguously one of OUR escapes and is decoded; a bare '%' NOT in that
 * exact shape is a literal '%' and passes through unchanged. Residual ambiguity:
 * a user who literally types "%uXXXX"/"%UXXXXXX" in an AROS name will see it
 * decoded to the corresponding code point on the way to the host (documented in
 * the spec). The decode is intentionally conservative and FIXED-WIDTH — only the
 * exact "%u"+4hex / "%U"+6hex shapes are treated as escapes — so ordinary names
 * with stray '%' survive untouched.
 */
#include "hostvolume.h"
#include <string.h>

#define HV_MARK '%'

/* Decode one UTF-8 code point at *p; advance *p past it. Mirrors hv_norm's
 * decoder; kept local so each translation unit is independent. */
static uint32_t u8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = s[0];
    if (c < 0x80) { *p = s + 1; return c; }
    int n; uint32_t min;
    if      ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; min = 0x10000; }
    else { *p = s + 1; return s[0]; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + 1; return s[0]; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *p = s + 1; return s[0];
    }
    *p = s + n + 1;
    return c;
}

/* snprintf-style emit of one byte. */
static size_t put1(char b, char *dst, size_t need, size_t cap) {
    if (need + 1 < cap) dst[need] = b;
    return need + 1;
}

/* Emit one code point as UTF-8 (snprintf-style; advances/returns need). */
static size_t put_cp_utf8(uint32_t c, char *dst, size_t need, size_t cap) {
    if (c < 0x80) {
        need = put1((char)c, dst, need, cap);
    } else if (c < 0x800) {
        need = put1((char)(0xC0 | (c >> 6)), dst, need, cap);
        need = put1((char)(0x80 | (c & 0x3F)), dst, need, cap);
    } else if (c < 0x10000) {
        need = put1((char)(0xE0 | (c >> 12)), dst, need, cap);
        need = put1((char)(0x80 | ((c >> 6) & 0x3F)), dst, need, cap);
        need = put1((char)(0x80 | (c & 0x3F)), dst, need, cap);
    } else {
        need = put1((char)(0xF0 | (c >> 18)), dst, need, cap);
        need = put1((char)(0x80 | ((c >> 12) & 0x3F)), dst, need, cap);
        need = put1((char)(0x80 | ((c >> 6) & 0x3F)), dst, need, cap);
        need = put1((char)(0x80 | (c & 0x3F)), dst, need, cap);
    }
    return need;
}

/* Hex digit value, or -1 if not a hex digit (uppercase canonical, lowercase
 * tolerated so a hand-typed escape still decodes). */
static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

/* Read exactly `ndig` hex digits at s into *v; return 1 on success (all hex),
 * 0 otherwise. Stops reading at the first non-hex (incl. the NUL terminator),
 * so it never runs past the string. */
static int read_fixed_hex(const unsigned char *s, int ndig, uint32_t *v) {
    uint32_t acc = 0;
    for (int i = 0; i < ndig; i++) {
        int h = hexval(s[i]);
        if (h < 0) return 0;
        acc = (acc << 4) | (uint32_t)h;
    }
    *v = acc;
    return 1;
}

/* Decode one of our FIXED-WIDTH, self-delimiting escapes at s:
 *   "%u" + exactly 4 hex  -> a BMP code point (U+0000..U+FFFF)
 *   "%U" + exactly 6 hex  -> any code point  (U+000000..U+10FFFF)
 * Fixed width is what makes the escape unambiguous: a hex digit immediately
 * following the escape (e.g. the 'd' in "...done") is NOT absorbed, so
 * "%u0025done" decodes to "%done", not a 5-hex-digit run. Returns 1 and fills
 * cp + consumed on a valid escape, else 0 (s is a literal byte run). */
static int decode_escape(const unsigned char *s, uint32_t *cp, size_t *consumed) {
    if (s[0] != HV_MARK) return 0;
    if (s[1] == 'u') {
        uint32_t v;
        if (!read_fixed_hex(s + 2, 4, &v)) return 0;
        *cp = v; *consumed = 6;               /* "%u" + 4 */
        return 1;
    }
    if (s[1] == 'U') {
        uint32_t v;
        if (!read_fixed_hex(s + 2, 6, &v)) return 0;
        if (v > 0x10FFFF) return 0;           /* not a valid code point: literal */
        *cp = v; *consumed = 8;               /* "%U" + 6 */
        return 1;
    }
    return 0;
}

size_t hv_latin1_to_utf8(const char *latin1, char *dst, size_t dstcap) {
    const unsigned char *s = (const unsigned char *)latin1;
    size_t need = 0;
    while (*s) {
        uint32_t cp; size_t consumed;
        if (decode_escape(s, &cp, &consumed)) {
            /* Our fixed-width reversible escape (incl. the marker's own
             * "%u0025") -> the real code point as UTF-8. This is what makes the
             * bridge a genuine round-trip: f%u0153.txt -> fœ.txt, %u0025 -> %. */
            need = put_cp_utf8(cp, dst, need, dstcap);
            s += consumed;
        } else {
            uint32_t c = *s++;               /* Latin-1 byte == code point */
            need = put_cp_utf8(c, dst, need, dstcap);
        }
    }
    if (dstcap > 0) dst[(need < dstcap) ? need : dstcap - 1] = '\0';
    return need;
}

/* Emit a FIXED-WIDTH, self-delimiting escape for a code point:
 *   c <= U+FFFF   -> "%u" + exactly 4 uppercase hex
 *   c >  U+FFFF   -> "%U" + exactly 6 uppercase hex
 * Fixed width (paired with the fixed-width decoder) makes the escape boundary
 * unambiguous regardless of the byte that follows. */
static size_t put_escape(uint32_t c, char *dst, size_t need, size_t cap) {
    static const char hexd[] = "0123456789ABCDEF";
    int nd = (c <= 0xFFFF) ? 4 : 6;
    need = put1(HV_MARK, dst, need, cap);
    need = put1((nd == 4) ? 'u' : 'U', dst, need, cap);
    for (int sh = (nd - 1) * 4; sh >= 0; sh -= 4)
        need = put1(hexd[(c >> sh) & 0xF], dst, need, cap);
    return need;
}

size_t hv_utf8_to_latin1(const char *utf8, char *dst, size_t dstcap) {
    const unsigned char *p = (const unsigned char *)utf8;
    size_t need = 0;
    while (*p) {
        uint32_t c = u8_next(&p);
        if (c == HV_MARK) {              /* escape the marker itself (symmetry) */
            need = put_escape(c, dst, need, dstcap);
        } else if (c <= 0xFF) {          /* representable as one Latin-1 byte */
            need = put1((char)c, dst, need, dstcap);
        } else {                         /* un-mappable -> reversible escape */
            need = put_escape(c, dst, need, dstcap);
        }
    }
    if (dstcap > 0) dst[(need < dstcap) ? need : dstcap - 1] = '\0';
    return need;
}
