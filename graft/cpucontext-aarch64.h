/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: CPU context definition for AArch64.

    Replacement for arch/aarch64-all/include/aros/cpucontext.h, whose current
    definition `{ IPTR r[29]; IPTR fp; IPTR sp; IPTR pc; }` is wrong: it mislabels
    x30 as fp, omits SPSR/CPSR entirely, and has no FP/NEON pointer. That breaks
    the hosted darwin backend, whose SAVEREGS/RESTOREREGS (cpu_aarch64.h)
    CopyMemQuick straight between this struct and Darwin's _STRUCT_ARM_THREAD_STATE64
    — so the layout must match: x[0..28], fp(x29), lr(x30), sp, pc, cpsr.

    This shape is also the AROS-canonical equivalent of the bare-metal Phase-1 trap
    frame in boot/kern.h (x[31] + elr + spsr): pc carries ELR_EL1 when trapped and
    cpsr carries SPSR_EL1.
*/

#ifndef AROS_AARCH64_CPUCONTEXT_H
#define AROS_AARCH64_CPUCONTEXT_H

#include <exec/types.h>

/* ECF_ flags */
#define ECF_FPU  0x0001   /* fpuContext is valid */

struct ExceptionContext
{
    UQUAD  x[29];        /* General purpose registers x0-x28                 */
    UQUAD  fp;           /* x29, frame pointer                               */
    UQUAD  lr;           /* x30, link register                               */
    UQUAD  sp;           /* x31, stack pointer (SP_EL0)                      */
    UQUAD  pc;           /* Program counter (ELR_EL1 when trapped)           */
    ULONG  cpsr;         /* Processor state (SPSR_EL1 when trapped)          */
    ULONG  Flags;        /* ECF_* — e.g. ECF_FPU when fpuContext is present  */
    APTR   fpuContext;   /* -> NEON/VFP state (host _STRUCT_ARM_NEON_STATE64)*/
};

/*
 * Up to here (x[29], fp, lr, sp, pc, cpsr) the layout is binary-compatible with
 * Darwin's _STRUCT_ARM_THREAD_STATE64, which is what lets the hosted backend copy
 * register state with a single CopyMemQuick. Keep the first six members in this
 * order if you touch this struct.
 */

#endif /* AROS_AARCH64_CPUCONTEXT_H */
