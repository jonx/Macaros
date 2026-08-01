/* emu68k_host.h — the host-side 68k execution service (libemu68k.dylib).
 * (OURS, AROS-licensed. The dylib links the engine via libjit68k; this header
 * contains NO Emu68 source.)
 *
 * One C API for the AROS side (emu68k.library via hostlib.resource): create a
 * runnable program from raw hunk-file bytes, run it in bounded QUANTA (so the
 * caller can breathe between them: AROS scheduling, break checks, interleave),
 * kill it asynchronously, free it. Output streams through a sink callback the
 * caller supplies (the AROS side writes it to the process console).
 *
 * THREADING CONTRACT: calls for ALL runs must be serialized by the caller
 * (hosted AROS tasks share one host thread; emu68k.library holds a semaphore
 * across every call). emu68k_run_kill is the one exception: callable any time
 * (one volatile store).
 *
 * Faults inside translated code are contained per [T0-P3]: the quantum returns
 * EMU68K_RC_ERROR with the message, a crash bundle is written (dir settable),
 * and the process survives. */

#ifndef EMU68K_HOST_H
#define EMU68K_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct emu68k_run emu68k_run;

/* output sink: called with program output bytes as they appear (quantum end) */
typedef void (*emu68k_sink_fn)(const char *buf, long len, void *user);

#define EMU68K_RC_DONE   0   /* program exited; *exit_d0 holds its D0            */
#define EMU68K_RC_YIELD  1   /* quantum used up; call emu68k_run_quantum again   */
#define EMU68K_RC_KILLED 2   /* a kill request landed at a safe point            */
#define EMU68K_RC_ERROR  (-1)/* load/translate/fault error; err holds the reason */

/* Load + relocate a hunk image into a fresh guest arena with its own engine
 * instance and stub OS; deliver the AmigaDOS argument string (args/argslen,
 * WITHOUT the trailing newline — it is appended here). NULL on error. */
emu68k_run *emu68k_run_new(const void *image, unsigned long imagelen,
                           const char *args, unsigned long argslen,
                           emu68k_sink_fn sink, void *sink_user,
                           char *err, unsigned errlen);

/* Run up to max_roundtrips dispatcher roundtrips. Flushes new output to the
 * sink before returning. Returns an EMU68K_RC_* code. */
int emu68k_run_quantum(emu68k_run *r, unsigned long max_roundtrips,
                       unsigned int *exit_d0, char *err, unsigned errlen);

/* Async kill: the run stops at its next safe point (chained loops included). */
void emu68k_run_kill(emu68k_run *r);

void emu68k_run_free(emu68k_run *r);

/* Crash-bundle directory for subsequent runs (default: the engine's own). */
void emu68k_set_crash_dir(const char *dir);

const char *emu68k_version(void);

#ifdef __cplusplus
}
#endif
#endif /* EMU68K_HOST_H */
