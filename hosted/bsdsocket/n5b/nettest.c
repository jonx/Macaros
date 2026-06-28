/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Live exercise of two more bsdsocket.library paths on hosted AROS:
            [WS] WaitSelect (LVO 21) over the localhost echo server, and
            [N6] an outbound HTTP/1.0 GET to 1.1.1.1:80 (raw IP, no DNS).
          Run from the AROS shell (the echo server must be listening on
          127.0.0.1:12345 for [WS]; [N6] needs host outbound internet).

          Inline LVO stubs via defines/bsdsocket.h (no clib protos -> no sys/types.h);
          a local AmiTCP-compatible fd_set/timeval mirror avoids net_types.h.
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>

#include <aros/libcall.h>
typedef struct fd_set fd_set;            /* opaque, for the WaitSelect macro */
#include <defines/bsdsocket.h>

struct Library *SocketBase;

/* AmiTCP fd_set / timeval are binary { long }: FD_SETSIZE=64 -> one 64-bit word. */
struct fds { long w; };
#define FZERO(p)    ((p)->w = 0)
#define FSET(n, p)  ((p)->w |= (1L << (n)))
#define FISSET(n,p) ((p)->w &  (1L << (n)))
struct tvl { long sec, usec; };

static int connect_to(unsigned addr_net, unsigned short port_net)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_len         = sizeof sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = port_net;
    sa.sin_addr.s_addr = addr_net;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { CloseSocket(s); return -1; }
    return s;
}

int main(void)
{
    SocketBase = OpenLibrary("bsdsocket.library", 0);
    if (!SocketBase) { Printf("[NET] FAIL: cannot open bsdsocket.library\n"); return 20; }

    /* ---- [WS] WaitSelect over the localhost echo server ------------------ */
    {
        int s = connect_to(0x0100007f, 0x3930);   /* 127.0.0.1:12345 */
        if (s < 0)
            Printf("[WS] FAIL: connect echo, errno %ld\n", (LONG)Errno());
        else
        {
            struct fds r; struct tvl t; int rc;
            send(s, "Z", 1, 0);                    /* server echoes -> read-ready */
            FZERO(&r); FSET(s, &r);
            t.sec = 3; t.usec = 0;
            rc = WaitSelect(s + 1, (fd_set *)&r, (fd_set *)0, (fd_set *)0, (struct timeval *)&t, (ULONG *)0);
            if (rc >= 1 && FISSET(s, &r))
            {
                char b[4]; int n = recv(s, b, sizeof b, 0);
                if (n == 1 && b[0] == 'Z')
                    Printf("[WS] PASS: WaitSelect woke on read-ready, recv echoed the byte\n");
                else
                    Printf("[WS] FAIL: recv after WaitSelect n=%ld\n", (LONG)n);
            }
            else
                Printf("[WS] FAIL: WaitSelect rc=%ld, errno %ld\n", (LONG)rc, (LONG)Errno());
            CloseSocket(s);
        }
    }

    /* ---- [N6] outbound HTTP/1.0 GET to 1.1.1.1:80 (raw IP) --------------- */
    {
        int s = connect_to(0x01010101, 0x5000);   /* 1.1.1.1:80 (htons(80)) */
        if (s < 0)
            Printf("[N6] FAIL: connect 1.1.1.1:80, errno %ld\n", (LONG)Errno());
        else
        {
            const char *req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\n\r\n";
            char b[80]; int n, sent, i;
            sent = send(s, req, strlen(req), 0);
            memset(b, 0, sizeof b);
            n = recv(s, b, sizeof b - 1, 0);
            if (n > 4 && strncmp(b, "HTTP/1", 6) == 0)
            {
                for (i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') { b[i] = 0; break; }
                Printf("[N6] PASS: AROS fetched over the internet from 1.1.1.1 -> '%s'\n", b);
            }
            else
                Printf("[N6] FAIL: sent=%ld recv=%ld errno %ld first='%s'\n",
                       (LONG)sent, (LONG)n, (LONG)Errno(), b);
            CloseSocket(s);
        }
    }

    /* ---- [DNS] gethostbyname -> connect -> fetch (real DNS) -------------- */
    {
        struct hostent *he = gethostbyname("one.one.one.one");   /* -> 1.1.1.1 */
        if (!he || !he->h_addr_list || !he->h_addr_list[0])
            Printf("[DNS] FAIL: gethostbyname, errno %ld\n", (LONG)Errno());
        else
        {
            unsigned ip; int s;
            memcpy(&ip, he->h_addr_list[0], 4);                  /* network order */
            s = connect_to(ip, 0x5000);                          /* :80 */
            if (s < 0)
                Printf("[DNS] FAIL: connect resolved host, errno %ld\n", (LONG)Errno());
            else
            {
                const char *req = "GET / HTTP/1.0\r\nHost: one.one.one.one\r\n\r\n";
                char b[80]; int n, i;
                send(s, req, strlen(req), 0);
                memset(b, 0, sizeof b);
                n = recv(s, b, sizeof b - 1, 0);
                if (n > 4 && strncmp(b, "HTTP/1", 6) == 0)
                {
                    for (i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') { b[i] = 0; break; }
                    Printf("[DNS] PASS: resolved one.one.one.one -> %ld.%ld.%ld.%ld, fetched '%s'\n",
                           (LONG)(ip & 0xff), (LONG)((ip >> 8) & 0xff),
                           (LONG)((ip >> 16) & 0xff), (LONG)((ip >> 24) & 0xff), b);
                }
                else
                    Printf("[DNS] FAIL: fetch n=%ld, errno %ld\n", (LONG)n, (LONG)Errno());
                CloseSocket(s);
            }
        }
    }

    CloseLibrary(SocketBase);
    return 0;
}
