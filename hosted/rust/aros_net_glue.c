/* aros_net_glue.c -- flat-C boundary between Rust and the host-passthrough
 * bsdsocket.library. Rust can't call the inline LVO stubs (socket/connect/...
 * dispatch through SocketBase in a6), so this exposes plain extern "C" wrappers
 * that std's `sys/net/connection/aros.rs` and the RS4 probe drive over FFI.
 *
 * Two layers of names:
 *   - aros_net_open / aros_tcp_socket / aros_connect_v4 / aros_send / aros_recv /
 *     aros_closesocket / aros_sock_errno  -- the original RS4 round-trip probe.
 *   - aros_np_*  -- the fuller BSD surface std::net needs (bind/listen/accept/
 *     sendto/recvfrom/getsockname/getpeername/shutdown/setsockopt/getsockopt/
 *     nonblock/resolve). "np" = net pal.
 *
 * All IPv4 addresses/ports cross this boundary already in network byte order, as
 * a u32 (s_addr) + u16 (port); the glue builds/reads the sockaddr_in so Rust never
 * touches the BSD struct layout.
 *
 * Header-clean on purpose: the AmiTCP <sys/socket.h>/<netinet/in.h> pull the macOS
 * SDK on this backend (UPSTREAM-NOTES #9/#16/#34), so we declare the few socket
 * types ourselves (AmiTCP/NetBSD numbering, which AROS's bsdsocket.library and the
 * BSD-identical macOS host both use for IPv4 + the common options) and include only
 * <defines/bsdsocket.h> for the inline LVO stubs. Compiled with -ffixed-x18 so the
 * bridge can't park a value in the reserved platform register. Every wrapper guards
 * SocketBase so a misordered call returns an error instead of dereferencing NULL.
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
struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define FIONBIO     0x8004667eUL   /* _IOW('f', 126, int) -- BSD, host-identical */

#include <defines/bsdsocket.h>  /* socket/connect/.../CloseSocket/Errno -> SocketBase */

struct Library *SocketBase;

/* ---- library lifetime -------------------------------------------------------
 * std keeps bsdsocket.library open for the whole process (one ref); aros_net_open
 * is idempotent, and std never calls aros_net_close (the RS4 probe still does). */
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

/* The bsdsocket per-task errno (AmiTCP numbering, host->AmiTCP translated by the
 * library). 0 if the library isn't open. std maps this via from_raw_os_error. */
int aros_sock_errno(void)
{
    return SocketBase ? Errno() : 0;
}

void aros_closesocket(int s)
{
    if (SocketBase && s >= 0)
        CloseSocket(s);
}

/* ---- RS4 probe surface (kept stable) --------------------------------------- */
int aros_tcp_socket(void)
{
    if (!SocketBase)
        return -1;
    return socket(AF_INET, SOCK_STREAM, 0);
}

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

long aros_send(int s, const void *buf, unsigned long len)
{
    if (!SocketBase || s < 0 || !buf)
        return -1;
    return send(s, buf, len, 0);
}

long aros_recv(int s, void *buf, unsigned long len)
{
    if (!SocketBase || s < 0 || !buf)
        return -1;
    return recv(s, buf, len, 0);
}

/* ---- std::net surface (aros_np_*) ------------------------------------------ */

/* socket(domain,type,proto). Rust passes AF_INET / SOCK_STREAM|SOCK_DGRAM. */
int aros_np_socket(int domain, int type, int proto)
{
    if (!SocketBase)
        return -1;
    return socket(domain, type, proto);
}

int aros_np_connect(int s, unsigned int addr_net, unsigned short port_net)
{
    return aros_connect_v4(s, addr_net, port_net);
}

int aros_np_bind(int s, unsigned int addr_net, unsigned short port_net)
{
    struct sockaddr_in sa = {0};
    if (!SocketBase || s < 0)
        return -1;
    sa.sin_len         = sizeof sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = port_net;
    sa.sin_addr.s_addr = addr_net;
    return bind(s, (struct sockaddr *)&sa, sizeof sa);
}

int aros_np_listen(int s, int backlog)
{
    if (!SocketBase || s < 0)
        return -1;
    return listen(s, backlog);
}

/* accept(); on success returns the new descriptor and writes the peer addr/port
 * (network order). -1 on error. */
int aros_np_accept(int s, unsigned int *addr_net, unsigned short *port_net)
{
    struct sockaddr_in sa = {0};
    int len = sizeof sa;
    int ns;
    if (!SocketBase || s < 0)
        return -1;
    ns = accept(s, (struct sockaddr *)&sa, &len);
    if (ns >= 0) {
        if (addr_net) *addr_net = sa.sin_addr.s_addr;
        if (port_net) *port_net = sa.sin_port;
    }
    return ns;
}

/* The generated bsdsocket stubs pass the length as a 32-bit int (D1). Rust hands us
 * usize buffer lengths; a >= 2 GiB buffer would go negative or wrap at the LVO
 * boundary. std's contract is a SHORT read/write, so clamp instead of erroring. */
static unsigned long np_clamp_len(unsigned long len)
{
    return len > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : len;
}

long aros_np_send(int s, const void *buf, unsigned long len, int flags)
{
    if (!SocketBase || s < 0 || (!buf && len))
        return -1;
    return send(s, buf, np_clamp_len(len), flags);
}

long aros_np_recv(int s, void *buf, unsigned long len, int flags)
{
    if (!SocketBase || s < 0 || (!buf && len))
        return -1;
    return recv(s, buf, np_clamp_len(len), flags);
}

long aros_np_sendto(int s, const void *buf, unsigned long len, int flags,
                    unsigned int addr_net, unsigned short port_net)
{
    struct sockaddr_in sa = {0};
    if (!SocketBase || s < 0 || (!buf && len))
        return -1;
    sa.sin_len         = sizeof sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = port_net;
    sa.sin_addr.s_addr = addr_net;
    return sendto(s, buf, np_clamp_len(len), flags, (struct sockaddr *)&sa, sizeof sa);
}

long aros_np_recvfrom(int s, void *buf, unsigned long len, int flags,
                      unsigned int *addr_net, unsigned short *port_net)
{
    struct sockaddr_in sa = {0};
    int slen = sizeof sa;
    long n;
    if (!SocketBase || s < 0 || (!buf && len))
        return -1;
    n = recvfrom(s, buf, np_clamp_len(len), flags, (struct sockaddr *)&sa, &slen);
    if (n >= 0) {
        if (addr_net) *addr_net = sa.sin_addr.s_addr;
        if (port_net) *port_net = sa.sin_port;
    }
    return n;
}

int aros_np_getsockname(int s, unsigned int *addr_net, unsigned short *port_net)
{
    struct sockaddr_in sa = {0};
    int len = sizeof sa;
    if (!SocketBase || s < 0)
        return -1;
    if (getsockname(s, (struct sockaddr *)&sa, &len) < 0)
        return -1;
    if (addr_net) *addr_net = sa.sin_addr.s_addr;
    if (port_net) *port_net = sa.sin_port;
    return 0;
}

int aros_np_getpeername(int s, unsigned int *addr_net, unsigned short *port_net)
{
    struct sockaddr_in sa = {0};
    int len = sizeof sa;
    if (!SocketBase || s < 0)
        return -1;
    if (getpeername(s, (struct sockaddr *)&sa, &len) < 0)
        return -1;
    if (addr_net) *addr_net = sa.sin_addr.s_addr;
    if (port_net) *port_net = sa.sin_port;
    return 0;
}

int aros_np_shutdown(int s, int how)
{
    if (!SocketBase || s < 0)
        return -1;
    return shutdown(s, how);
}

int aros_np_setsockopt(int s, int level, int name, const void *val, unsigned int len)
{
    if (!SocketBase || s < 0)
        return -1;
    return setsockopt(s, level, name, (void *)val, (int)len);
}

int aros_np_getsockopt(int s, int level, int name, void *val, unsigned int *len)
{
    if (!SocketBase || s < 0)
        return -1;
    return getsockopt(s, level, name, val, len);
}

/* FIONBIO. NB: the AROS bsdsocket.library keeps host sockets O_NONBLOCK and
 * emulates blocking with a timer-poll park, so it treats FIONBIO as a no-op
 * success -- non-blocking mode is not yet effective (UPSTREAM-NOTES). We still
 * issue the ioctl so behaviour tracks the library instead of lying. */
int aros_np_set_nonblock(int s, int nonblock)
{
    int arg = nonblock ? 1 : 0;
    if (!SocketBase || s < 0)
        return -1;
    return IoctlSocket(s, FIONBIO, (char *)&arg);
}

/* Dup2Socket(fd, -1) allocates a fresh descriptor referring to the same underlying
 * socket (BSD dup semantics), for TcpStream/TcpListener/UdpSocket::try_clone. The
 * library park model does NOT interfere here: both descriptors share one host socket.
 * Returns the new fd, or -1. */
int aros_np_dup(int s)
{
    if (!SocketBase || s < 0)
        return -1;
    return Dup2Socket(s, -1);
}

/* Resolve an IPv4 host name to up to `max` addresses (network order) via
 * gethostbyname. Returns the count written, or -1 on failure (h_errno-ish via
 * Errno). Rust only calls this for non-literal hosts. */
int aros_np_resolve4(const char *name, unsigned int *out_addrs, int max)
{
    struct hostent *he;
    int n = 0;
    if (!SocketBase || !name || !out_addrs || max <= 0)
        return -1;
    he = gethostbyname((char *)name);
    if (!he || he->h_addrtype != AF_INET || he->h_length != 4 || !he->h_addr_list)
        return -1;
    while (n < max && he->h_addr_list[n]) {
        unsigned char *b = (unsigned char *)he->h_addr_list[n];
        unsigned char *o = (unsigned char *)&out_addrs[n];
        /* h_addr is already network order; copy the 4 bytes straight into s_addr's
         * memory (endian-independent -- matches how aros_np_connect reads it). */
        o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = b[3];
        n++;
    }
    return n;
}
