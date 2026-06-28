/* errno_xlate.c — Darwin BSD errno -> AmiTCP errno (see errno_xlate.h).
 *
 * Values are LITERALS, not the E* symbols, so the file is identical in the host
 * test and the AROS build (the input is a raw Darwin number; the output is an
 * AROS sys/errno.h number). Each row is verified:
 *   left  = macOS SDK <sys/errno.h>  (the host value libSystem returns)
 *   right = AROS workbench/network/common/include/sys/errno.h  (what apps expect)
 * The BSD socket range is identity EXCEPT EOPNOTSUPP (the one renumber).
 */
#include "errno_xlate.h"

int bsdsock_errno_h2a(int e)
{
    switch (e) {
    case 0:   return 0;
    case 4:   return 4;    /* EINTR        */
    case 9:   return 9;    /* EBADF        */
    case 13:  return 13;   /* EACCES       */
    case 14:  return 14;   /* EFAULT       */
    case 22:  return 22;   /* EINVAL       */
    case 24:  return 24;   /* EMFILE       */
    case 32:  return 32;   /* EPIPE        */
    case 35:  return 35;   /* EAGAIN / EWOULDBLOCK */
    case 36:  return 36;   /* EINPROGRESS  */
    case 37:  return 37;   /* EALREADY     */
    case 38:  return 38;   /* ENOTSOCK     */
    case 39:  return 39;   /* EDESTADDRREQ */
    case 40:  return 40;   /* EMSGSIZE     */
    case 41:  return 41;   /* EPROTOTYPE   */
    case 42:  return 42;   /* ENOPROTOOPT  */
    case 43:  return 43;   /* EPROTONOSUPPORT */
    case 45:  return 45;   /* ENOTSUP      -> AmiTCP EOPNOTSUPP (45) */
    case 47:  return 47;   /* EAFNOSUPPORT */
    case 48:  return 48;   /* EADDRINUSE   */
    case 49:  return 49;   /* EADDRNOTAVAIL */
    case 50:  return 50;   /* ENETDOWN     */
    case 51:  return 51;   /* ENETUNREACH  */
    case 53:  return 53;   /* ECONNABORTED */
    case 54:  return 54;   /* ECONNRESET   */
    case 56:  return 56;   /* EISCONN      */
    case 57:  return 57;   /* ENOTCONN     */
    case 60:  return 60;   /* ETIMEDOUT    */
    case 61:  return 61;   /* ECONNREFUSED */
    case 64:  return 64;   /* EHOSTDOWN    */
    case 65:  return 65;   /* EHOSTUNREACH */

    /* NON-IDENTITY: macOS renumbered EOPNOTSUPP out of the BSD range. A socket op
     * that returns 102 on Darwin must surface as AmiTCP EOPNOTSUPP (45). */
    case 102: return 45;   /* EOPNOTSUPP (macOS 102) -> AmiTCP EOPNOTSUPP (45) */

    default:
        /* Darwin-only: the BSD range 1..65 above is identity with AmiTCP, so an
         * unmapped value in that range is already correct; values >100 are macOS
         * POSIX-extension territory and EOPNOTSUPP (handled above) is the only one
         * a socket op realistically returns. Pass through. The LINUX port MUST
         * replace this default with explicit mappings + an EINVAL fallback. */
        return e;
    }
}
