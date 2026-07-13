/* sys/ioctl.h -- declaration-only shim for aarch64-unknown-aros.
 *
 * AROS posixc has no ioctl (device control goes through exec devices).
 * sqlite3.c includes this header unconditionally in its unix VFS but only
 * *calls* ioctl behind __linux__ guards (F2FS batched writes) and in WAL
 * shm paths we compile out (SQLITE_OMIT_WAL, SQLITE_MAX_MMAP_SIZE=0), so a
 * declaration satisfies the compile and nothing references it at link.
 */
#ifndef _AROS_COMPAT_SYS_IOCTL_H
#define _AROS_COMPAT_SYS_IOCTL_H

extern int ioctl(int fd, unsigned long request, ...);

#endif /* _AROS_COMPAT_SYS_IOCTL_H */
