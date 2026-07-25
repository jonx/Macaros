/* rs_alloc_main.c -- ALLOC harness: growing over-aligned allocations. */
#include <proto/dos.h>
#define AROS_ALLOC_MAGIC 0x414c4c43u   /* "ALLC" */
extern unsigned int aros_rust_alloc_test(void);
int aros_argc = 0;
char **aros_argv = 0;
int main(int argc, char **argv)
{
    aros_argc = argc; aros_argv = argv;
    return aros_rust_alloc_test() == AROS_ALLOC_MAGIC ? 0 : 20;
}
