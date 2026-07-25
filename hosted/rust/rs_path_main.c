/* rs_path_main.c -- PATH harness: Path::is_absolute on AROS volume paths. */
#include <proto/dos.h>
#define AROS_PATH_MAGIC 0x50415448u   /* "PATH" */
extern unsigned int aros_rust_path_test(void);
int aros_argc = 0;
char **aros_argv = 0;
int main(int argc, char **argv)
{
    aros_argc = argc; aros_argv = argv;
    return aros_rust_path_test() == AROS_PATH_MAGIC ? 0 : 20;
}
