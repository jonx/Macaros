/* rs_shell_main.c -- SHELL harness: a shell as a child over live pipes. */
#include <proto/dos.h>
#define AROS_SHELL_MAGIC 0x53484c4cu   /* "SHLL" */
extern unsigned int aros_rust_shell_test(void);
int aros_argc = 0;
char **aros_argv = 0;
int main(int argc, char **argv)
{
    aros_argc = argc; aros_argv = argv;
    return aros_rust_shell_test() == AROS_SHELL_MAGIC ? 0 : 20;
}
