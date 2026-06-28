/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: [N5b] live round-trip test for the host-passthrough bsdsocket.library.
          OpenLibrary -> socket -> connect 127.0.0.1:12345 -> send -> recv ->
          compare. Run from the AROS shell with a localhost echo server already
          listening on the host. Prints one [N5B] PASS/FAIL line.

          Uses the inline LVO stubs from <proto/bsdsocket.h> (defines/bsdsocket.h)
          dispatched through SocketBase — no linklib. Address bytes are written in
          network order directly (127.0.0.1:12345) to avoid an htons/htonl dep.
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>

/* Inline LVO stubs (socket/connect/send/recv/CloseSocket/Errno -> AROS_LC*,
   dispatched through SocketBase) WITHOUT clib/bsdsocket_protos.h, which pulls
   sys/types.h+sys/select.h that aren't installed in this -quick build. The
   defines header is self-contained (its pthreadsocket include is PTHREAD_H-gated). */
#include <aros/libcall.h>
#include <defines/bsdsocket.h>

struct Library *SocketBase;

#define PORT_NET   0x3930        /* htons(12345) on a little-endian host  */
#define ADDR_NET   0x0100007f    /* 127.0.0.1 in network order on LE host */

int main(void)
{
    struct sockaddr_in sa;
    char buf[16];
    int s, rc, n;

    SocketBase = OpenLibrary("bsdsocket.library", 0);
    if (!SocketBase)
    {
        Printf("[N5B] FAIL: cannot open bsdsocket.library\n");
        return 20;
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
    {
        Printf("[N5B] FAIL: socket() = %ld, errno %ld\n", (LONG)s, (LONG)Errno());
        CloseLibrary(SocketBase);
        return 20;
    }

    memset(&sa, 0, sizeof sa);
    sa.sin_len         = sizeof sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = PORT_NET;
    sa.sin_addr.s_addr = ADDR_NET;

    rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0)
    {
        Printf("[N5B] FAIL: connect() = %ld, errno %ld\n", (LONG)rc, (LONG)Errno());
        CloseSocket(s);
        CloseLibrary(SocketBase);
        return 20;
    }

    rc = send(s, "PING42", 6, 0);
    if (rc != 6)
    {
        Printf("[N5B] FAIL: send() = %ld, errno %ld\n", (LONG)rc, (LONG)Errno());
        CloseSocket(s);
        CloseLibrary(SocketBase);
        return 20;
    }

    memset(buf, 0, sizeof buf);
    n = recv(s, buf, sizeof buf - 1, 0);
    if (n < 0)
    {
        Printf("[N5B] FAIL: recv() = %ld, errno %ld\n", (LONG)n, (LONG)Errno());
        CloseSocket(s);
        CloseLibrary(SocketBase);
        return 20;
    }

    if (n == 6 && memcmp(buf, "PING42", 6) == 0)
        Printf("[N5B] PASS: round-trip echoed '%s' (%ld bytes)\n", buf, (LONG)n);
    else
        Printf("[N5B] FAIL: expected 'PING42', got %ld bytes '%s'\n", (LONG)n, buf);

    CloseSocket(s);
    CloseLibrary(SocketBase);
    return 0;
}
