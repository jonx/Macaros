/* t3_nativelib_test.c — [T3] the native-library bootstrap, host harness (OURS).
 *
 * Proves the AmigaOS idiom end to end: the program reads SysBase from absolute
 * address 4, opens dos.library through exec's OpenLibrary, and calls Output()
 * and Write() through the returned base - with the "native" dos.library being
 * a stand-in here at exactly the seam where AROS's real one plugs in
 * (emu68k_set_oscall; the in-OS implementation is emu68k_oscall.c).
 * Marker: [T3HELLO] PASS / FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "emu68k_host.h"

struct regs68k { uint32_t d[8], a[8]; };   /* the leading, stable part */

static int oscall(const char *lib, int lvo, void *regs, void *guest0,
                  void *user, char *err, unsigned el)
{
    struct regs68k *r = regs;
    (void)user;
    if (strcmp(lib, "dos.library") != 0) {
        snprintf(err, el, "no such library here"); return 1;
    }
    if (lvo == 10) { r->d[0] = 0x2A2A2A2A; return 0; }        /* Output() -60   */
    if (lvo == 8) {                                            /* Write()  -48   */
        uint32_t buf = r->d[2], len = r->d[3];
        fwrite((char *)guest0 + buf, 1, len, stdout);
        r->d[0] = len;
        return 0;
    }
    snprintf(err, el, "dos LVO %d not served", lvo);
    return 1;
}

int main(void)
{
    void *h = dlopen("build/libemu68k.dylib", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    emu68k_run *(*p_new)(const void*,unsigned long,const char*,unsigned long,
                         emu68k_sink_fn,void*,char*,unsigned) = dlsym(h,"emu68k_run_new");
    int (*p_q)(emu68k_run*,unsigned long,unsigned int*,char*,unsigned) = dlsym(h,"emu68k_run_quantum");
    void (*p_free)(emu68k_run*) = dlsym(h,"emu68k_run_free");
    void (*p_oscall)(emu68k_oscall_fn,void*) = dlsym(h,"emu68k_set_oscall");
    if (!p_oscall) { printf("emu68k_set_oscall missing\n"); return 1; }
    p_oscall(oscall, NULL);

    FILE *f = fopen("hosted/emu68k/nativelib/bin/hellodos.exe","rb");
    if (!f) { printf("no test program\n"); return 1; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *b=malloc(n); if (fread(b,1,n,f)!=(size_t)n) return 1; fclose(f);

    char err[256]={0}; unsigned d0=0;
    emu68k_run *r = p_new(b,n,"",0,NULL,NULL,err,sizeof err);
    if (!r) { printf("load: %s\n", err); return 1; }
    int rc; while ((rc=p_q(r,4096,&d0,err,sizeof err))==EMU68K_RC_YIELD){}
    p_free(r);
    if (rc != 0 || d0 != 0) {
        printf("[T3HELLO] FAIL: rc=%d exit=%u %s\n", rc, d0, err);
        return 1;
    }
    printf("[T3HELLO] PASS: a real AmigaOS-idiom 68k program (SysBase from "
           "absolute 4, OpenLibrary through exec, Output/Write through the "
           "returned base) ran its whole library bootstrap and printed through "
           "the native seam.\n");
    return 0;
}
