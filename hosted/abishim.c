// Hosted AArch64 AROS — Phase 2 H3: host-call ABI shim probe.
//
// Drives the hand-written marshaller in abishim.S to prove an AROS-side call can
// be bridged into Apple's arm64 variadic ABI from raw code — and proves the naive
// register-passing path is genuinely broken, so the boundary really had to be
// grounded. The "AROS side" is modelled as a generic descriptor: a fixed prefix
// (buf,size,fmt) plus a runtime array of 64-bit argument words — the shape an
// AROS host-printf bridge actually marshals.

#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern int aros_call_snprintf(char *buf, unsigned long size, const char *fmt,
                              long argc, const unsigned long *argv);
extern int aros_call_snprintf_aapcs(char *buf, unsigned long size, const char *fmt,
                                     long argc, const unsigned long *argv);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H3a] cross-ABI shim: marshalling AROS-side args into Apple's arm64 variadic ABI\n");

    char buf[128];
    int ok = 1;

    // 1) Three ints — the classic printf case that exposes the divergence.
    unsigned long ints[] = { 11, 22, 33 };
    aros_call_snprintf(buf, sizeof buf, "%d %d %d", 3, ints);
    printf("[H3]   ints      -> \"%s\"  (want \"11 22 33\")\n", buf);
    ok &= !strcmp(buf, "11 22 33");

    // 2) A double carried through the SAME 64-bit arg array as its bit pattern,
    //    proving Apple parks variadic FP args in the integer stack slots too.
    double d = 3.5;
    unsigned long dbits;
    memcpy(&dbits, &d, sizeof dbits);
    unsigned long mixed[] = { 7, dbits, (unsigned long)'Z' };
    aros_call_snprintf(buf, sizeof buf, "%d %.1f %c", 3, mixed);
    printf("[H3]   int/dbl/ch -> \"%s\"  (want \"7 3.5 Z\")\n", buf);
    ok &= !strcmp(buf, "7 3.5 Z");

    // 3) A pointer arg (%s) — host strings flow through unchanged.
    const char *name = "AROS";
    unsigned long strs[] = { (unsigned long)name };
    aros_call_snprintf(buf, sizeof buf, "<%s>", 1, strs);
    printf("[H3]   string     -> \"%s\"  (want \"<AROS>\")\n", buf);
    ok &= !strcmp(buf, "<AROS>");

    // 4) Negative control: the naive generic-AAPCS64 path (varargs in registers).
    //    Apple's snprintf reads the stack, not x3..x5 -> reliably WRONG. This is
    //    the exact failure mode the grounding was meant to catch.
    char wrong[128];
    aros_call_snprintf_aapcs(wrong, sizeof wrong, "%d %d %d", 3, ints);
    printf("[H3]   naive-regs -> \"%s\"  (Apple ignores x3..x5 -> not \"11 22 33\")\n", wrong);
    ok &= (strcmp(wrong, "11 22 33") != 0);

    if (ok)
        printf("[H3] host-call ABI shim ok: AROS->Apple variadic boundary bridged (naive path proven broken)\n");
    else
        printf("[H3] FAIL: shim output did not match\n");
    return ok ? 0 : 1;
}
