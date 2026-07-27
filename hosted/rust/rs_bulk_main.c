/* rs_bulk_main.c -- BULK harness: a lot of output through one pipe. */
#include <proto/dos.h>
#define AROS_BULK_MAGIC 0x42554c4bu   /* "BULK" */
extern unsigned int aros_rust_bulk_test(void);
int aros_argc = 0;
char **aros_argv = 0;
int main(int argc, char **argv)
{
    aros_argc = argc; aros_argv = argv;
    return aros_rust_bulk_test() == AROS_BULK_MAGIC ? 0 : 20;
}
