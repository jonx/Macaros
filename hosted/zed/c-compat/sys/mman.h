/* Minimal <sys/mman.h> for AROS C cross-builds.
   AROS posixc has no mmap; this provides the declarations C deps (sqlite's WAL
   path) reference so they compile. mmap returns MAP_FAILED at runtime, which
   sqlite handles by falling back to heap-based shared memory. */
#ifndef _AROS_COMPAT_SYS_MMAN_H
#define _AROS_COMPAT_SYS_MMAN_H

#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED  0x0001
#define MAP_PRIVATE 0x0002
#define MAP_FIXED   0x0010
#define MAP_ANON    0x1000
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED  ((void *)-1)

#define MS_ASYNC      0x1
#define MS_INVALIDATE 0x2
#define MS_SYNC       0x4

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t len, int flags);
int   getpagesize(void);

#ifdef __cplusplus
}
#endif

#endif
