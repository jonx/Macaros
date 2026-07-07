/* hello_main.c -- C startup for the RustHello sample. AROS/collect-aros wants a C
 * `main`; it stashes argc/argv where sys/args/aros.rs reads them (std::env::args),
 * then calls the Rust demo. Mirrors rs3_main.c, minus the PASS/FAIL harness lines --
 * RustHello is a user-facing sample, so its own println! output is the whole show.
 */
extern int rust_hello_main(void);

/* Read by sys/args/aros.rs so std::env::args() works (C owns main, not lang_start). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return rust_hello_main();
}
