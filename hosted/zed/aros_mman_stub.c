/* aros_mman_stub.c -- mmap/munmap leaves for AROS (which has no VM mmap).
 *
 * sqlite (WAL path) and memmap2 (memory-mapped file reads, e.g. fonts)
 * reference these. Rather than fail at runtime, back them with the heap:
 *  - anonymous maps (fd < 0)   -> zeroed malloc block
 *  - file-backed maps (fd >=0) -> malloc block filled by pread (read-only)
 * Writes are not synced back, which is fine for the read-only maps on the
 * editor boot path. getpagesize is a fixed 4096. */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MMAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    (void)addr;
    (void)prot;
    (void)flags;
    if (len == 0) {
        len = 1;
    }
    void *p = malloc(len);
    if (!p) {
        return MMAP_FAILED;
    }
    if (fd < 0) {
        memset(p, 0, len);
        return p;
    }
    /* posixc has no pread; seek+read is fine for these one-shot read-only maps. */
    if (lseek(fd, offset, SEEK_SET) < 0) {
        free(p);
        return MMAP_FAILED;
    }
    ssize_t n = read(fd, p, len);
    if (n < 0) {
        free(p);
        return MMAP_FAILED;
    }
    if ((size_t)n < len) {
        memset((char *)p + n, 0, len - (size_t)n);
    }
    return p;
}

int munmap(void *addr, size_t len)
{
    (void)len;
    free(addr);
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

int msync(void *addr, size_t len, int flags)
{
    (void)addr;
    (void)len;
    (void)flags;
    return 0;
}

int getpagesize(void)
{
    return 4096;
}
