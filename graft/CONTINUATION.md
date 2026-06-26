# CONTINUATION — darwin-aarch64 AROS (state as of 2026-06-25)

Precise hand-off so the next session resumes without re-deriving anything.
Read `graft/WORKFLOW.md` and `graft/UPSTREAM-NOTES.md` for the broader map.

## Headline

**AROS (AmigaOS) reaches an interactive AmigaDOS Shell prompt on Apple Silicon.**
The whole chain runs: exec / kernel.resource / hostlib / dos.library /
emul-handler / dosboot / unixio.hidd + BASE modules → EMU: mounts → SYS: assigned
→ boot assigns (C:, LIBS:, DEVS:, L:, S:) → `InitCode(RTF_AFTERDOS)` → the
emergency-console process LoadSegs the Shell from `L:UserShell-Seg`, **and the
Shell prints its `%N> ` prompt, reads typed commands, executes them, and exits
cleanly on EOF.** Verified on a real tty (via `expect`, which faithfully
reproduces Terminal.app): the Shell waits at the prompt, runs typed commands
(`Dir` lists the SYS: volume), loops, and exits cleanly on Ctrl-D; 0 traps. Also
verified on the unattended pipe path (input arriving after the parked read is
delivered). Multitasking, W^X, runtime code loading and console I/O all work.
**The full standard AmigaDOS command set is now installed in C:** (see the
command-set section below) — `Echo` and the rest run; missing commands are only
specialized GUI/dev tools.

## Library build-out (2026-06-25) — the GUI stack compiles AND runs

Building out the rest of AROS for darwin-aarch64. The thesis holds: with the
low-level port done (kernel/exec/host), the **upper layers are pure recompiles**.

- **Full standard C: command set built — 116 commands (2026-06-26).** The first
  73 came via a `workbench-c-tools` %build_progs target (committed) that bypasses
  the `Decoration` GUI-class link failure (a single missing `__ieee754_sin`) and
  the freetype/glu/jpeg GUI-lib fetch chain. The remaining 41 (the AmigaOS shell
  builtins `Echo`/`Ask`/`CD`/`If`/`Else`/`Run`/`Set`/`Path`/`Quit`/… plus
  `Identify`/`Partition`/`WBRun`/`LoadResource`/`R`) build with **no source fixes**
  via the **`-quick` mmake variant** (e.g. `make workbench-c-shellcommands-quick`).
  `-quick` skips the `includes`/`linklibs` dep metatargets — already built — and so
  **does not drag in the GUI-lib fetch chain** that stalls the plain targets. They
  land straight in the boot C: (`bin/darwin-aarch64/AROS/C`), no separate install.
  `Echo` verified live in the shell: `Which Echo` → `System:C/Echo`, and the
  `NOLINE`/`FIRST`/`LEN` options all behave per source. `Version` → `Kickstart
  51.51`. `Unpack` also built (needed `libbz2_nostdio.a`, already present; verified
  `Unpack ?` → `FILE/A,TO/A:`). **`CPUInfo` ported to AArch64** (upstream commits
  `526b560a` + `30d4f911` + `47c0bb84`): it was x86-only (`x86.c` decodes CPUID via
  inline asm; `main.c` walks an x86 `cpu.resource`). Now arch-clean — the CPUID path
  is guarded behind `__i386__`/`__x86_64__`, and a new portable backend
  `cpuinfo_arch.c` uses the architecture-neutral `processor.resource` (`GetCPUInfo()`,
  like ShowConfig) plus build-time AArch64 facts (ARMv8-A, 64-bit, X0–X30 + V0–V31,
  NEON). It uses dos.library output and is built `-noposixc`, so it needs only
  dos.library at runtime. **On darwin it also reports the real host CPU** —
  EL0 can't read MIDR_EL1/ID_AA64*, so it goes through `hostlib.resource` →
  `HostLib_Open("libSystem.dylib")` → `sysctlbyname` for `machdep.cpu.brand_string`
  + `hw.physicalcpu`/`hw.logicalcpu`/`hw.perflevelN.physicalcpu`, printing e.g.
  `Apple M5 — 10 (4 performance + 6 efficiency)`. Gated by `#if __aarch64__` plus the
  runtime hostlib/libSystem/sysctlbyname chain, so it's inert on every other
  platform; host calls use `HostLib_Lock/Unlock` (no `AROS_HOST_BARRIER` — undefined
  and unneeded on aarch64). Verified: `CPUInfo` / `CPUInfo VERBOSE` print correctly,
  0 traps. Also
  fixed `rom/processor/defaults.h` (no `__aarch64__` case → reported
  `PROCESSORARCH_UNKNOWN`; now maps to `PROCESSORARCH_ARM`, so it prints
  `Architecture: ARM`) and removed the stray `workbench-c-cpuinfo` target in
  `tools/zopfli/mmakefile.src` (undefined `EXEDIR` → `/CPUInfo`). Still unbuilt:
  `HDTool`/HDToolBox + `IPrefs` (Zune/intuition GUI, not console commands).
- **GUI library stack compiles, 0 source fixes**: graphics, intuition, layers,
  keymap (`kernel-graphics/-intuition/-layers/-keymap`), the HIDD base
  (`kernel-hidd` → `hiddclass.hidd`), gfx HIDD (`kernel-hidd-gfx` → `gfx.hidd`),
  `kernel-console` → console.device.
- **GUI library stack RUNS** (InitResident succeeds, 0 traps) once `hiddclass.hidd`
  is in the kickstart. **Key finding:** the darwin build's `AROSBootstrap.conf` was
  generated with a *stripped* module set (missing hiddclass.hidd / the GUI modules),
  so graphics.library used to fail to load — not a code bug, a missing module.
  `hiddclass.hidd` registers `CLID_HW_Root`, which `gfx_init.c:311`
  (`OOP_NewObject(CLID_HW_Root)`) needs; without it gfx.hidd init returns NULL and
  graphics.library cascades to NULL. With it: hiddclass → gfx.hidd → graphics →
  intuition all InitResident OK.
- **The proper fix** (vs the manual conf edit): the conf is generated by
  `arch/all-unix/boot/mkbootconf.sh` from the template
  `arch/all-unix/boot/AROSBootstrap.conf`, which already lists the full standard set
  (hiddclass, graphics, intuition, input HIDDs, gadtools, con-handler, …). Build the
  rest of that set, then regenerate the conf. In progress: input subsystem
  (`kernel-hidd-input/-kbd/-mouse`, `kernel-input/-keyboard/-gameport`),
  `workbench-libs-gadtools`, con-handler.
- **Boot still drops to the emergency shell** but now for the *right* reason: no
  display *driver* (monitor) — that's the cocoametal slot, not a library failure.

## The "solve memory once and for all" work — DONE & committed

Branch `aarch64-darwin-graft` of `/Users/user/Source/aros-upstream` (LOCAL ONLY).

- `4911e214` **real KrnAllocPages/KrnFreePages for hosted ports.** The hosted
  `KrnAllocPages` resolved to generic `rom/kernel/allocpages.c` (MMU page DB that
  the hosted port never populates) → a **silent no-op returning NULL**. That is
  why CliInit's `CreateSegList(internalBootCliHandler)` returned BNULL and dos
  failed to init. Replaced with a host-`mmap`/`munmap` allocator in
  `arch/all-unix/kernel/allocpages.c`+`freepages.c`, wired into the unix kernel
  FUNCS so they override the stubs (like setprotection/getpagesize already do).
- `71f75760` **executable memory is W^X-correct.** Both `CreateSegList`
  trampolines and `LoadSeg`'d ELF code (`internalloadseg_elf.c`, `loadseg.c`) map
  **R/W**, get written + relocated, then flip to **R/X** via `KrnSetProtection`
  (RWX is *refused* on Apple Silicon even with the disable-executable-page-
  protection entitlement — verified empirically — so populate-then-protect is
  mandatory). Page-allocated (executable) hunks are recognised by
  `TypeOfMem()==0` and freed with `KrnFreePages` (in both `unloadseg.c` and
  `loadseg.c` FreeFuncs).
- `4325bdb4` **unixio.hidd ported to darwin** (emul-handler needs it for the host
  libc interface). Dropped the `<sys/ioctl.h>`→`<net/if.h>` header drag (the
  class only calls ioctl via the libc fn-ptr); stubbed the Linux-only raw-packet
  methods in a new `unixpkt_darwin.c`. Added to the kickstart conf (residentpri
  91, inits before emul-handler at -1).
- Earlier: `-lstdc.static`, fenv/setjmp/longjmp, **PrepareContext**,
  **SAVEREGS/RESTOREREGS** (the context-switch wall), CreateSegList W^X seed,
  -D__arm64__, crash-diagnostic stack.

Non-code state needed to boot (in the build's `…/AROS/`): **`AROS.boot` must
contain `aarch64`** (it ships empty; `__dos_IsBootable` greps it for AROS_CPU, and
an empty file = "not bootable" = SYS: never mounts). `L:UserShell-Seg` must exist
(built by `make workbench-c-shell`). The full standard C: command set is now built
(114 commands incl. Echo/Type/List); `make workbench-c` still aborts at
`Decoration` (a link error), which is why the per-target `-quick` variants are used
instead of the umbrella target (see the command-set section above).

## The interactive-prompt work — DONE & committed (2026-06-25)

The console-I/O last mile is solved. Two commits on `aarch64-darwin-graft`:

- `0b703487` **wake `Hidd_UnixIO_Wait` via the timer tick on darwin.** Root cause
  of the original "hangs at console input": on darwin signal-driven I/O
  (`F_SETOWN`+`O_ASYNC`→`SIGIO`) is **not delivered for pipes** (sockets/ttys only),
  so a reader parked in `Wait` on a pipe stdin never woke (`SigIO_IntServer` never
  ran). Fix: also register `SigIO_IntServer` on the periodic timer IRQ (`SIGALRM`,
  on which `core_IRQ` is installed; `timer.device` arms `ITIMER_REAL`). The fd list
  is then polled every scheduler tick regardless of `SIGIO`, so `Wait` works for
  pipes and ttys. `arch/all-unix/hidd/unixio/{unixio.h,unixio_class.c}`, darwin-only.
- `d98975d7` **make the emul-handler console interactive.** *This* was the real
  blocker (the SIGIO fix alone was necessary but not sufficient). The display-less
  emergency Shell opens `"*"` (the AmigaDOS console alias) for its interactive
  console; with no console.device, the process console task is the emul-handler,
  which treated `"*"` as a host **filename** (`<root>/*` → ENOENT) → the Shell got
  no readable console and exited *before any read*. Fixes in
  `arch/all-hosted/filesys/emul_handler/emul_handler.c` +
  `arch/all-unix/filesys/emul_handler/emul_host.c`:
  (1) `open_()` resolves `"*"`/`"CONSOLE:"` to `eb_stdin`/`eb_stdout`;
  (2) `ACTION_FINDINPUT/OUTPUT` marks a console (`FHD_STDIO`) handle interactive so
  `IsInteractive()` is true → the Shell prints `%N> ` and loops (not batch);
  (3) `DoRead()` stdio path now breaks on `read()==0` (EOF) — previously only `-1`
  broke, so on stdin close the Shell spun returning NUL bytes instead of seeing EOF.

**How it was found (don't repeat the dead ends):** instrument the *emul-handler*,
not the Shell — the LoadSeg'd Shell's `bug()` is silent (it uses
`__EXEC_NOLIBBASE__`; its `D(bug)` only works under `DEBUG` with `ss->ss_SysBase`,
and even then only once it actually runs). The decisive trace was a `DoOpen`
hostname log (showed `<root>/*`) + an `ACTION_READ` fd/type log (showed the console
fd 0 was never read). **Verify with the reliable path:** a held-open pipe with
*delayed* input — `(sleep 2.5; printf 'Dir\n'; sleep 1) | AROSBootstrap` — proves
the Shell parks then wakes (`FGetC` returns the real byte after the delay) and
exits cleanly when the pipe closes. **Do NOT trust an immediate-exit/EndCLI timing
as proof of a read** (that was an earlier false positive — the Shell was exiting on
its own). The `expect` pty harness shows an immediate-EOF teardown quirk on the
controlling tty (a raw `openpty` slave polls not-ready correctly), so a live
Terminal.app session is best confirmed by hand; the mechanism is proven via pipe.

## Build / run environment (ephemeral!)

- Build dir: `/private/tmp/claude-501/-Users-user-Source-aros-aarch64/<session>/scratchpad/arosbuild`
  (under `/private/tmp`, may be cleared on reboot). If gone, the AROS branch +
  these notes are the source of truth; rebuild crosstools + configured build dir.
- Boot dir: `…/AROS/boot/darwin`. Confs there: `AROSBootstrap.conf` (plain) and
  `AROSBootstrap.sysdbg.conf` (+ `arguments sysdebug=…`). Both now list
  `…/Devs/Drivers/unixio.hidd`.
- `export PATH="$SCRATCH/graft-tools:/opt/homebrew/bin:$PATH"` before any `make`.
- Run: `cd …/boot/darwin && printf 'CMD\n' | ./AROSBootstrap -c "$PWD/AROSBootstrap.conf"`
  (it reaches the emergency shell; EMU: maps to `…/AROS`). Backgrounded + `kill -9`.
- Rebuilds: launch STANDALONE (`nohup make kernel-X > log 2>&1 &`, own command),
  poll separately, or the 2-min tool timeout SIGTERMs the build. `make kernel-dos`
  re-triggers a slow codesets/expat include chain (~3 min). The `kernel-dos`
  metatarget also touches `stdc.library` which fails an `llvm-strip` step at the
  very end — harmless, dos.library links before it; but if arossupport/a linklib
  changed, the module won't relink unless a direct module source also changed
  (touch e.g. cliinit.c to force it).

## Diagnostic tooling — USE THESE

- Boot narration, NO rebuild: `arguments sysdebug=InitResident,InitCode,AddTask,CreateLibrary,RamLib`.
- Stack backtrace + full registers print on every trap. Trap signal 10 = SIGBUS =
  executing non-executable memory (the W^X symptom — now fixed for code paths).
- Symbolicate a PC: the resident dump at boot lists module load addresses; or an
  AddTask narration prints a task initpc (a real loaded function address) to
  anchor `base = loaded - nm_value`, then map with `llvm-nm -n`.

## Clean tree

All diagnostic DEBUG/markers reverted (dos_init.c, cliinit.c, emul_handler.c back
to upstream). Working tree is clean on the branch; three new commits above.
