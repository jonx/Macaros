/* bsdsock_resolve.c — async host DNS for bsdsocket.library's gethostbyname.
 *
 * getaddrinfo() can block for seconds, which would freeze AROS's single underlying
 * thread (H6). So the lookup runs on a detached host pthread and the AROS side
 * timer-polls hs_resolve_poll() (spec R-DARWIN-WAKE) — never blocking its own
 * thread, the same discipline as the socket park. Independent work: POSIX/Apple
 * man pages only; any resemblance is coincidental.
 */
#include "bsdsock_host.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

struct ResolveJob {
    _Atomic int state;      /* 0 pending, 1 ok, -1 fail */
    unsigned    ip;         /* resolved IPv4, NETWORK byte order */
    char        name[256];
    pthread_t   th;
};

static void *resolve_main(void *arg)
{
    struct ResolveJob *j = (struct ResolveJob *)arg;
    struct addrinfo hints, *res = NULL;
    int rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;       /* IPv4-first, matching the library */
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(j->name, NULL, &hints, &res);
    if (rc == 0 && res) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        j->ip = (unsigned)sa->sin_addr.s_addr;     /* already network order */
        atomic_store(&j->state, 1);
    } else {
        atomic_store(&j->state, -1);
    }
    if (res) freeaddrinfo(res);
    return NULL;
}

/* Start an async lookup. Returns an opaque job (poll it) or NULL on failure. */
ResolveJob *hs_resolve_start(const char *name)
{
    struct ResolveJob *j;
    if (!name) return NULL;
    j = (struct ResolveJob *)calloc(1, sizeof *j);
    if (!j) return NULL;
    strncpy(j->name, name, sizeof j->name - 1);
    atomic_store(&j->state, 0);
    if (pthread_create(&j->th, NULL, resolve_main, j) != 0) { free(j); return NULL; }
    pthread_detach(j->th);             /* fire-and-forget; AROS polls the result */
    return j;
}

/* Poll a job: 1 = done OK (*ip_net_out set), -1 = done failed, 0 = still pending. */
int hs_resolve_poll(ResolveJob *j, unsigned *ip_net_out)
{
    int s;
    if (!j) return -1;
    s = atomic_load(&j->state);
    if (s == 1 && ip_net_out) *ip_net_out = j->ip;
    return s;
}

/* Free a job — call only after hs_resolve_poll() returned non-zero (thread done). */
void hs_resolve_free(ResolveJob *j)
{
    free(j);
}
