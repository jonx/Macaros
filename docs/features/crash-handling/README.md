# Crash handling

**Status: built.** A bounded **guru** with a symbolized backtrace: when a hosted
AROS task faults, the trap handler prints a self-contained report that names the
faulting `module function+offset` instead of leaving a bare address — turning a
wild fault into a starting point.

It is always on and free (no debug build needed), and it is deliberately
*bounded* so a crash in one task can't take the harness out of the unattended
loop.

## Related tools

The backtrace is the first line of defence; deeper digging uses the
[debug & bring-up tools](../debug-tools/README.md):

- **MUNGWALL** (`AROS_HOST_ARGS=mungwall …`) — guards every `AllocMem`/pool alloc,
  for a suspected allocator/memory bug.
- **host lldb** (`lldb -p "$(cat /tmp/aros-cm.pid)"`) — `Macaros` is a darwin
  process, so lldb catches the fault address before AROS's guru.

## Docs

- [design.md](design.md) — the trap handler, symbolization, and the bound
