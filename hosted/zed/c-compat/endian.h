/* Minimal little-endian <endian.h> for aarch64 AROS C cross-builds.
   AROS's posixc has no <endian.h>; C crates (tree-sitter, ...) expect the
   glibc/BSD byte-order helpers. AROS on aarch64 is always little-endian. */
#ifndef _AROS_COMPAT_ENDIAN_H
#define _AROS_COMPAT_ENDIAN_H

#include <stdint.h>

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN    4321
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER    __LITTLE_ENDIAN
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER    __BYTE_ORDER
#endif

#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))

#define htobe16(x) __builtin_bswap16((uint16_t)(x))
#define htobe32(x) __builtin_bswap32((uint32_t)(x))
#define htobe64(x) __builtin_bswap64((uint64_t)(x))
#define be16toh(x) __builtin_bswap16((uint16_t)(x))
#define be32toh(x) __builtin_bswap32((uint32_t)(x))
#define be64toh(x) __builtin_bswap64((uint64_t)(x))

#endif
