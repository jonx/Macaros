/* aros_fd_shim.c -- a unified-fd shim so the async socket stack
 * (async-io / socket2 / mio / tokio) runs on AROS.
 *
 * Those crates drive sockets as raw libc fds: socket()/connect()/fcntl()/
 * read()/write()/close(). AROS has no libc socket layer -- sockets are
 * bsdsocket.library LVOs -- and keeps sockets and dos files in *separate* fd
 * spaces with overlapping small-integer fds. This shim unifies them:
 *
 *  - socket()/accept() return a fd tagged with SOCK_TAG (a high bit), so socket
 *    fds never collide with posixc file fds.
 *  - The socket calls (connect/bind/.../recv/send/setsockopt/...) route the
 *    untagged fd to the bsdsocket LVO.
 *  - read()/write()/close()/fcntl()/ioctl() are provided here (posixc's are
 *    weak, so these win) and DISPATCH: a tagged fd goes to bsdsocket, everything
 *    else forwards to the real posixc function (__*_PosixCBase_wrapper).
 *  - On a socket error we copy the bsdsocket errno (BSD numbering, already
 *    host-translated) into the C errno, so callers see EWOULDBLOCK (-> WouldBlock)
 *    etc.
 *
 * Pragmatic first cut (make it work); a more AROS-native design can follow.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <aros/libcall.h>
#include <stdarg.h>

typedef unsigned int socklen_t;
struct sockaddr { unsigned char sa_len; unsigned char sa_family; char sa_data[14]; };

#define FIONBIO     0x8004667eUL
#define F_GETFL     3
#define F_SETFL     4
#define O_NONBLOCK  0x0800

#include <defines/bsdsocket.h>  /* socket/connect/... LVO macros via SocketBase */

extern struct Library *SocketBase;
extern int aros_net_open(void);

/* The real posixc file-path functions (posixc's weak read/write/... alias these). */
extern long __read_PosixCBase_wrapper(int fd, void *buf, unsigned long n);
extern long __write_PosixCBase_wrapper(int fd, const void *buf, unsigned long n);
extern int  __close_PosixCBase_wrapper(int fd);
extern int  __fcntl_PosixCBase_wrapper(int fd, int cmd, long arg);
extern int  __ioctl_PosixCBase_wrapper(int fd, unsigned long req, void *arg);

/* The C errno slot (per-thread since the stdc/pthread errno work). */
extern int *__stdc_geterrnoptr(void);

#define SOCK_TAG    0x40000000
#define IS_SOCK(fd) (((fd) & SOCK_TAG) != 0)
#define RAW(fd)     ((fd) & ~SOCK_TAG)

/* Undo the LVO macros so we can define functions with these names and reach the
 * LVOs through the _WB wrappers (SocketBase-in-a-register calling convention). */
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef shutdown

/* Copy the bsdsocket errno into the C errno after a failed socket op. */
static void sock_errno(void)
{
    int *e = __stdc_geterrnoptr();
    if (e)
        *e = SocketBase ? Errno() : 0;
}

int socket(int domain, int type, int protocol)
{
    int s, nb;
    aros_net_open();
    nb = (type & O_NONBLOCK) ? 1 : 0;          /* SOCK_NONBLOCK folded into type */
    s = __socket_WB(SocketBase, domain, type & 0xff, protocol);
    if (s < 0) { sock_errno(); return -1; }
    if (nb) { int on = 1; __IoctlSocket_WB(SocketBase, s, FIONBIO, (char *)&on); }
    return s | SOCK_TAG;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    int r = __connect_WB(SocketBase, RAW(fd), (struct sockaddr *)addr, len);
    if (r < 0) sock_errno();
    return r;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    int r = __bind_WB(SocketBase, RAW(fd), (struct sockaddr *)addr, len);
    if (r < 0) sock_errno();
    return r;
}

int listen(int fd, int backlog)
{
    int r = __listen_WB(SocketBase, RAW(fd), backlog);
    if (r < 0) sock_errno();
    return r;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    int s = __accept_WB(SocketBase, RAW(fd), addr, len);
    if (s < 0) { sock_errno(); return -1; }
    return s | SOCK_TAG;
}

long send(int fd, const void *buf, unsigned long n, int flags)
{
    long r = __send_WB(SocketBase, RAW(fd), (void *)buf, n, flags);
    if (r < 0) sock_errno();
    return r;
}

long recv(int fd, void *buf, unsigned long n, int flags)
{
    long r = __recv_WB(SocketBase, RAW(fd), buf, n, flags);
    if (r < 0) sock_errno();
    return r;
}

long sendto(int fd, const void *buf, unsigned long n, int flags,
            const struct sockaddr *addr, socklen_t alen)
{
    long r = __sendto_WB(SocketBase, RAW(fd), (void *)buf, n, flags,
                         (struct sockaddr *)addr, alen);
    if (r < 0) sock_errno();
    return r;
}

long recvfrom(int fd, void *buf, unsigned long n, int flags,
              struct sockaddr *addr, socklen_t *alen)
{
    long r = __recvfrom_WB(SocketBase, RAW(fd), buf, n, flags, addr, alen);
    if (r < 0) sock_errno();
    return r;
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    int r = __setsockopt_WB(SocketBase, RAW(fd), level, name, (void *)val, len);
    if (r < 0) sock_errno();
    return r;
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    int r = __getsockopt_WB(SocketBase, RAW(fd), level, name, val, len);
    if (r < 0) sock_errno();
    return r;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    int r = __getsockname_WB(SocketBase, RAW(fd), addr, len);
    if (r < 0) sock_errno();
    return r;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    int r = __getpeername_WB(SocketBase, RAW(fd), addr, len);
    if (r < 0) sock_errno();
    return r;
}

int shutdown(int fd, int how)
{
    int r = __shutdown_WB(SocketBase, RAW(fd), how);
    if (r < 0) sock_errno();
    return r;
}

/* -- the dispatchers (socket fd -> bsdsocket, else -> real posixc) ---------- */

long read(int fd, void *buf, unsigned long n)
{
    if (IS_SOCK(fd)) {
        long r = __recv_WB(SocketBase, RAW(fd), buf, n, 0);
        if (r < 0) sock_errno();
        return r;
    }
    return __read_PosixCBase_wrapper(fd, buf, n);
}

long write(int fd, const void *buf, unsigned long n)
{
    if (IS_SOCK(fd)) {
        long r = __send_WB(SocketBase, RAW(fd), (void *)buf, n, 0);
        if (r < 0) sock_errno();
        return r;
    }
    return __write_PosixCBase_wrapper(fd, buf, n);
}

int close(int fd)
{
    if (IS_SOCK(fd)) {
        __CloseSocket_WB(SocketBase, RAW(fd));
        return 0;
    }
    return __close_PosixCBase_wrapper(fd);
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);

    if (IS_SOCK(fd)) {
        if (cmd == F_SETFL) {
            int on = (arg & O_NONBLOCK) ? 1 : 0;
            __IoctlSocket_WB(SocketBase, RAW(fd), FIONBIO, (char *)&on);
            return 0;
        }
        if (cmd == F_GETFL)
            return 0;  /* flags are opaque; callers set them explicitly */
        return 0;      /* F_SETFD/F_GETFD (CLOEXEC) are no-ops on AROS */
    }
    return __fcntl_PosixCBase_wrapper(fd, cmd, arg);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (IS_SOCK(fd)) {
        if (req == FIONBIO) {
            __IoctlSocket_WB(SocketBase, RAW(fd), FIONBIO, (char *)arg);
            return 0;
        }
        return 0;
    }
    return __ioctl_PosixCBase_wrapper(fd, req, arg);
}
