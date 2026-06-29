/* aros-compat.h — prototypes ffmpeg needs that AROS's posixc headers don't expose in
 * ffmpeg's include mode, even though libposixc.a provides the symbols (verified:
 * weak `fdopen`/`mkstemp` present). Force-included into every ffmpeg compile via
 * `--extra-cflags=-include …/aros-compat.h`.
 *
 * Deliberately CORRECT signatures (not a blanket -Wno-implicit-function-declaration):
 * `fdopen` returns a pointer, so an implicit `int fdopen()` would truncate it to 32
 * bits on this LP64 target. The proper fix is to expose these in the AROS posixc
 * headers (upstream `compiler/crt/posixc`); this shim unblocks the native port
 * without touching the OS tree. Grow only as the build actually reports a gap.
 * See NOTES.md "[FF0]" and docs/features/ffmpeg-native/README.md.
 */
#ifndef AROS_FFMPEG_COMPAT_H
#define AROS_FFMPEG_COMPAT_H

#include <stdio.h>      /* FILE */
#include <stddef.h>     /* size_t */

FILE *fdopen(int fd, const char *mode);              /* guarded out of posixc/stdio.h */
int   mkstemp(char *template_);                       /* absent from posixc headers */
char *tempnam(const char *dir, const char *pfx);      /* absent; in libposixc.a */
int   posix_memalign(void **memptr, size_t align, size_t size);  /* absent; in libposixc.a */

#endif /* AROS_FFMPEG_COMPAT_H */
