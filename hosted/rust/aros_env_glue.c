/* aros_env_glue.c -- enumerate the process's local environment variables for the
 * Rust env pal (std::env::vars, sys/env/aros.rs env()).
 *
 * AROS has no POSIX `environ` array: getenv/setenv read and write the process's
 * pr_LocalVars list (LV_VAR nodes), created by SetVar(..., LV_VAR|GVF_LOCAL_ONLY).
 * That is exactly what std::env::set_var writes here, so enumerating LV_VAR nodes is
 * the honest counterpart to getenv/setenv. Global ENV: file variables are out of
 * scope (getenv reads them, but they are not part of this process's local set).
 *
 * We hand each (name, value) pair to a Rust callback instead of packing a buffer, so
 * the pal never has to size an output buffer or lay out the LocalVar struct. We do
 * NOT Forbid() across the callback: the callback allocates (Rust global allocator),
 * and allocating under Forbid() is illegal; the local-var list is per-process and
 * mutated only by this process's own SetVar/DeleteVar, so a lone enumerating thread
 * sees a stable list (same threading caveat as the rest of the env pal).
 *
 * Header-clean: AROS headers only, no macOS SDK. Compiled with -ffixed-x18.
 *
 * Independent work: from the AROS dos.library FindVar/ScanVars autodocs + headers.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>

static unsigned long z_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

void aros_env_enum(void (*cb)(void *ctx,
                              const unsigned char *name, unsigned long name_len,
                              const unsigned char *val, unsigned long val_len),
                   void *ctx)
{
    struct Process  *pr = (struct Process *)FindTask(NULL);
    struct LocalVar *var;

    if (!pr || !cb)
        return;

    ForeachNode(&pr->pr_LocalVars, var) {
        if (var->lv_Node.ln_Type == LV_VAR && var->lv_Node.ln_Name) {
            const unsigned char *val = (const unsigned char *)var->lv_Value;
            unsigned long        vlen = val ? (unsigned long)var->lv_Len : 0;
            cb(ctx,
               (const unsigned char *)var->lv_Node.ln_Name,
               z_strlen(var->lv_Node.ln_Name),
               val, vlen);
        }
    }
}
