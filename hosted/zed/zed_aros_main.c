/* zed_aros_main.c -- C entry for hosted Zed editor-core on AROS.
 * Sets the argc/argv globals the Rust std args pal reads (same contract as
 * hosted/rust/rs3_main.c), then calls the staticlib entry and returns its
 * exit code. */

extern int zed_aros_main(void);

int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return zed_aros_main();
}
