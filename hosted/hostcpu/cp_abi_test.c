/* cp_abi_test.c — [CP3] same values through the hostlib (dlopen) boundary.
 *
 * This is the darwin analogue of cocoametal-abi / coreaudio-abi: it links NONE of
 * the shim source. It dlopen()s build/hostcpu/hostcpu.dylib (the REAL artifact the
 * AROS side loads via hostlib.resource → HostLib_Open), resolves the _hc_* symbols,
 * and asserts the values it gets back EQUAL the raw sysctl values it reads itself.
 * Proves the dylib + exports surface is correct and lossless across the boundary.
 *
 * Independent work; resemblance coincidental. Prints "[CP3] PASS"; exits 0 on PASS.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/sysctl.h>

#include "hostcpu_shim.h"   /* types + prototypes only; symbols come from the dylib */

typedef HCContext *(*fn_open)(void);
typedef void       (*fn_close)(HCContext *);
typedef int        (*fn_brand)(HCContext *, char *, int);
typedef int        (*fn_topo)(HCContext *, HCTopology *, unsigned *);
typedef int        (*fn_feat)(HCContext *, const char *);

static int raw_int(const char *name, long long *out)
{
    uint64_t v = 0; size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0) != 0) return -1;
    *out = (n == 4) ? (long long)(uint32_t)v : (long long)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "build/hostcpu/hostcpu.dylib";

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[CP3] FAIL dlopen(%s): %s\n", path, dlerror()); return 1; }

    fn_open  p_open  = (fn_open)  dlsym(h, "hc_open");
    fn_close p_close = (fn_close) dlsym(h, "hc_close");
    fn_brand p_brand = (fn_brand) dlsym(h, "hc_brand");
    fn_topo  p_topo  = (fn_topo)  dlsym(h, "hc_topology");
    fn_feat  p_feat  = (fn_feat)  dlsym(h, "hc_feature");
    if (!p_open || !p_close || !p_brand || !p_topo || !p_feat) {
        printf("[CP3] FAIL missing exported symbol(s)\n"); return 1;
    }

    int ok = 1;
    HCContext *c = p_open();
    if (!c) { printf("[CP3] FAIL hc_open via dylib\n"); return 1; }

    /* brand: dylib vs raw sysctl */
    char dl_brand[160] = {0}, raw_brand[160]; size_t bn = sizeof raw_brand;
    p_brand(c, dl_brand, sizeof dl_brand);
    if (sysctlbyname("machdep.cpu.brand_string", raw_brand, &bn, NULL, 0) != 0) raw_brand[0] = '\0';
    if (strcmp(dl_brand, raw_brand) != 0) { printf("[CP3] FAIL brand mismatch '%s' != '%s'\n", dl_brand, raw_brand); ok = 0; }

    /* topology: dylib vs raw sysctl */
    HCTopology t; unsigned tm = 0;
    p_topo(c, &t, &tm);
    long long rphys = -1, rlog = -1;
    raw_int("hw.physicalcpu", &rphys);
    raw_int("hw.logicalcpu", &rlog);
    if (rphys >= 0 && t.physical != (int)rphys) { printf("[CP3] FAIL physical %d != %lld\n", t.physical, rphys); ok = 0; }
    if (rlog  >= 0 && t.logical  != (int)rlog)  { printf("[CP3] FAIL logical %d != %lld\n", t.logical, rlog); ok = 0; }

    /* feature: dylib neon vs raw sysctl neon */
    long long rneon = 0;
    int have_neon_raw = (raw_int("hw.optional.neon", &rneon) == 0);
    int dl_neon = p_feat(c, "hw.optional.neon");
    int exp_neon = have_neon_raw ? (rneon != 0 ? 1 : 0) : -1;
    if (dl_neon != exp_neon) { printf("[CP3] FAIL neon %d != %d\n", dl_neon, exp_neon); ok = 0; }

    p_close(c);
    dlclose(h);

    if (ok) { printf("[CP3] PASS dylib boundary lossless (brand=\"%s\" phys=%d log=%d neon=%d)\n",
                     dl_brand, t.physical, t.logical, dl_neon); return 0; }
    printf("[CP3] FAIL\n");
    return 1;
}
