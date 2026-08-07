/* bridge_lab.h - the runtime event recorder (OURS, AROS-licensed).
 *
 * Records what a 68k program asked the RUNTIME for, as JSON Lines, so a run
 * becomes reusable evidence instead of a terminal that has scrolled away.
 *
 * The consumer is an agent. That decides the shape: events are compact and
 * machine-first, identities are assigned by FIRST-SEEN ORDER rather than by
 * address (a bump allocator makes addresses shift on unrelated edits, and
 * address-keyed baselines would churn on every commit), and enabling the
 * recorder always produces run.start - so an empty file means "the flag never
 * reached the process" and a file with only run.start means "the events did not
 * happen", which are different bugs that must not look the same.
 *
 * Disabled by default. It never allocates from guest memory, never changes what
 * the program observes, and cannot enable a crossing: a trace is evidence, and
 * only a reviewed contract registry can change what the bridge will serve. */

#ifndef BRIDGE_LAB_H
#define BRIDGE_LAB_H

#include <stdint.h>

#define BL_OFF      0
#define BL_SUMMARY  1
#define BL_RUNTIME  2
#define BL_CALLS    3
#define BL_DEBUG    4

/* Open the trace if EMU68K_BRIDGE_TRACE names a file; emits run.start.
 * Runtime detail is capped at 32 MiB by default, with 64 KiB reserved for the
 * trace.truncated and final summary records. EMU68K_BRIDGE_TRACE_MAX_BYTES
 * overrides the cap; zero explicitly selects unlimited output. */
void bl_open(const char *program);
void bl_close(const char *result);

/* The active level, so a caller can skip building an event it would drop. */
int  bl_level(void);
static inline int bl_on(int level) { return bl_level() >= level; }

/* Stable identities. Each namespace numbers what it sees in the order it sees
 * it, so the same run produces the same names however the heap moved. */
const char *bl_id(const char *kind, uint32_t addr);

/* One event. `fields` is already-formatted JSON without the enclosing braces,
 * or NULL. The common keys (schema, seq, context, task, pc) are added here. */
void bl_event(int level, int context, uint32_t task, uint32_t pc,
              const char *event, const char *fields, ...);

#endif /* BRIDGE_LAB_H */
