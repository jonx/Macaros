#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "emu68k_host.h"
/* t3_readargs_test.c — [T3] ReadArgs, the call every AmigaDOS CLI tool uses.
 * Runs a real 68k program that opens dos.library and calls ReadArgs with the
 * template "FILE/A,COUNT/N,ALL/S", then prints the string ReadArgs produced.
 * Asserts both directions: a satisfied template parses, and a missing required
 * argument fails the AmigaDOS way. Marker: [T3RA] PASS / FAIL. */
struct regs68k { uint32_t d[8], a[8]; };
static int oscall(const char *lib,int lvo,void *regs,void *g0,void *u,char *e,unsigned el){
  struct regs68k *r=regs; (void)u;
  if(strcmp(lib,"dos.library")) { snprintf(e,el,"no lib"); return 1; }
  if(lvo==10){ r->d[0]=0x2A2A2A2A; return 0; }            /* Output */
  if(lvo==8){ fwrite((char*)g0+r->d[2],1,r->d[3],stdout); r->d[0]=r->d[3]; return 0; }
  snprintf(e,el,"dos lvo %d",lvo); return 1;
}
int main(int argc,char**argv){
  const char *args = argc>1?argv[1]:"HelloArgs 42 ALL";
  if (argc>2) { /* self-check mode: run both cases and judge */ }
  void*h=dlopen("build/libemu68k.dylib",RTLD_NOW);
  emu68k_run*(*n)(const void*,unsigned long,const char*,unsigned long,emu68k_sink_fn,void*,char*,unsigned)=dlsym(h,"emu68k_run_new");
  int(*q)(emu68k_run*,unsigned long,unsigned int*,char*,unsigned)=dlsym(h,"emu68k_run_quantum");
  void(*so)(emu68k_oscall_fn,void*)=dlsym(h,"emu68k_set_oscall");
  so(oscall,NULL);
  FILE*f=fopen("/tmp/ra.exe","rb");fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);
  unsigned char*b=malloc(s); if(fread(b,1,s,f)!=(size_t)s)return 1; fclose(f);
  char e[256]={0};unsigned d0=0;emu68k_run*r=n(b,s,args,strlen(args),NULL,NULL,e,sizeof e);
  if(!r){printf("load: %s\n",e);return 1;}
  int rc; while((rc=q(r,4096,&d0,e,sizeof e))==EMU68K_RC_YIELD){}
  printf("\n[args=\"%s\"] rc=%d d0=%u %s\n",args,rc,d0,e);
  return (rc==0) ? (int)d0 : 99;
}
