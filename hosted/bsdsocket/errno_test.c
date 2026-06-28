/* errno_test.c — host unit test for the Darwin->AmiTCP errno table ([NERR]).
 *
 * Feeds the actual macOS <errno.h> symbol values (the host numbers libSystem
 * returns) through bsdsock_errno_h2a() and asserts each lands on the AmiTCP value
 * AROS apps expect. The load-bearing assertion is the NON-identity EOPNOTSUPP
 * (macOS 102 -> AROS 45); the rest of the socket range is identity but checked
 * explicitly so a table typo can't slip through. Hermetic, no sockets.
 * Independent work: built from the two errno.h headers + POSIX only.
 */
#include <errno.h>
#include <stdio.h>
#include "errno_xlate.h"

static int pass, total;

#define CK(host_sym, aros_val) do {                                            \
    int h = (int)(host_sym), got = bsdsock_errno_h2a(h);                       \
    total++;                                                                   \
    if (got == (aros_val)) {                                                   \
        pass++;                                                                \
        printf("[NERR] PASS %-14s host=%-3d -> aros=%d\n", #host_sym, h, got); \
    } else                                                                     \
        printf("[NERR] FAIL %-14s host=%-3d -> got %d, want %d\n",             \
               #host_sym, h, got, (aros_val));                                 \
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Identity across the BSD socket range (host value == AmiTCP value). */
    CK(EWOULDBLOCK,   35);
    CK(EAGAIN,        35);
    CK(EINPROGRESS,   36);
    CK(EALREADY,      37);
    CK(ENOTSOCK,      38);
    CK(EDESTADDRREQ,  39);
    CK(EMSGSIZE,      40);
    CK(EAFNOSUPPORT,  47);
    CK(EADDRINUSE,    48);
    CK(EADDRNOTAVAIL, 49);
    CK(ENETUNREACH,   51);
    CK(ECONNABORTED,  53);
    CK(ECONNRESET,    54);
    CK(EISCONN,       56);
    CK(ENOTCONN,      57);
    CK(ETIMEDOUT,     60);
    CK(ECONNREFUSED,  61);
    CK(EHOSTUNREACH,  65);
    CK(EBADF,          9);
    CK(EINVAL,        22);
    CK(EINTR,          4);
    CK(EPIPE,         32);

    /* The grounding payoff: macOS renumbered these out of the BSD range, so they
     * are NOT identity — they must fold onto AmiTCP EOPNOTSUPP (45). */
    CK(EOPNOTSUPP,    45);   /* macOS 102 -> 45 */
    CK(ENOTSUP,       45);   /* macOS  45 -> 45 */

    /* Darwin default: an unmapped value passes through unchanged. */
    CK(424242,    424242);

    int ok = (pass == total);
    printf("[NERR] %s %d/%d  (Darwin BSD errno -> AmiTCP errno table)\n",
           ok ? "PASS" : "FAIL", pass, total);
    return ok ? 0 : 1;
}
