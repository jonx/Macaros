/* aros_net_glue.c -- flat-C boundary between Rust and the host-passthrough
 * bsdsocket.library, mirroring aros_rt_glue.c. Rust can't call the inline LVO
 * stubs (socket/connect/... dispatch through SocketBase), so this exposes plain
 * extern "C" wrappers Rust drives over FFI.
 *
 * Header-clean on purpose: the AmiTCP <sys/socket.h>/<netinet/in.h> pull the macOS
 * SDK on this backend (UPSTREAM-NOTES #9/#16, the same trap aros_rt_glue.c dodges),
 * so we declare the few socket types ourselves (NetBSD numbering, which AROS's
 * bsdsocket.library uses) and include only <defines/bsdsocket.h> for the inline
 * LVO stubs. Compiled with -ffixed-x18 so the bridge can't park a value in the
 * reserved platform register. Every wrapper guards SocketBase so a misordered call
 * returns an error instead of dereferencing NULL.
 *
 * Independent work: written from the AROS bsdsocket.library autodocs and this
 * project's own socktest.c usage pattern; no third-party source consulted.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <aros/libcall.h>

typedef unsigned int socklen_t;
struct sockaddr   { unsigned char sa_len; unsigned char sa_family; char sa_data[14]; };
struct in_addr    { unsigned int s_addr; };
struct sockaddr_in {
    unsigned char  sin_len;
    unsigned char  sin_family;
    unsigned short sin_port;    /* network byte order */
    struct in_addr sin_addr;    /* network byte order */
    char           sin_zero[8];
};
#define AF_INET     2
#define SOCK_STREAM 1

#include <defines/bsdsocket.h>  /* socket/connect/send/recv/CloseSocket/Errno -> SocketBase */

struct Library *SocketBase;

/* Open bsdsocket.library. Returns 0 on success, -1 if it can't be opened. */
int aros_net_open(void)
{
    if (SocketBase)
        return 0;                /* idempotent */
    SocketBase = OpenLibrary("bsdsocket.library", 0);
    return SocketBase ? 0 : -1;
}

void aros_net_close(void)
{
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = (struct Library *)0;
    }
}

/* A blocking IPv4 TCP socket. Returns the descriptor, or -1 on error. */
int aros_tcp_socket(void)
{
    if (!SocketBase)
        return -1;
    return socket(AF_INET, SOCK_STREAM, 0);
}

/* Connect `s` to addr/port (both already in network byte order). Returns 0 / -1. */
int aros_connect_v4(int s, unsigned int addr_net, unsigned short port_net)
{
    struct sockaddr_in sa = {0};
    if (!SocketBase || s < 0)
        return -1;
    sa.sin_len         = sizeof sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = port_net;
    sa.sin_addr.s_addr = addr_net;
    return connect(s, (struct sockaddr *)&sa, sizeof sa);
}

/* Returns bytes sent (>=0) or -1. */
long aros_send(int s, const void *buf, unsigned long len)
{
    if (!SocketBase || s < 0 || !buf)
        return -1;
    return send(s, buf, len, 0);
}

/* Returns bytes received (>=0, 0 = peer closed) or -1. */
long aros_recv(int s, void *buf, unsigned long len)
{
    if (!SocketBase || s < 0 || !buf)
        return -1;
    return recv(s, buf, len, 0);
}

void aros_closesocket(int s)
{
    if (SocketBase && s >= 0)
        CloseSocket(s);
}

/* The bsdsocket per-task errno (NetBSD numbering). 0 if the library isn't open. */
int aros_sock_errno(void)
{
    return SocketBase ? Errno() : 0;
}
