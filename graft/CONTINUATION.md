# CONTINUATION — resuming the darwin-aarch64 AROS boot (state as of 2026-06-24)

This is a precise hand-off so the next session resumes without re-deriving anything.
Read `graft/WORKFLOW.md` and `graft/UPSTREAM-NOTES.md` for the broader map.

## Headline

**AROS (AmigaOS) boots on Apple Silicon through the full AmigaDOS init.** kernel.resource,
exec.library, dos.library, emul-handler, dosboot.resource all run; multitasking works
(housekeeper/Guru/Boot-Mount tasks context-switch cleanly); the Apple-Silicon W^X wall
is passed (runtime-built seglist code executes). The boot reaches **dosboot's
volume-mount stage** and stops at a clean, symbolicated AmigaDOS alert.

## Committed fixes this session (branch `aarch64-darwin-graft` of `/Users/user/Source/aros-upstream`, LOCAL ONLY)

`git log --oneline` top ~14, newest first:
- `cba799ed` darwin-aarch64 host headers need -D__arm64__ (CPU-gated, make.cfg.in)
- `94bee0cd` CreateSegList builds its jump vector in executable memory (W^X hosts)
- `760395d4` **fix SAVEREGS/RESTOREREGS clobbering ExceptionContext.Flags** (the context-switch wall)
- `5a610c69` crash diagnostics — stack backtrace + full register dump on trap
- `54a79f6d` implement PrepareContext() for AArch64 (task creation works)
- `a68e4c5c` emul-handler builds on modern macOS (native 64-bit inodes)
- `202081a8` aarch64 fenv_t/setjmp/longjmp + dos LoadSeg relocations (dos.library builds)
- `c37b9dde` solve weak-StdC-stub linking globally + make build reproducible (LDFLAGS -lstdc.static; CPU-gated --allow-multiple-definition / -mcmodel=large in make.cfg.in)
- `2d46ddfc` use real -lstdc.static instead of hand-built libkrnmem.a
- earlier: kernel.resource/exec.library link, AROS boots exec+kernel+hostlib, etc.

## CURRENT precise blocker (where to resume)

dosboot tries to boot from **EMU:** (the emul-handler host-filesystem volume). The
scan finds EMU: but `dosboot_BootDos()` → `InitResident(dos.library)` → **`DosInit`
(rom/dos/dos_init.c) fails BEFORE reaching `CliInit`** (the actual boot). With dos
debug on, the trace is `[DosInit] Creating dos.library...` → `[DosInit] Expunge...`
→ `DosExpunge: Expunged.` — i.e. one of the base-creation steps returns NULL and it
tears down. No `CliInit` / `Mounting` / `Proposed SYS:` lines ever appear.

**IN-PROGRESS BISECT:** I added three `bug()` markers in `rom/dos/dos_init.c` (these
are UNCOMMITTED, plus `#define DEBUG 1` at the top):
- before `OpenDevice("timer.device", UNIT_VBLANK)` (~line 240)
- before `set_call_libfuncs(SETNAME(INITLIB), ...)` (~line 249)
- before `CliInit(NULL)` (~line 265)
The dos rebuild to see which marker is the last before "Expunge" was still running at
compaction. **Prime suspect: `OpenDevice("timer.device", UNIT_VBLANK)`** (the hosted
timer.device on aarch64 — note multitasking switches we saw were all *voluntary*
Wait()s, so the periodic VBLANK interrupt may not be wired up). Second suspect:
`set_call_libfuncs(INITLIB)` (dos's platform init set). **Next step: read di5.log /
re-run the bisect to find the failing step, then fix it.**

After dos boots: it mounts EMU: as SYS:, then wants `SYS:S/Startup-Sequence` (the host
dir `bin/.../AROS/` has none). The no-display CLI path is **emul-handler's emergency
shell** (`arch/all-hosted/filesys/emul_handler/boot.c` `EmulBoot`, RTF_AFTERDOS) which
runs an interactive Shell on the host terminal (eb_stdin/eb_stdout) when there's no
display — but it only runs AFTER dos boots, so dos must boot first.

## Build / run environment (ephemeral!)

- Build dir: `/private/tmp/claude-501/-Users-user-Source-aros-aarch64/<session>/scratchpad/arosbuild`
  — under `/private/tmp`, **may be cleared on reboot**. If gone, the AROS branch +
  these notes are the source of truth; the crosstools + configured build dir must be
  rebuilt (see `graft/build-darwin-aarch64.sh` and WORKFLOW.md).
- AROS tree: `<arosbuild>/bin/darwin-aarch64/AROS`. Boot dir: `…/AROS/boot/darwin`.
- Crosstools: `<arosbuild>/bin/darwin-aarch64/tools/crosstools/bin` (clang, llvm-nm…).
- `export PATH="$SCRATCH/graft-tools:/opt/homebrew/bin:$PATH"` before any `make`.
- Kickstart conf: `…/AROS/boot/darwin/AROSBootstrap.conf` — 18 `module` lines + an
  `arguments sysdebug=…` line. Entitlements plist: `$SCRATCH/aros-entitlements.plist`.
- Sign: `codesign -s - -f -o runtime --entitlements <plist> <…>/AROSBootstrap`.
- Run: `cd …/boot/darwin && ./AROSBootstrap -c "$PWD/AROSBootstrap.conf"` (it
  loops/halts at the alert; run backgrounded with `sleep 9; kill -9`).
- Stable user bundle (refreshed): `~/aros-darwin/run.sh`.

## Diagnostic tooling (committed) — USE THESE

- **Boot narration, NO rebuild:** add `arguments sysdebug=InitResident,InitCode,AddTask,CreateLibrary`
  to the conf. exec's `ExecLog` then prints each resident/task/library as it inits — the
  last line before a crash names the culprit. Flags in `rom/exec/exec_flags.c`.
- **Stack backtrace + full registers** print automatically on every trap (kernel.c
  `core_TrapHandler`, cpu_aarch64.h `PRINT_SC`).
- **Symbolicate a crash PC:** get a known `.text` loaded address as an anchor (e.g. the
  `AddTask` narration prints a task's initpc = a real exec function loaded address),
  `base = loaded_addr - nm_value`, then map the PC with `llvm-nm -n`. The alert path
  also prints `Module X .text (<base>) Offset <off>` directly.
- **Harness:** `graft/bootrun.sh <bootdir> <secs> <sysdebug-flags> <entitlements>`.

## BUILD FRICTION — important

Rebuilding `kernel-dos` / `kernel-dosboot` re-triggers a global include-generation
chain (codesets → expat, etc.), making each rebuild slow (minutes). And: **launch
builds in a STANDALONE command** (`nohup make kernel-X > log 2>&1 &`, returns
immediately), then poll in SEPARATE commands. Do NOT put the launch + a
`sleep`/`for` poll loop in the same Bash call — when that call hits the 2-minute
timeout it SIGTERMs the build's process group (the build dies "Terminated: 15").
`setsid` is NOT available on macOS.

## Uncommitted at compaction (clean these up next session)

- `rom/dos/dos_init.c`: `#define DEBUG 1` + 3 bisect `bug()` markers (revert after
  finding the failing step).
