/*===---- stdalign.h - Standard header for alignment ------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 *
 * Vendored copy of clang's freestanding <stdalign.h>. The AROS crosstools
 * clang ships this in its resource dir, but that dir is a preserved binary
 * outside the repo and a scrub/re-clone can drop this one header (only the
 * ffmpeg build includes it; the AROS SDK C does not). aros-cc.sh adds this
 * dir with -idirafter, so the toolchain's own copy wins when present and this
 * fills the gap only when it is missing. Pure macros, no compiler intrinsics,
 * so it is safe across clang versions.
 */

#ifndef __STDALIGN_H
#define __STDALIGN_H

#if defined(__cplusplus) ||                                                    \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ < 202311L)
#ifndef __cplusplus
#define alignas _Alignas
#define alignof _Alignof
#endif

#define __alignas_is_defined 1
#define __alignof_is_defined 1
#endif /* __STDC_VERSION__ */

#endif /* __STDALIGN_H */
