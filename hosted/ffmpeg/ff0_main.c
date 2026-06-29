/* ff0_main.c — [FF0] AROS C: smoke: libavutil, built natively by the AROS crosstools,
 * linked into a real AROS command, and run on booted AROS.
 *
 * Proves the toolchain + C runtime carry a real library: the version string comes
 * from inside libavutil.a, and av_malloc/av_mallocz/av_free exercise ffmpeg's
 * allocator on this target. One PASS/FAIL line per check; returns 0 on full PASS,
 * 20 (FAILAT trips) otherwise. See docs/features/ffmpeg-native/README.md, NOTES.md.
 *
 * Output goes through dos PutStr, NOT printf: printf pulls stdcio.library, which is
 * not present in every AROS distribution (e.g. /tmp/arosbuild), and the smoke proves
 * libavutil, not stdio. Same header-clean approach as the Rust [RS0] harness. The
 * version int is formatted by hand (no sprintf) to keep the stdcio dependency off.
 */
#include <proto/dos.h>      /* PutStr; no <stdio.h> */
#include <libavutil/avutil.h>
#include <libavutil/mem.h>

static void put(const char *s) { PutStr((CONST_STRPTR)s); }

/* unsigned -> decimal, libc-free, into buf (caller gives enough room); returns buf */
static const char *udec(unsigned v, char *buf)
{
    char tmp[12]; int i = 0, j = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

int main(void)
{
    int ok = 1;
    const char *ver = av_version_info();
    unsigned v = avutil_version();
    unsigned char *p, *z;
    unsigned i, nz, n = 4096;
    char b[12];

    put("[FF0] libavutil ");
    put(udec(AV_VERSION_MAJOR(v), b)); put(".");
    put(udec(AV_VERSION_MINOR(v), b)); put(".");
    put(udec(AV_VERSION_MICRO(v), b));
    put("  version_info="); put(ver ? ver : "(null)"); put("\n");
    if (!ver || !ver[0]) { put("[FF0] FAIL: av_version_info() empty\n"); ok = 0; }

    /* av_malloc round-trip: non-null + writable */
    p = av_malloc(n);
    if (!p) { put("[FF0] FAIL: av_malloc -> NULL\n"); ok = 0; }
    else {
        for (i = 0; i < n; i++) p[i] = 0xA5;
        if (p[0] != 0xA5 || p[n - 1] != 0xA5) { put("[FF0] FAIL: av_malloc not writable\n"); ok = 0; }
        else put("[FF0] av_malloc(4096) ok, writable\n");
        av_free(p);
    }

    /* av_mallocz: must come back zeroed */
    z = av_mallocz(n);
    if (!z) { put("[FF0] FAIL: av_mallocz -> NULL\n"); ok = 0; }
    else {
        for (i = 0, nz = 0; i < n; i++) if (z[i]) nz++;
        if (nz) { put("[FF0] FAIL: av_mallocz not zeroed\n"); ok = 0; }
        else put("[FF0] av_mallocz(4096) ok, zeroed\n");
        av_free(z);
    }

    put(ok ? "FFMPEG-AROS: [FF0] ALL PASS\n" : "FFMPEG-AROS: [FF0] FAIL\n");
    return ok ? 0 : 20;
}
