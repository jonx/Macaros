/* errno_xlate.h — host (Darwin) BSD errno -> AmiTCP (AROS) errno mapping.
 *
 * The raw int a libSystem socket call returns through the H3 shim is a Darwin
 * errno; AROS apps expect the AmiTCP values in
 * workbench/network/common/include/sys/errno.h. Translate, never blind-passthrough
 * (spec "errno translation"). Pure int->int so this compiles identically in the
 * host unit test (host clang) and the AROS crosstools build — neither side's
 * errno.h is assumed in scope.
 *
 * On Darwin the BSD socket range (35..65) is identity, but there is at least one
 * genuine NON-identity case — macOS EOPNOTSUPP==102 vs AmiTCP EOPNOTSUPP==45 — so
 * the table is built explicitly, entry by entry (verified against the macOS SDK
 * <sys/errno.h> and AROS sys/errno.h). The Linux port swaps only this file (Linux
 * errno numbering differs widely from BSD).
 *
 * Independent work: no third-party implementation source was read or consulted;
 * built from the two cited headers + POSIX only.
 */
#ifndef BSDSOCK_ERRNO_XLATE_H
#define BSDSOCK_ERRNO_XLATE_H

int bsdsock_errno_h2a(int host_errno);

#endif /* BSDSOCK_ERRNO_XLATE_H */
