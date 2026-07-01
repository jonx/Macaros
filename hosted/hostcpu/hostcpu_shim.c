/* hostcpu_shim.c — darwin/AArch64 host CPU facts for AROS's processor.resource.
 *
 * Independent work: no third-party implementation source was read, searched, or
 * consulted; any resemblance is coincidental. Implemented from
 * docs/features/processor-resource/spec.md against Apple's documented sysctl(3)
 * and Mach host_statistics/host_processor_info interfaces [PUB]. Read-only and
 * prompt-free: no entitlement, no TCC.
 *
 * Pulls NO AROS headers. The whole file is plain C over libSystem; it links into
 * libhostcpu.dylib (dlopen'd by the AROS side via hostlib.resource) and can be
 * compiled straight into the Macaros app.
 */
#include "hostcpu_shim.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <mach/mach_time.h>

struct HCContext { int alive; };

HCContext *hc_open(void)
{
    HCContext *c = (HCContext *)calloc(1, sizeof *c);
    if (c) c->alive = 1;
    return c;
}

void hc_close(HCContext *c) { free(c); }

/* sysctlbyname for a string OID. */
static int sc_str(const char *name, char *buf, int len)
{
    size_t n = (size_t)len;
    if (len <= 0) return -1;
    if (sysctlbyname(name, buf, &n, NULL, 0) != 0) { buf[0] = '\0'; return -1; }
    buf[len - 1] = '\0';
    return 0;
}

/* sysctlbyname for an integer OID. Handles both 32-bit and 64-bit OIDs; darwin is
 * little-endian (this is a darwin-only shim) so the low bytes carry the value. */
static int sc_ll(const char *name, long long *out)
{
    uint64_t v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0) != 0) return -1;
    if (n == 4)      *out = (long long)(uint32_t)v;
    else if (n == 8) *out = (long long)v;
    else             return -1;
    return 0;
}

int hc_brand(HCContext *c, char *brand, int brandLen)
{
    (void)c;
    if (!brand || brandLen <= 0) return -1;
    return sc_str("machdep.cpu.brand_string", brand, brandLen);
}

int hc_topology(HCContext *c, HCTopology *out, unsigned *haveMask)
{
    (void)c;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    unsigned have = 0;
    long long v;
    if (sc_ll("hw.ncpu", &v) == 0)                    { out->ncpu     = (int)v; have |= HC_TOPO_NCPU; }
    if (sc_ll("hw.physicalcpu", &v) == 0)             { out->physical = (int)v; have |= HC_TOPO_PHYSICAL; }
    if (sc_ll("hw.logicalcpu", &v) == 0)              { out->logical  = (int)v; have |= HC_TOPO_LOGICAL; }
    if (sc_ll("hw.perflevel0.physicalcpu", &v) == 0)  { out->pcores   = (int)v; have |= HC_TOPO_PCORES; }
    if (sc_ll("hw.perflevel1.physicalcpu", &v) == 0)  { out->ecores   = (int)v; have |= HC_TOPO_ECORES; }
    if (haveMask) *haveMask = have;
    return 0;
}

int hc_misc(HCContext *c, HCMisc *out, unsigned *haveMask)
{
    (void)c;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    unsigned have = 0;
    long long v;
    if (sc_ll("hw.byteorder", &v) == 0)     { out->byteorder     = (int)v; have |= HC_MISC_BYTEORDER; }
    if (sc_ll("hw.cachelinesize", &v) == 0) { out->cachelineBytes = (int)v; have |= HC_MISC_CACHELINE; }
    if (sc_ll("hw.l1icachesize", &v) == 0)  { out->l1iBytes = (unsigned long long)v; have |= HC_MISC_L1I; }
    if (sc_ll("hw.l1dcachesize", &v) == 0)  { out->l1dBytes = (unsigned long long)v; have |= HC_MISC_L1D; }
    if (sc_ll("hw.l2cachesize", &v) == 0)   { out->l2Bytes  = (unsigned long long)v; have |= HC_MISC_L2; }
    if (sc_ll("hw.l3cachesize", &v) == 0)   { out->l3Bytes  = (unsigned long long)v; have |= HC_MISC_L3; }
    if (haveMask) *haveMask = have;
    return 0;
}

int hc_feature(HCContext *c, const char *oidName)
{
    (void)c;
    if (!oidName) return -1;
    long long v;
    if (sc_ll(oidName, &v) != 0) return -1;   /* OID absent → feature unknown/false */
    return v != 0 ? 1 : 0;
}

int hc_cpu_ticks(HCContext *c, HCTicks *whole, HCTicks *perCore, int maxCores,
                 int *outCores, unsigned long long *outTimestampNs)
{
    (void)c;

    /* Whole-machine cumulative ticks. HOST_CPU_LOAD_INFO uses host_statistics
     * (the canonical call for this flavour); cpu_ticks[] are natural_t. */
    if (whole) {
        host_cpu_load_info_data_t load;
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                            (host_info_t)&load, &count) != KERN_SUCCESS)
            return -1;
        whole->user   = load.cpu_ticks[CPU_STATE_USER];
        whole->system = load.cpu_ticks[CPU_STATE_SYSTEM];
        whole->idle   = load.cpu_ticks[CPU_STATE_IDLE];
        whole->nice   = load.cpu_ticks[CPU_STATE_NICE];
    }

    /* Per-core cumulative ticks. Must vm_deallocate the returned array. */
    if (perCore && maxCores > 0) {
        natural_t ncpu = 0;
        processor_info_array_t info = NULL;
        mach_msg_type_number_t infoCnt = 0;
        if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                &ncpu, &info, &infoCnt) == KERN_SUCCESS) {
            processor_cpu_load_info_t cpu = (processor_cpu_load_info_t)info;
            int k = (int)ncpu;
            if (k > maxCores) k = maxCores;
            for (int i = 0; i < k; i++) {
                perCore[i].user   = cpu[i].cpu_ticks[CPU_STATE_USER];
                perCore[i].system = cpu[i].cpu_ticks[CPU_STATE_SYSTEM];
                perCore[i].idle   = cpu[i].cpu_ticks[CPU_STATE_IDLE];
                perCore[i].nice   = cpu[i].cpu_ticks[CPU_STATE_NICE];
            }
            if (outCores) *outCores = (int)ncpu;
            vm_deallocate(mach_task_self(), (vm_address_t)info,
                          infoCnt * sizeof(integer_t));
        } else if (outCores) {
            *outCores = 0;
        }
    } else if (outCores) {
        *outCores = 0;
    }

    if (outTimestampNs) {
        static mach_timebase_info_data_t tb = {0, 0};
        if (tb.denom == 0) mach_timebase_info(&tb);
        unsigned long long t = mach_absolute_time();
        *outTimestampNs = (tb.denom ? t * tb.numer / tb.denom : t);
    }
    return 0;
}
