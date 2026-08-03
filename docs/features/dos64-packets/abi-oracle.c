/*
 * abi-oracle.c - report the on-the-wire layout of struct DosPacket64.
 *
 * Build and run this on a machine with the official AmigaOS 4 SDK installed.
 * It prints the field order, sizes, offsets, alignment and total size that AROS
 * needs in order to pin its own 32-bit DosPacket64 with compile-time assertions.
 *
 * Only the printed numbers are needed. The SDK header itself is not required,
 * and should not be copied into this repository.
 *
 *     gcc -o abi-oracle abi-oracle.c && ./abi-oracle
 *
 * Paste the output into docs/features/dos64-packets/README.md.
 */

#include <stdio.h>
#include <stddef.h>

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#define P(s, f)   printf("  %-10s offset %3zu  size %2zu\n", #f, \
                         offsetof(struct s, f), sizeof(((struct s *)0)->f))

int main(void)
{
    printf("target pointer size : %zu\n", sizeof(void *));
    printf("sizeof(struct Message)      : %zu\n", sizeof(struct Message));
    printf("sizeof(struct DosPacket)    : %zu\n", sizeof(struct DosPacket));
    printf("sizeof(struct DosPacket64)  : %zu\n", sizeof(struct DosPacket64));
    printf("_Alignof(struct DosPacket64): %zu\n", _Alignof(struct DosPacket64));

    printf("\nstruct DosPacket (baseline, for the common prefix):\n");
    P(DosPacket, dp_Link);
    P(DosPacket, dp_Port);
    P(DosPacket, dp_Type);
    P(DosPacket, dp_Res1);
    P(DosPacket, dp_Res2);
    P(DosPacket, dp_Arg1);
    P(DosPacket, dp_Arg2);
    P(DosPacket, dp_Arg3);
    P(DosPacket, dp_Arg4);
    P(DosPacket, dp_Arg5);

    printf("\nstruct DosPacket64:\n");
    P(DosPacket64, dp_Link);
    P(DosPacket64, dp_Port);
    P(DosPacket64, dp_Type);
    /* Remove any line below that does not compile, and say which. That the
     * field is absent is itself part of the answer. */
    P(DosPacket64, dp_Res0);
    P(DosPacket64, dp_Res1);
    P(DosPacket64, dp_Res2);
    P(DosPacket64, dp_Arg1);
    P(DosPacket64, dp_Arg2);
    P(DosPacket64, dp_Arg3);
    P(DosPacket64, dp_Arg4);
    P(DosPacket64, dp_Arg5);

#ifdef DP64_INIT
    printf("\nDP64_INIT = %d\n", (int)DP64_INIT);
#else
    printf("\nDP64_INIT: not defined by the SDK headers\n");
#endif

    /* StandardPacket64 may not exist. If this block does not compile, delete it
     * and report that, which is also an answer we need. */
    printf("\nsizeof(struct StandardPacket64) : %zu\n",
           sizeof(struct StandardPacket64));
    printf("offsetof(StandardPacket64, sp_Pkt): %zu\n",
           offsetof(struct StandardPacket64, sp_Pkt));

    return 0;
}
