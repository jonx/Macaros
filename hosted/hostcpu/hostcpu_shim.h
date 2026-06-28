/* hostcpu_shim.h — flat C ABI for the darwin/AArch64 processor.resource backend.
 *
 * Implemented from docs/features/processor-resource/spec.md ("The C ABI
 * (hostcpu_shim.h)" and "The host facts — required behaviour"). Independent work:
 * no third-party implementation source — emulator, agent, driver, or otherwise —
 * was read, searched, or consulted in producing it, and any resemblance to
 * existing implementations is coincidental. Sources: Apple sysctl(3)/Mach
 * host_statistics/host_processor_info docs [PUB]; the Arm ARM for what is
 * EL0-unreadable [PUB]; this project's flat-C host-shim shape
 * (hosted/coreaudio/coreaudio_shim.h, the bsdsock verbs) [OURS]. No AROS headers
 * are pulled here; the AROS side pulls no Mach/sysctl headers.
 *
 * This header is the ONLY contact surface between the AROS-side processor.resource
 * darwin backend (AROS crosstools) and this host shim (Apple clang). It is also the
 * surface the Daedalos app links/loads directly (see hosted/hostcpu/README.md):
 * the static-fact verbs are pure sysctl forwarders and the one stateful fact
 * (CPU load) lives behind hc_cpu_ticks as cumulative counters + a host timestamp,
 * so the consumer owns the delta.
 *
 * On Apple Silicon AROS runs at EL0, where MIDR_EL1 / the ID_AA64* registers are
 * not readable; querying the host (sysctl/Mach) is the only way to learn the part.
 * All calls here are read-only and prompt-free: no entitlement, no TCC.
 */
#ifndef HOSTCPU_SHIM_H
#define HOSTCPU_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HCContext HCContext;

/* Open: resolve sysctl + Mach entry points; no host resources held beyond the
   handle. Returns NULL on failure (out of memory). */
HCContext *hc_open(void);
void       hc_close(HCContext *);

/* STATIC IDENTITY — brand = machdep.cpu.brand_string (e.g. "Apple M5"). Returns 0
   on success; nonzero (and brand[0]='\0') if the OID is absent. */
int hc_brand(HCContext *, char *brand, int brandLen);

/* STATIC TOPOLOGY — hw.ncpu / hw.physicalcpu / hw.logicalcpu and the P/E split
   hw.perflevel0.physicalcpu (performance) + hw.perflevel1.physicalcpu (efficiency).
   Any field the host lacks is left 0 and reported absent via the bitmask `*haveMask`
   (bit per field, in declaration order). Returns 0 on success. */
typedef struct {
    int ncpu, physical, logical, pcores, ecores;
} HCTopology;
#define HC_TOPO_NCPU     (1u<<0)
#define HC_TOPO_PHYSICAL (1u<<1)
#define HC_TOPO_LOGICAL  (1u<<2)
#define HC_TOPO_PCORES   (1u<<3)
#define HC_TOPO_ECORES   (1u<<4)
int hc_topology(HCContext *, HCTopology *out, unsigned *haveMask);

/* STATIC MISC — hw.byteorder (1234=LE/4321=BE), hw.cachelinesize and the cache
   sizes in BYTES (caller converts to kB for GCIT_*CacheSize). Absent fields 0,
   reported via `*haveMask` (bit per field, in declaration order). */
typedef struct {
    int byteorder, cachelineBytes;
    unsigned long long l1iBytes, l1dBytes, l2Bytes, l3Bytes;
} HCMisc;
#define HC_MISC_BYTEORDER (1u<<0)
#define HC_MISC_CACHELINE (1u<<1)
#define HC_MISC_L1I       (1u<<2)
#define HC_MISC_L1D       (1u<<3)
#define HC_MISC_L2        (1u<<4)
#define HC_MISC_L3        (1u<<5)
int hc_misc(HCContext *, HCMisc *out, unsigned *haveMask);

/* STATIC FEATURES — query a named hw.optional[.arm] OID DEFENSIVELY: returns 1 if
   the OID exists and is nonzero, 0 if it exists and is zero, and -1 if the OID is
   ABSENT (never assume a name exists — names vary per chip/OS). */
int hc_feature(HCContext *, const char *oidName);   /* e.g. "hw.optional.neon" */

/* LIVE LOAD — the one stateful fact. Returns CUMULATIVE CPU ticks (NOT a
   percentage) for the whole machine and up to maxCores per-core, plus a monotonic
   timestamp in ns (mach_absolute_time/mach_timebase_info) so the consumer computes
   load = 1 - dIdle/dTotal over the true elapsed host time. *outCores = host core
   count. Returns 0 on success. Pass whole=NULL or perCore=NULL to skip either. */
typedef struct { unsigned long long user, system, idle, nice; } HCTicks;
int hc_cpu_ticks(HCContext *, HCTicks *whole, HCTicks *perCore, int maxCores,
                 int *outCores, unsigned long long *outTimestampNs);

#ifdef __cplusplus
}
#endif

#endif /* HOSTCPU_SHIM_H */
