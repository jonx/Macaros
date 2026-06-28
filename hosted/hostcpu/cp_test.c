/* cp_test.c — [CP1] host CPU facts are plausible + [CP2] load delta/16.16 math.
 *
 * Pure host probe (no AROS), links hostcpu_shim.c directly. Mirrors the
 * unattended-verify discipline of the audio [A1] / SysMon [SM1] spikes: assert
 * VALUES, not "it didn't crash". Independent work; resemblance coincidental.
 *
 *   [CP1] static identity/topology/misc/features are self-consistent and plausible:
 *         non-empty brand; ncpu/logical/physical > 0; P+E == physical (when both
 *         reported); byteorder == 1234 (LE); cacheline > 0; hw.optional.neon == 1.
 *   [CP2] the cumulative-ticks → fraction → 16.16 conversion the resource will do:
 *         load f in [0,1]; SysMon's decode ((v>>16)*1000)>>16 lands in 0..1000;
 *         first-sample-zero and zero-elapsed guards behave.
 *
 * Prints "[CP1] PASS"/"[CP2] PASS" and a combined "[CP] PASS"; exits 0 on success.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hostcpu_shim.h"

/* The 16.16 conversion the darwin processor.resource backend will apply for
 * GCIT_ProcessorLoad (R-LOAD): pack fraction f in [0,1] so SysMon's decode
 * ((v>>16)*1000)>>16 yields tenths-of-a-percent in 0..1000. Uses the
 * (ULONG)(f*0xffffffff) form to avoid the f==1 overflow of (f*65536)<<16. */
static uint32_t pack_load_16_16(double f)
{
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return (uint32_t)(f * 4294967295.0);
}
static int sysmon_decode_tenths(uint32_t v)   /* processor_gauge.c:144 shape */
{
    return (int)(((v >> 16) * 1000u) >> 16);
}

int main(void)
{
    int ok = 1;

    HCContext *c = hc_open();
    if (!c) { printf("[CP] FAIL hc_open returned NULL\n"); return 1; }

    /* ---- [CP1] static facts ---------------------------------------------- */
    int cp1 = 1;

    char brand[160] = {0};
    int br = hc_brand(c, brand, sizeof brand);
    if (br != 0 || brand[0] == '\0') { printf("[CP1] sub-FAIL brand absent/empty\n"); cp1 = 0; }

    HCTopology topo; unsigned tmask = 0;
    hc_topology(c, &topo, &tmask);
    if (!(tmask & HC_TOPO_NCPU) || topo.ncpu <= 0)         { printf("[CP1] sub-FAIL ncpu\n"); cp1 = 0; }
    if (!(tmask & HC_TOPO_LOGICAL) || topo.logical <= 0)   { printf("[CP1] sub-FAIL logicalcpu\n"); cp1 = 0; }
    if (!(tmask & HC_TOPO_PHYSICAL) || topo.physical <= 0) { printf("[CP1] sub-FAIL physicalcpu\n"); cp1 = 0; }
    if (topo.logical < topo.physical)                      { printf("[CP1] sub-FAIL logical<physical\n"); cp1 = 0; }
    /* P+E == physical only when the host reports the perflevel split. */
    if ((tmask & HC_TOPO_PCORES) && (tmask & HC_TOPO_ECORES)) {
        if (topo.pcores + topo.ecores != topo.physical) {
            printf("[CP1] sub-FAIL P(%d)+E(%d) != physical(%d)\n",
                   topo.pcores, topo.ecores, topo.physical);
            cp1 = 0;
        }
    }

    HCMisc misc; unsigned mmask = 0;
    hc_misc(c, &misc, &mmask);
    if (!(mmask & HC_MISC_BYTEORDER) || misc.byteorder != 1234) { printf("[CP1] sub-FAIL byteorder!=1234\n"); cp1 = 0; }
    if (!(mmask & HC_MISC_CACHELINE) || misc.cachelineBytes <= 0) { printf("[CP1] sub-FAIL cachelinesize\n"); cp1 = 0; }

    int neon = hc_feature(c, "hw.optional.neon");
    if (neon != 1) { printf("[CP1] sub-FAIL hw.optional.neon=%d (expect 1)\n", neon); cp1 = 0; }
    /* defensive-query rule (R-FEAT2): an absent OID must report -1, not crash. */
    int bogus = hc_feature(c, "hw.optional.this_oid_does_not_exist");
    if (bogus != -1) { printf("[CP1] sub-FAIL absent OID reported %d (expect -1)\n", bogus); cp1 = 0; }

    if (cp1) {
        printf("[CP1] PASS brand=\"%s\" ncpu=%d phys=%d log=%d P=%d E=%d LE cacheline=%dB neon=1\n",
               brand, topo.ncpu, topo.physical, topo.logical, topo.pcores, topo.ecores,
               misc.cachelineBytes);
    } else {
        ok = 0;
    }

    /* ---- [CP2] load delta + 16.16 scaling -------------------------------- */
    int cp2 = 1;

    HCTicks w0, w1;
    int cores0 = 0, cores1 = 0;
    unsigned long long ts0 = 0, ts1 = 0;
    if (hc_cpu_ticks(c, &w0, NULL, 0, &cores0, &ts0) != 0) { printf("[CP2] sub-FAIL ticks#1\n"); cp2 = 0; }

    /* Generate measurable busy time so dTotal > 0 (volatile to defeat the optimiser). */
    volatile double acc = 0.0;
    for (long i = 0; i < 60000000L; i++) acc += (double)i * 1.0000001;
    if (acc < 0) printf("");   /* keep acc live */

    if (hc_cpu_ticks(c, &w1, NULL, 0, &cores1, &ts1) != 0) { printf("[CP2] sub-FAIL ticks#2\n"); cp2 = 0; }

    if (cp2) {
        unsigned long long dUser = w1.user - w0.user;
        unsigned long long dSys  = w1.system - w0.system;
        unsigned long long dIdle = w1.idle - w0.idle;
        unsigned long long dNice = w1.nice - w0.nice;
        unsigned long long dTotal = dUser + dSys + dIdle + dNice;

        if (dTotal == 0) { printf("[CP2] sub-FAIL dTotal==0 (no elapsed ticks)\n"); cp2 = 0; }
        if (ts1 <= ts0)  { printf("[CP2] sub-FAIL timestamp not monotonic\n"); cp2 = 0; }

        if (cp2) {
            double f = 1.0 - (double)dIdle / (double)dTotal;   /* busy fraction */
            if (f < 0.0 || f > 1.0) { printf("[CP2] sub-FAIL fraction %.4f out of [0,1]\n", f); cp2 = 0; }

            uint32_t packed = pack_load_16_16(f);
            int tenths = sysmon_decode_tenths(packed);
            if (tenths < 0 || tenths > 1000) { printf("[CP2] sub-FAIL decode %d out of 0..1000\n", tenths); cp2 = 0; }

            /* first-sample-zero rule (R-LOADSTATE): a single read → 0. */
            if (sysmon_decode_tenths(pack_load_16_16(0.0)) != 0) { printf("[CP2] sub-FAIL seed!=0\n"); cp2 = 0; }
            /* full-load saturates without overflow: 0xffffffff decodes to 999
             * (99.9% — the inherent rounding of the 16.16 form), never 0/overflow. */
            if (sysmon_decode_tenths(pack_load_16_16(1.0)) < 999) { printf("[CP2] sub-FAIL f=1 decode<999\n"); cp2 = 0; }

            if (cp2) {
                printf("[CP2] PASS dTotal=%llu busy=%.1f%% packed=0x%08x decode=%d/1000 dt=%lluns cores=%d\n",
                       dTotal, f * 100.0, packed, tenths, ts1 - ts0, cores1);
            }
        }
    }
    if (!cp2) ok = 0;

    hc_close(c);

    if (ok) { printf("[CP] PASS host CPU facts + load math verified\n"); return 0; }
    printf("[CP] FAIL\n");
    return 1;
}
