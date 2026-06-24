/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Darwin (macOS) hosted host-kernel CPU glue for AArch64 — the AArch64 peer of
    cpu_arm.h / cpu_x86_64.h. Maps a macOS arm64 signal context (ucontext) to/from
    AROS's struct ExceptionContext, so the hosted "interrupt" (a Unix signal) can
    save and resume AROS tasks. The macOS field layout used here is verified by the
    Phase-2 H2/H4 spikes (hosted/preempt.c, hosted/exec.c) and grounded against the
    SDK: <mach/arm/_structs.h> (_STRUCT_ARM_THREAD_STATE64) and <arm/_mcontext.h>
    (_STRUCT_MCONTEXT64 = { __es, __ss, __ns }).
*/

#include <exec/types.h>
#include <aros/aarch64/cpucontext.h>

#ifdef __AROS_EXEC_LIBRARY__

/* regs_t is a black box here */
struct ucontext;
typedef struct ucontext *regs_t;

#else

#include <sys/ucontext.h>

#define SIGCORE_NEED_SA_SIGINFO

typedef ucontext_t regs_t;

#define SIGHANDLER      bsd_sighandler
typedef void (*SIGHANDLER_T)(int);

#define SC_DISABLE(sc)   sc->uc_sigmask = KernelBase->kb_PlatformData->sig_int_mask
#define SC_ENABLE(sc)                           \
do {                                            \
    pd->iface->SigEmptySet(&(sc)->uc_sigmask);  \
    AROS_HOST_BARRIER                           \
} while(0)

/*
 * For -arch arm64, __DARWIN_OPAQUE_ARM_THREAD_STATE64 == 0, so the thread state
 * exposes the plain register fields (no pointer-auth opaque packing). Verified in
 * the H2 spike. __ss = general state, __ns = NEON/FP state.
 */
#define Xn(context, n)  ((context)->uc_mcontext->__ss.__x[(n)])
#define FP(context)     ((context)->uc_mcontext->__ss.__fp)   /* x29 */
#define LR(context)     ((context)->uc_mcontext->__ss.__lr)   /* x30 */
#define SP(context)     ((context)->uc_mcontext->__ss.__sp)   /* x31 */
#define PC(context)     ((context)->uc_mcontext->__ss.__pc)
#define CPSR(context)   ((context)->uc_mcontext->__ss.__cpsr)

#define GPSTATE(context) ((context)->uc_mcontext->__ss)
#define FPSTATE(context) ((context)->uc_mcontext->__ns)

#define GLOBAL_SIGNAL_INIT(sighandler) \
    static void sighandler ## _gate (int sig, siginfo_t *info, void *sc) \
    {                                                                    \
        sighandler(sig, sc);                                             \
    }

/*
 * SAVEREGS / RESTOREREGS rely on struct ExceptionContext having the same layout
 * as Darwin's _STRUCT_ARM_THREAD_STATE64 (x[29], fp, lr, sp, pc, cpsr) — which is
 * exactly what graft/cpucontext-aarch64.h establishes. The H4 spike does the same
 * copy via *uc->uc_mcontext.
 */
#define SAVEREGS(cc, sc)                                                          \
    CopyMemQuick(&GPSTATE(sc), (cc)->regs.x, sizeof(_STRUCT_ARM_THREAD_STATE64));  \
    if ((cc)->regs.fpuContext)                                                    \
    {                                                                            \
        (cc)->regs.Flags |= ECF_FPU;                                             \
        CopyMemQuick(&FPSTATE(sc), (cc)->regs.fpuContext, sizeof(_STRUCT_ARM_NEON_STATE64)); \
    }

#define RESTOREREGS(cc, sc)                                                       \
    CopyMemQuick((cc)->regs.x, &GPSTATE(sc), sizeof(_STRUCT_ARM_THREAD_STATE64));  \
    if ((cc)->regs.Flags & ECF_FPU)                                              \
        CopyMemQuick((cc)->regs.fpuContext, &FPSTATE(sc), sizeof(_STRUCT_ARM_NEON_STATE64));

/* Print signal context. Used in the crash handler. */
#define PRINT_SC(sc) \
    bug ("    X0 =%016llX  X1 =%016llX  X2 =%016llX  X3 =%016llX\n" \
         "    FP =%016llX  LR =%016llX  SP =%016llX  PC =%016llX\n" \
         "    CPSR=%08X\n"                                          \
            , (unsigned long long)Xn(sc,0), (unsigned long long)Xn(sc,1) \
            , (unsigned long long)Xn(sc,2), (unsigned long long)Xn(sc,3) \
            , (unsigned long long)FP(sc),  (unsigned long long)LR(sc)    \
            , (unsigned long long)SP(sc),  (unsigned long long)PC(sc)    \
            , (unsigned int)CPSR(sc)                                     \
        )

#endif /* __AROS_EXEC_LIBRARY__ */

/* We emulate the AArch64 synchronous/IRQ/FIQ/SError exceptions (not softint). */
#define EXCEPTIONS_COUNT 6

struct AROSCPUContext
{
    struct ExceptionContext regs;
    int errno_backup;
};

/* Darwin arm64 has NEON/VFP. */
#define AARCH64_FPU_TYPE FPU_VFP
#define AARCH64_FPU_SIZE sizeof(_STRUCT_ARM_NEON_STATE64)
