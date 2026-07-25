/* rs_proc_main.c -- PROC harness: std::process streaming child pipes.
 * aros_rust_proc_test prints its own PASS/FAIL through std. */
#include <proto/dos.h>

#define AROS_PROC_MAGIC 0x50524f43u   /* "PROC" */

extern unsigned int aros_rust_proc_test(void);

int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return aros_rust_proc_test() == AROS_PROC_MAGIC ? 0 : 20;
}
