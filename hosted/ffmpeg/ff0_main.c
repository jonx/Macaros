/* ff0_main.c — [FF0] AROS C: smoke: libavutil, built natively by the AROS crosstools,
 * linked into a real AROS command, and run on booted AROS.
 *
 * Proves the toolchain + C runtime carry a real library: the version string comes
 * from inside libavutil.a, and av_malloc/av_mallocz/av_free exercise ffmpeg's
 * allocator over the AROS C library on this target. One PASS/FAIL line per check;
 * returns 0 on full PASS, 20 (FAILAT trips) otherwise. See
 * docs/features/ffmpeg-native/README.md and NOTES.md "[FF0]".
 */
#include <stdio.h>
#include <string.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>

int main(void)
{
    int ok = 1;
    const char *ver = av_version_info();
    unsigned v = avutil_version();
    unsigned char *p, *z;
    size_t n = 4096, i, nz;

    printf("[FF0] libavutil %u.%u.%u  version_info=\"%s\"\n",
           AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v),
           ver ? ver : "(null)");
    if (!ver || !ver[0]) { printf("[FF0] FAIL: av_version_info() empty\n"); ok = 0; }

    /* av_malloc round-trip: non-null + writable */
    p = av_malloc(n);
    if (!p) { printf("[FF0] FAIL: av_malloc(%zu) -> NULL\n", n); ok = 0; }
    else {
        memset(p, 0xA5, n);
        if (p[0] != 0xA5 || p[n - 1] != 0xA5) {
            printf("[FF0] FAIL: av_malloc block not writable\n"); ok = 0;
        } else {
            printf("[FF0] av_malloc(%zu) ok, writable\n", n);
        }
        av_free(p);
    }

    /* av_mallocz: must come back zeroed */
    z = av_mallocz(n);
    if (!z) { printf("[FF0] FAIL: av_mallocz(%zu) -> NULL\n", n); ok = 0; }
    else {
        for (i = 0, nz = 0; i < n; i++) if (z[i]) nz++;
        if (nz) { printf("[FF0] FAIL: av_mallocz left %zu non-zero bytes\n", nz); ok = 0; }
        else    { printf("[FF0] av_mallocz(%zu) ok, zeroed\n", n); }
        av_free(z);
    }

    printf("%s\n", ok ? "FFMPEG-AROS: [FF0] ALL PASS" : "FFMPEG-AROS: [FF0] FAIL");
    return ok ? 0 : 20;
}
