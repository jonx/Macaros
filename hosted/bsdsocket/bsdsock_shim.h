/* bsdsock_shim.h — non-blocking host socket op wrappers (the bsdsocket ABI seam).
 *
 * Implemented clean-room from docs/features/bsdsocket-net/spec.md (R-NONBLOCK,
 * R-PARK, LVO surface). No GPL emulator source (WinUAE/FS-UAE/Amiberry/E-UAE/
 * Janus-UAE/vAmiga) was read, searched, or consulted. POSIX/Apple man pages
 * [PUB] only.
 *
 * Signatures mirror the spec's socket/connect/send/recv/close ABI rows. The
 * trailing PumpSig* is the wake target — the readiness-signal seam the AROS
 * graft replaces with the calling task's SocketBase/readySig. errno follows the
 * POSIX convention; the AROS side translates host errno -> AmiTCP errno per the
 * spec's value table (out of scope for this standalone proof, documented as a
 * deviation in the return notes).
 */
#ifndef BSDSOCK_SHIM_H
#define BSDSOCK_SHIM_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PumpSig PumpSig;

int  hs_set_nonblock(int fd);
int  hs_socket(int domain, int type, int protocol);
int  hs_close(int fd, PumpSig *sig);
int  hs_connect(int fd, const struct sockaddr *name, unsigned namelen, PumpSig *sig);

/* hs_send — ONE logical non-blocking BSD send (FINDING 3). Returns the actual,
 * POSSIBLY PARTIAL, byte count (>=1 on success, 0 if the peer closed, -1+errno
 * on error). It does NOT loop to completion: bsdsocket.library's send() LVO must
 * preserve BSD single-call/partial-return semantics, which AROS apps depend on.
 * If the FIRST send would block it parks once on write-readiness and retries the
 * single send; once any bytes are accepted it returns that count immediately. */
long hs_send(int fd, const void *buf, unsigned len, int flags, PumpSig *sig);

/* hs_send_all — write-all convenience built on hs_send, parking between partial
 * sends until the WHOLE buffer is delivered (FINDING 3). Returns `len` on full
 * delivery, 0 if the peer closed mid-write, -1+errno on error. For tests/callers
 * that genuinely want full delivery — NOT for the send() LVO. */
long hs_send_all(int fd, const void *buf, unsigned len, int flags, PumpSig *sig);

long hs_recv(int fd, void *buf, unsigned len, int flags, PumpSig *sig);
long hs_recv_nonblock(int fd, void *buf, unsigned len, int flags);

#ifdef __cplusplus
}
#endif

#endif /* BSDSOCK_SHIM_H */
