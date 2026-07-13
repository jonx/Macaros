/* endian.h -- byte-order shim for aarch64-unknown-aros (little-endian).
 *
 * AROS ships no <endian.h>; vendored C (tree-sitter's portable/endian.h
 * honors HAVE_ENDIAN_H) expects the glibc/BSD macro set. aarch64 AROS is
 * little-endian, so the host-order conversions are identity and the
 * big-endian ones byte-swap.
 */
#ifndef _AROS_COMPAT_ENDIAN_H
#define _AROS_COMPAT_ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412
#define __BYTE_ORDER    __LITTLE_ENDIAN

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER      __BYTE_ORDER
#endif

#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) ((__UINT16_TYPE__)(x))
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) ((__UINT16_TYPE__)(x))

#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) ((__UINT32_TYPE__)(x))
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) ((__UINT32_TYPE__)(x))

#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) ((__UINT64_TYPE__)(x))
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) ((__UINT64_TYPE__)(x))

#endif /* _AROS_COMPAT_ENDIAN_H */
