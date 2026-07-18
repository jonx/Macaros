# Upstream notes — friction hit bringing up `darwin-aarch64`, worth fixing for everyone

A running list of AROS build-system issues found while grafting a new
`darwin-aarch64` (hosted-on-Apple-Silicon) target. Most aren't AArch64-specific —
they're general robustness/bit-rot problems that bite anyone building AROS today,
especially on macOS. Each is a candidate patch for `aros-development-team`.

## Build-system robustness

1. **`fetch.sh` can hang forever on a stalled download.** `curl_http`'s URL-resolve
   probe (`curl -fsIL ... -w %{url_effective}`) and the TLS-1.0 fallback had **no
   `--connect-timeout`/`--max-time`**, so a flaky mirror wedges the whole build
   indefinitely (we lost an hour to it twice). Only the actual transfer had
   `--speed-limit/--speed-time`. *Fix:* add `--connect-timeout 30 --max-time 120`
   to the probes and `--connect-timeout 30` to the transfer. (Done on our branch.)

2. **Killed builds leave the tree in confusing half-states.** Two traps hit:
   - a stale `.installflag-crosstools` / `.setupflag-*` makes mmake think the
     crosstools are built and silently skip the compile, then fail later using a
     `clang` that never got produced (`No such file or directory`);
   - re-running setup after a source dir was already moved fails on `mv ... src:
     No such file`.
   *Fix idea:* validate that the installed artifact actually exists before trusting
   an install/setup flag; make setup steps idempotent / re-entrant.

3. **The patch-applied marker scheme is brittle.** Patches are gated by a
   `.<patch>.applied` marker; if it's lost but the source is already patched,
   re-applying fails (`hunk failed`). There's no "is this patch already applied?"
   check (`patch -R --dry-run`), so any marker loss is unrecoverable without manual
   surgery. *Fix idea:* detect already-applied patches and skip, instead of relying
   solely on a sentinel file.

## Stale / missing toolchain pieces

4. **Default LLVM is 11.0.0 (2020) — too old to build cleanly on current macOS.**
   `config/llvm_def` pins `11.0.0`; bumping to `20.1.0` (which already has AROS
   diffs for llvm/clang/compiler-rt/libcxx/libcxxabi/libunwind) builds on macOS 26 /
   Apple clang 21. *Fix:* default to a modern LLVM, or detect host toolchain age.

5. **`lld` AROS patch missing for 20.1.0.** `tools/crosstools/llvm/` ships
   `lld-11.0.0.src-aros.diff` but no `lld-20.1.0.src-aros.diff`, so a 20.1.0
   crosstools build dies in `crosstools-llvm-lld-fetch`. The patch is *4 lines*
   (add `aarch64elf_aros`/`armelf_aros`/`armelfb_aros` to lld's `-m` emulation
   table in `ELF/Driver.cpp`). (Written on our branch — upstreamable as-is.)

## Dead external dependencies

6. **ACPICA download URL is dead (404), and it gates *everything*.**
   `arch/all-native/acpica` declares `#MM includes-copy : acpica-fetch`, so the
   global `includes-copy` step depends on downloading ACPICA from a now-404 URL —
   blocking even a hosted build that has no use for x86 ACPI. *Fix:* update the
   ACPICA URL/version, and don't make `includes-copy` depend on a native-only,
   network-fetched Port (scope it to native targets).

7. **Other contrib Ports are network-fragile in the toolchain graph.** Building the
   *toolchain* pulls in a broad fetch graph (boost, heif, de265, webp, mesa,
   codesets, freetype, …). Several 404 or have version-sensitive patches; any one
   failing aborts the build. *Fix idea:* the crosstools build shouldn't transitively
   require contrib-datatype sources.

## macOS host specifics

8. **macOS has no `objcopy`.** configure's prereq check wants `objcopy`; stock macOS
   ships only `objdump` (LLVM-based). *Fix:* accept `llvm-objcopy` (or `gobjcopy`)
   automatically on Darwin hosts.

9. **The darwin hosted backend is bit-rotted to ~2010 Xcode.** `configure`'s darwin
   cases use `kernel_tool_prefix="*-apple-darwin10-"` and `iPhoneSimulator`
   `Developer/usr/bin` paths that don't exist on a modern Apple-Silicon Mac. The
   modern path is the native LLVM toolchain (`clang -arch arm64`/`--target=…-aros`,
   `ld.lld`). *Fix:* modernise the darwin cases; this is the bulk of "make hosted
   macOS work again."

10. **AArch64 simply isn't a darwin target yet.** `configure` errors with
    *"Unsupported target CPU for darwin hosted flavour"* — added an `*aarch64*` case
    (native `clang -arch arm64`). Also note the AROS-style target order is
    `<arch>-<cpu>` ⇒ **`darwin-aarch64`**, not `aarch64-darwin` (a foot-gun worth a
    clearer error message).

11. **`struct ExceptionContext` for aarch64 is wrong upstream.**
    `arch/aarch64-all/include/aros/cpucontext.h` is `{ IPTR r[29]; fp; sp; pc; }` —
    mislabels x30, omits SPSR, no FP pointer. The hosted backend's
    `SAVEREGS`/`RESTOREREGS` `CopyMemQuick` straight between it and Darwin's
    `_STRUCT_ARM_THREAD_STATE64`, so it must match that layout. (Fixed on our branch.)

## Building decades-old AROS C with a modern clang

12. **clang 16+ turns four legacy-C warnings into hard errors, breaking the build.**
    `-Wimplicit-function-declaration`, `-Wimplicit-int`, `-Wint-conversion` and
    `-Wincompatible-pointer-types` are errors by default in modern clang; AROS's
    pre-C99 sources hit them immediately (first casualty:
    `compiler/arossupport/createseglist.c` via the `__AROS_SET_FULLJMP` macro). AROS
    even *detects* `-Wno-int-conversion` et al. into `NOWARN_*` (config/compiler.cfg)
    but never applies them to the general target `CFLAGS`. *Fix:* demote these back to
    warnings in the global `CFLAGS` (config/make.cfg.in) — added a `MODERN_CLANG_NOWARN`
    set there. (Fixed on our branch; benefits every target, not just aarch64.)

13. **`_JMPLEN` (jmp_buf size) has no `__aarch64__` case.**
    `compiler/crt/stdc/include/aros/stdc/setjmp.h` defines `_JMPLEN` per-CPU
    (m68k/i386/x86_64/ppc/arm/riscv) but not aarch64, so *any* source pulling in
    `<setjmp.h>` fails with `use of undeclared identifier '_JMPLEN'`. *Fix:* add an
    `__aarch64__` case sized for the AAPCS64 callee-saved set (x19-x30, sp, NEON
    v8-v15). A matching `arch/aarch64-all/stdc/setjmp.s` is still needed at link time.
    (Header fixed on our branch.)

## AArch64 was half-added: arch files exist, dispatch chains forgot it

14. **A whole family of `#if defined(__cpu__)` dispatch chains lack an
    `__aarch64__` case**, even though the per-arch files they select *already exist*
    for aarch64. Each one stops the build with a generic `#error`. Found so far:
    - `compiler/crt/stdc/include/aros/stdc/fenv.h` — never routed to the existing
      `aros/aarch64/fenv.h`; fell through to a stub needing a nonexistent
      `aros/types/fenv_t.h`.
    - `compiler/crt/stdc/math/fpmath.h` — `#error unsupported CPU type`; the
      `aarch64/_fpmath.h` (binary128 long double) already exists, just wasn't selected.
    - `rom/exec/exec_util.h` — `#error unsupported CPU type`; needed an `__aarch64__`
      `PC`/`FP` case (maps to `ExceptionContext.pc`/`.fp`).
    - `arch/all-unix/kernel/kernel_cpu.h` — no `__aarch64__` include of the darwin
      `cpu_aarch64.h` signal glue.
    *Fix:* add the missing `__aarch64__` arm of each chain (all one-liners on our
    branch). *Meta-fix worth proposing:* a single canonical CPU-select header instead
    of this pattern duplicated across a dozen files.

15. **`__AROS_SET_FULLJMP` in `arch/aarch64-all/include/aros/cpu.h` was copy-pasted
    from ARM** — it wrote `0xe51ff004` (`ldr pc,[pc,#-4]`, a 32-bit *ARM* instruction)
    into an aarch64 jump trampoline. It only compiled because of issue #12 (the
    pointer→int assignment warning), and would have executed an illegal opcode at
    runtime. *Fix:* real AArch64 trampoline `ldr x16,#8 ; br x16` + 64-bit target.
    (Fixed on our branch.) The sibling `genmodule.h` (library-call stubs) was missing
    entirely for aarch64 — written fresh from the ARM template.

## macOS-on-Apple-Silicon host specifics

16. **The hosted-darwin backend pulls macOS SDK headers that dispatch on Apple's
    `__arm64__`, which the `aarch64-aros` triple doesn't define** (it defines
    `__aarch64__`). `cc_include/machine/_types.h` and `sys/cdefs.h` then `#error
    architecture not supported`. *Fix:* define `-D__arm64__` for the hosted aarch64
    target (the host genuinely is arm64). Cleaner long-term: don't feed raw SDK
    headers to the AROS target compile (part of modernising the darwin backend, #9).

## Toolchain runtime + link stage (getting exec.library to link)

17. **`crosstools-compiler-rt` cmake breaks with a modern LLVM that ships shared
    dylibs.** On macOS, LLVM 20.1.0 builds `libLLVM.dylib`/`libLTO.dylib`, so
    `LLVMExports.cmake` declares `add_library(LTO SHARED IMPORTED)`. compiler-rt's
    `load_llvm_config()` always `find_package(LLVM)`s, and with
    `-DCMAKE_SYSTEM_NAME=Generic` (no shared-lib support) the SHARED import aborts
    configure. AROS's default LLVM 11 didn't hit this. *Fix idea:* build the
    crosstools LLVM with `-DLLVM_BUILD_LLVM_DYLIB=OFF` (static only), or make the
    compiler-rt cmake tolerate it. (Worked around locally by rewriting the SHARED
    imports to STATIC in the installed exports.)

18. **`AROS_LIBREQ` emits its version marker as a non-weak symbol under modern
    clang, so every object in a module collides.** The marker is declared
    `extern const LONG __aros_libreq_<base> __attribute__((weak))` for *reading*,
    but the *definition* (a block-scope static const, weak+used inside the call
    macro) is emitted by clang on aarch64-aros as a **global-absolute** symbol
    `__aros_libreq_SysBase.0` — `weak` is dropped on block-scope statics. Result:
    `ld.lld: duplicate symbol` across every object in exec.library. All copies are
    identical (the required version), so the safe unblock is
    `-Wl,--allow-multiple-definition`. *Cleaner fix:* emit the marker at file scope
    (where `weak` sticks), or via an explicit `.weak`+`.set` in asm.

19. **AArch64 had no `arch/aarch64-all/exec/` at all.** The generic
    `rom/exec/{stackswap,newstackswap}.c` `#error` by design ("must be replaced in
    $(ARCH)"), and `UseExecstubs=1` needs `execstubs.s`. Wrote AArch64 `stackswap.S`,
    `newstackswap.c`, `execstubs.s` + an `mmakefile.src`, grounded in the arm-all
    templates and the build-generated `aros/aarch64/asm.h` offsets. (On our branch.)

20. **A couple of fixes currently live only in the build dir's `config/make.cfg`**
    (not the shared `make.cfg.in`, since they're darwin-aarch64-specific):
    `-D__arm64__` (item 16) and `-Wl,--allow-multiple-definition` (item 18). They
    want a proper darwin-aarch64 home in `configure`. Also the crosstools `ld.lld`
    symlink pointed at a non-existent Homebrew path; it must point at the patched
    crosstools `ld.lld` (the one that knows `aarch64elf_aros`).

## kernel.resource + the boot bring-up on Apple Silicon

21. **The hosted bootstrap's `mmap`s don't work on Apple Silicon.**
    `arch/all-unix/bootstrap/memory.c` maps AROS RAM as `MAP_SHARED|PROT_EXEC` and
    the low pool with `MAP_32BIT`. On Apple Silicon both fail: a one-shot RWX (or
    SHARED+EXEC) anonymous `mmap` is refused (EACCES) under W^X even with
    `allow-jit`/`allow-unsigned-executable-memory` entitlements, and `MAP_32BIT`
    ENOMEMs because the low 4GB is unavailable. *Fix (on our branch):* on Darwin map
    the pool `RW|MAP_PRIVATE` and drop `MAP_32BIT`; early boot executes from the
    kickstart RO region (RW→`mprotect` RX), not the pool. The loader must be
    code-signed with an executable-memory entitlement. (Executing code *loaded into*
    the pool later — LoadSeg — will need the W^X-aware path the iOS code hints at.)

22. **The bootstrap ELF loader has no AArch64 relocations.**
    `bootstrap/elfloader.c` handled x86/ARM only; an aarch64 kickstart dies on
    `Unknown relocation type 257`. Implemented the full static set (ABS64/32,
    PREL64/32, ADR_PREL_PG_HI21, ADD/LDST_ABS_LO12, CALL26/JUMP26, and MOVW_UABS_*).
    (The R_AARCH64_* constants aren't in AROS `dos/elf.h` either.)

23. **Weak symbols force GOT relocs the simple loader can't do.** AArch64 clang
    accesses weak symbols (e.g. `__aros_libreq_SysBase`) via the GOT even with
    `-fno-pic`, emitting `ADR_GOT_PAGE`/`LD64_GOT_LO12_NC` — and the bootstrap loader
    has no GOT. *Fix:* build the kickstart modules `-mcmodel=large` (absolute
    `movz/movk`, no GOT). Needs a proper darwin-aarch64 home in `configure`.

24. **The freestanding modules call the *weak* StdC library-call stubs, which
    crash before any library is up.** Compiler-emitted `memset`/`strcmp`/`strlen`/…
    in `exec.library`/`kernel.resource`/`hostlib.resource` resolve to AROS's weak
    StdC stubs (`memset` is `W` in `libstdc.a`), which deref a still-NULL `StdCBase`
    → SIGSEGV at the first call. AROS makes them weak so a strong static one can
    override, but `libstdc.static.a`'s strong versions aren't pulled (archive
    semantics: a weak def already "satisfies" the reference). *Fix (on our branch):*
    force-load a static C runtime via `USER_LDFLAGS += -Wl,--whole-archive -lkrnmem
    -Wl,--no-whole-archive`. `libkrnmem.a` is the mem/str/format dependency-closure
    carved out of `libstdc.static.a` — currently assembled by hand in the build dir;
    it needs a proper mmake rule (or the kernel build should link `stdc.static`
    such that strong defs win). *Worth proposing upstream:* a clean "freestanding
    module uses the static C runtime" knob.

25. **AArch64 `setjmp` has no implementation.** `setjmp`/`longjmp` are still the weak
    StdC stubs (no `arch/aarch64-all/stdc/setjmp.s`). Not hit on the early-boot path
    yet, but needed for full exec exception handling.

---

## The CLI-boot push — building the BASE kickstart (toward a shell)

26. **The whole weak-StdC-stub problem, solved globally.** Freestanding kickstart
    modules crashed on the compiler-emitted memset/strcmp/… (weak StdC stubs that
    deref a NULL StdCBase). Root cause subtlety: a *hosted* build compiles every
    module as `compiler=target`, so they link via `LDFLAGS`/`TARGET_LDFLAGS` — not
    `KERNEL_LDFLAGS` (which only applies to *native* bare-metal kickstart builds).
    *Fix:* link `-lstdc.static` early in `LDFLAGS` (make.cfg.in) so the strong
    freestanding functions bind before the spec's weak `-lstdc`; only referenced
    members are pulled, so modules that emit no such calls pay nothing and disk
    programs keep shared printf/malloc. Drops the by-hand libkrnmem.a entirely.

27. **make.cfg regeneration silently dropped the build-dir-only flags** (the cause
    of "we keep re-fixing the build"). Gave them a permanent CPU-gated home in
    make.cfg.in: `-Wl,--allow-multiple-definition` (the clang libreq-marker
    collision, item 18), `-mcmodel=large` (item 23), and `-D__arm64__` — the macOS
    host headers (cc_include) guard their arch dispatch on `__arm64__`, not the
    `__aarch64__` the AROS clang defines, so any file pulling a host header failed
    "architecture not supported".

28. **`aros/types/fenv_t.h` was missing tree-wide.** fenv.c / `<aros/stdc/fenv.h>`
    include it for just the fenv_t/fexcept_t types (no arch inlines). Added it
    dispatching by CPU (`compiler/crt/stdc/include/aros/types/`).

29. **AArch64 had no setjmp/longjmp** (generic longjmp.c `#error`s per-cpu). Added
    `arch/aarch64-all/stdc/{setjmp,longjmp}.s` (AAPCS64 callee-saved + NEON d8-d15).

30. **The runtime ELF loader (LoadSeg) had no aarch64 relocations.**
    `rom/dos/internalloadseg_elf.c` `#error`ed for aarch64; ported the full static
    relocation set (same as the bootstrap loader, item 22).

31. **emul-handler assumed 32-bit inodes.** `unix_hints.h` hardcoded
    `_DARWIN_NO_64_BIT_INODE`; modern macOS has only 64-bit inodes (hard error).
    Gave Darwin its own branch using the native (64-bit) stat.

32. **The translation catalogs are git submodules, not in the main checkout.**
    dos.library's `strings.h`/error catalog comes from `rom/dos/catalogs`
    (github.com/aros-translation-team/dos.library); 74 such submodules exist and
    must be `git submodule update --init`ed for the modules that have them.

33. **`PrepareContext()` was unimplemented for AArch64 — THE cold-start blocker.**
    rom/exec/preparecontext.c is a stub returning FALSE; the real impl is per-cpu in
    `arch/<cpu>-all/exec/`. Without it every NewCreateTask() failed, so exec's first
    service task (Exec_InitServices' "housekeeper") couldn't be created → the boot
    died on AT_DeadEnd|AN_ExecLib. Added the AArch64 PrepareContext (args in x0-x7,
    x29=0, x30=fallBack, 16-byte-aligned sp). The boot now runs *through* exec init.

## C-library correctness (surfaced by the in-tree benchmark suite)

34. **`printf("%f")` printed the format specifier literally — float conversions
    were compiled out of the C library.** AROS's shared format engine
    (`compiler/fmtprintf/fmtprintf.c`) gates `%[aAeEfFgG]` behind
    `#ifdef FULL_SPECIFIERS`. Every consumer defines it unconditionally
    (`__vcscan`, `__vwformat`, `__vwscanf`, the kernel `_vkprintf`) **except
    `__vcformat`**, which gated it on `#ifndef STDC_STATIC`. This is a direct
    consequence of item 26 (linking `-lstdc.static` early): the freestanding
    archive's *strong*, float-less `__vcformat` is pulled to satisfy
    `printf`/`vfprintf` and **shadows** the float-capable `StdCBase` dispatcher in
    `-lstdc_rel`. So `printf`/`vfprintf` from `posixc.library` **and**
    `stdcio.library` (i.e. every C program) rendered `%f`/`%lf`/`%g` verbatim,
    while `sprintf` (via `stdc.library`, which keeps its own float-ON `__vcformat`)
    worked — and integer conversions were unaffected, which made it look like a
    vararg bug rather than a missing case. *Fix (on our branch):* drop the lone
    `#ifndef STDC_STATIC` gate in `compiler/crt/stdc/__vcformat.c` so float support
    is unconditional, matching the sibling engines; the float math it pulls
    (`log10`/`pow`/`isinf`/…) resolves through the weak `StdCBase` library stubs.
    Rebuild `libstdc.static.a` + `posixc.library` + `stdcio.library`. Fixes float
    output system-wide (`printf`, `CPUInfo`-style tools, any app). Verified with
    `developer/debug/test/benchmarks` (exec + clib) — `allocvec`/`allocpooled`/
    `memset` now print real rates instead of `%f`.

35. **[FIXED] The emul-handler process stack was 16 KB — it overflowed under
    host-call load.** First seen as `[KRN] Task EMU went out of stack limits` +
    an unrecoverable trap on the `clib/stdio` benchmark; later reproduced as the
    DoExamineNext bus-fault under Feraille's concurrent metadata walkers.
    Root cause: `emul_init.c` hardcoded `dn_StackSize = 16384` (under half the
    aarch64 `AROS_STACKSIZE` of 40960) while one ExamineNext packet stacks
    several KB of locals (`DoExamineNext` `ep[1024]`, `NameToAros`/`hv_to_nfc`
    ~2 KB, `MetaRead` `path[1024]+buf[1024]`) plus darwin libc frames — and
    hosted interrupt delivery (SIGALRM tick → `core_IRQ` → scheduler) runs on
    the interrupted task's stack (no sigaltstack anywhere). Sustained walkers
    keep the handler current for nearly every tick, so the signal frame lands
    on the deepest frames and SP crosses `tc_SPLower`: `core_Switch` suspends
    the task and the below-stack spill corrupts neighboring memory (the "wild
    NULL-offset faults"). Fix: `EMUL_HANDLER_STACKSIZE` (64 KB) for both the
    EMU boot node and `AROS_HOST_VOLUME` mounts. Stress test:
    `hosted/exwalk` (C:ExWalk, N concurrent ExNext/ExAll walker processes).

36. **[RE-OPENED 2026-07-17 — partially explained, NOT fixed] The recursive-walk
    bus fault.** The posixc fd-table race below was real and is fixed, but it
    did **not** close this item: with the fixed posixc, Feraille's folder-size
    walker still bus-faults under real (human, mouse-driven) use — now
    reported as `Error 0x80000002 ... Module kernel Segment 1 .text Offset
    0x1AA0`, task `C:Feraille` (a *kernel*-module offset, not emul-handler as
    before — the posixc fix may have moved the fault rather than removed it).
    Feraille's walker is re-gated again (Feraille `main`, 2026-07-17). A
    separate hang also reproduces: the AROS exec thread pinned at 100% CPU
    spinning on `sigprocmask` (the hosted interrupt-mask path) while every
    guest task sits in `WAIT` — i.e. a scheduler livelock, no trap.
    **Methodology note for whoever picks this up:** synthetic input
    (`aros-ctl click`) does NOT reproduce either failure — dozens of scripted
    rounds pass while a human freezes it in a few clicks, because injected
    clicks teleport the pointer and generate none of the mouse-move/hover
    traffic real use does. And `crash=none`/`state=running` is NOT a liveness
    check: livelocks and contained Gurus both report clean. Test with a human
    at the mouse, or teach the harness to stream real pointer motion.

    Original (2026-07-16) analysis, still valid as far as it goes: the "second
    emul-handler `DoExamineNext` fault" was
    not purely an emul-handler bug — `posixc.library`'s shared fd table had no
    locking at all.** The tell was the fault offset: `.text+0x37A8` is the
    *first instruction* of `DoExamineNext` (`ldr w8, [x1, #0x10]`, reading
    `fh->type`) — the handler trapped dereferencing the filehandle *argument*
    it was handed in the DOS packet. The garbage `fh` came from the client
    side: pthreads (all Rust std threads) share the opener's `PosixCIntBase`,
    and `__fdesc.c` mutated `fd_array`/`fd_slots` and `internalpool` with zero
    synchronization (`/* TODO: Add locking */` was in the file). Three race
    classes, one per observed symptom:
    - `__getfdslot()` grew the table by `AllocPooled` + `CopyMem` +
      `FreePooled(old)` + pointer swap while other tasks read
      `fd_array[fd]` lock-free → a reader mid-`readdir()` gets a *freed*
      fdesc → garbage BPTR into the `ExNext` packet → emul-handler traps at
      `DoExamineNext+0`. Growth was to exactly `wanted_fd+1`, so under
      concurrent load nearly **every** open at the high-water mark
      realloc-freed the table — maximal race exposure (and O(n²) copying).
      Why ExWalk never reproduced it: single-threaded, one fd, no table churn.
    - Concurrent `AllocPooled`/`FreePooled` on `internalpool` (exec pools are
      not thread-safe) corrupted the pool free lists → wild faults in
      unrelated DOS calls (seen live: Feraille boot trap in `File::write` →
      `Dos_8_Write` → `dopacket` → `Exec_41_AddTail` on a NULL list).
    - `open()`/`opendir()` did find-then-claim (`__getfdslot(__getfirstfd())`)
      non-atomically → two threads claim the same fd; `__getfdslot` even
      `close()`s the occupied slot, killing a sibling thread's live handle
      (the "posixc `__open` bus-fault on a missing path" was this — the
      missing path was incidental, the concurrency was the trigger).
    Depending on what the corruption hit, the symptom was a containment
    requester, a silent deadend reboot, or a wedge — matching every flavor
    Feraille logged. *Fix (aros-upstream `crash-containment`, T-FDLOCK):* a
    `SignalSemaphore fd_sem` in `PosixCIntBase` — `__getfdesc` obtains
    shared; every table/pool mutation obtains exclusive; `__open`/`opendir`/
    `pipe`/`dup`/`dup2`/`fcntl(F_DUPFD)` find+claim atomically under it
    (claiming *before* the blocking DOS I/O so a slow volume can't stall
    other threads); `close()` unhooks the slot before freeing the fdesc
    (was freed-then-cleared); fd table grows geometrically. Verified on
    device: the previously-fatal Feraille repro (tree expand of `SYS:` +
    `SYS:Classes`, which deadend-rebooted the whole OS) plus a rapid
    all-locations navigation stress now run clean, `crash=none`. Remaining
    known-unprotected (pre-existing, lower severity): errno crosstalk via
    the shared `StdCBase`, `__upath` conversion buffer (unused by Rust std,
    which passes AROS paths), and closing an fd while another thread is
    mid-I/O on it.

---

*Status — **AROS boots through exec init; building the BASE kickstart toward a CLI***.
exec/kernel/hostlib + dos.library + ~17 BASE modules (aros, utility, oop, expansion,
emul-handler, dosboot, lddemon, FileSystem, bootloader, timer, battclock, processor,
ram-handler, debug, …) build for darwin-aarch64. With PrepareContext implemented,
task creation works and the boot proceeds past Exec_InitServices into the cold-start
sequence, where it now hits a *new* pre-dispatch trap (X0=0, in a non-kernel.resource
module — needs the bootstrap to print per-module load bases to symbolicate). The
no-display path is the **emul-handler emergency shell** (host stdin/stdout), so a CLI
prompt is reachable without a graphics HIDD. unixio.hidd still needs a host-header
fix (`net/if.h` incomplete structs). Items 1, 5, 6, 10–33 are on the
`aarch64-darwin-graft` branch.

## Surfaced by porting Rust `std` (posixc runtime, hosted darwin-aarch64)

Bringing up Rust's standard library exercised `posixc` the same way the ffmpeg port
did. New issues found (candidate patches / bug reports):

34. **The posixc/network headers pull the macOS SDK on darwin-aarch64.** Including
    `<time.h>`, `<sys/socket.h>`, `<netinet/in.h>` &c from a target program drags in
    the host's `<stdint.h>`/`<sys/types.h>` (Xcode SDK), which is either not found or
    redefines `intmax_t`/`uintmax_t` against AROS's `aros/types/int_t.h`. Both the
    ffmpeg glue and the Rust net glue had to be **header-clean** (declare the few
    structs themselves) to build. *Fix:* the darwin-aarch64 header set should resolve
    the C standard types from AROS/the crosstools, not the macOS SDK (the `__arm64__`
    gating is incomplete here).

35. ~~`setenv()` fails for a loaded `C:` command's process.~~ **Withdrawn — not a
    bug.** Re-tested empirically: `std::env::set_var` (→ `setenv` → `SetVar(...,
    LV_VAR | GVF_LOCAL_ONLY)`) **works** for a loaded command; `SetVar` returns
    DOSTRUE on the LOCAL_ONLY path (`rom/dos/setvar.c:197`) and the value reads back.
    The earlier "-1" was a boot-stall run misattributed to `setenv`. Left here so the
    numbering is stable.

36. **Hosted RTC isn't seeded from the host clock.** `clock_gettime(CLOCK_REALTIME)`
    returns `tv_sec` ≈ 252460808 (~1978), not the host wall-clock — the hosted AROS
    battclock/timer isn't seeded from the macOS host time at boot. `CLOCK_MONOTONIC`
    is fine (uptime). Minor, but every `SystemTime::now()` / `time()` is wrong.

> Not an AROS bug: `clock_gettime` itself works from a C command (rc=0, correct
> `sizeof(timespec)`=16, `sizeof(long)`=8). The *Rust* path faulting on it is the x18
> clobber in the not-yet-`-ffixed-x18` timer/posixc code (the x18 finding in
> [NOTES.md](../NOTES.md); the OS-wide rebuild covers it).

37. **`bsdsocket.library` treats `FIONBIO`/non-blocking as a no-op, so socket
    non-blocking and timeouts aren't honoured.** `arch/all-unix/bsdsocket` keeps the
    host socket `O_NONBLOCK` and emulates blocking with an internal timer-poll park
    (`bsdsocket_sockopt.c`: `IoctlSocket(FIONBIO)` returns success but does nothing).
    So an app that sets `FIONBIO` (or `SO_RCVTIMEO`/`SO_SNDTIMEO`) still blocks. This
    surfaced building Rust `std::net`: blocking TCP is solid, but `set_nonblocking`
    can't take effect and read/write timeouts can't be enforced (the Rust pal returns
    `Unsupported` for a requested timeout rather than silently ignore it). *Fix:* make
    `IoctlSocket(FIONBIO)` actually switch the library's park behaviour per-socket
    (return `EWOULDBLOCK` from `recv`/`send`/`accept`/`connect` when set), and honour
    `SO_RCVTIMEO`/`SO_SNDTIMEO` in the park loop. Needed for mio/tokio later.

---

38. **[FIXED 2026-07-17 — root cause of the "Feraille freezes under use"
    saga] Hosted darwin did not preempt a CPU-bound task.** Isolated with two standalone
    on-device tests (`hosted/timertest`, `hosted/clocktest`) after a long
    chase through the app. `ClockTest` matrix — a `pri -1` spinner thread vs a
    `pri 0` `timer.device` wait on the single guest CPU:

    | spinner | pri 0 timer wait | CPU |
    | --- | --- | --- |
    | `while(!stop){n++}` (no syscalls) | **HANGS** | 100% |
    | `clock_gettime()` loop | **HANGS** | 100% |
    | `sched_yield()` loop | ok | ~4% |
    | `pthread_cond_timedwait(1ms)` loop | ok | ~4% |

    The split is *voluntary yield vs not*, not anything clock- or
    syscall-specific (the pure-arithmetic spinner makes zero syscalls and still
    wedges everything). So a task that never blocks monopolizes the CPU
    forever; the UI task and `timer.device` completion delivery both starve, so
    the software clock (`CLOCK_MONOTONIC`/`GetUpTime`) appears frozen too — a
    live frozen Feraille showed exactly that (app-time pinned while wall time
    advanced minutes). `TimerTest` separately proved every timed-wait primitive
    is correct with a healthy clock, so this is not a primitive bug.

    **Mechanism.** Preemption relies solely on `setitimer(ITIMER_REAL)` →
    **process-directed** `SIGALRM` (`arch/all-unix/timer/timer_init.c`). On
    darwin a process-directed signal goes to an arbitrary thread with it
    unblocked, and the Macaros process has many (cocoa main, NSEventThread,
    Metal, libdispatch). `core_IRQ` then **drops** any `SIGALRM` that lands on
    a non-AROS thread (`arch/all-unix/kernel/kernel.c:~500`, the
    `pthread_self() != aros_host_thread` guard), on the assumption "the next
    tick will hit the AROS thread." Under CPU-bound load that assumption fails:
    the ticks keep missing the busy AROS scheduler thread and get dropped, so
    the spinner is never preempted. There is **no thread-directed tick**
    (no `pthread_kill(aros_host_thread, SIGALRM)`) as a fallback.

    **Fix direction.** Deliver the preemption tick thread-directed to
    `aros_host_thread` — e.g. a small dedicated host timer thread that
    `pthread_kill(aros_host_thread, SIGALRM)`s every VBlank period, instead of
    (or alongside) the process-directed `ITIMER_REAL`. That guarantees the tick
    lands on the thread actually running the guest and preempts a CPU-bound
    task. Verify against `C:ClockTest pure` (must reach `[D]`, not hang).

    **FIXED (aros-upstream `crash-containment`): core_IRQ now FORWARDS a
    mis-delivered tick to AROS's thread with `pthread_kill(aros_host_thread,
    sig)` instead of dropping it** (async-signal-safe; classic signals
    coalesce; a masked target just keeps it pending until the guest's next
    Enable()). Measured before/after with the T-TICKPROBE counters printed in
    the SIGINFO task dump: 40 handled / 7274 dropped under a pure spin ->
    4293 handled / 35 forwarded. All four ClockTest variants now reach [D]
    with the guest clock tracking real time exactly, and the Feraille scroll
    stress that froze the GUI on every run holds 3-10% CPU, responsive.

    NB the many app-side mitigations found along the way are still worth
    keeping (they reduce how often anything spins, and each is an independent
    real bug): sched_yield doing 2x SetTaskPri per call, cond_timedwait opening
    timer.device per call, negative-deadline SendIO, posixc's unlocked fd
    table, the gpui_aros poll loop and crossbeam/parking_lot spin-waits. But
    none of them is the root cause; this is.

39. **posixc `realpath()` can never succeed in AROS-native path mode.** Its
    first act is `open(".", O_RDONLY)` to save the cwd for the later
    `fchdir()` restore — and `.` is not a valid path component to DOS
    (`Lock(".")` → `ERROR_INVALID_COMPONENT_NAME` → EINVAL), so every call
    fails before even looking at its input. Proved on-device by the RS3g
    probe in `hosted/rust/std-probe` (2026-07-18): `open(".")` = -1 EINVAL
    while `chdir("MacRW:")`/`getcwd()` work fine. Fix direction: save the
    cwd without posix `open` — `Lock("", SHARED_LOCK)` + `CurrentDir()` (or
    a `getcwd()` string + final `chdir()`), and translate `.`/`..`
    components before handing paths to DOS. The Rust std pal now sidesteps
    it entirely: `aros_realpath()` in `hosted/rust/aros_fs_glue.c` does
    `Lock()` + `NameFromLock()`, which is both simpler and handler-correct.

40. **emul-handler `CreateDir` with a missing parent returns the wrong
    IoErr.** `mkdir("MacRW:a/b")` with `MacRW:a` absent fails with IoErr
    that maps to EINVAL — and on a freshly booted instance was observed as
    IoErr()=0 (errno 0, "No error") — instead of ERROR_DIR_NOT_FOUND /
    ERROR_OBJECT_NOT_FOUND (ENOENT). Consequence: any recover-on-ENOENT
    recursion (Rust's `fs::create_dir_all`, and the same pattern in other
    ported software) aborts instead of creating the parents. Reproduced by
    the RS3f step prints (std-probe, 2026-07-18): `create_dir_all` =
    Err(EINVAL) while the same two mkdirs done stepwise succeed. Fix: the
    host-errno→IoErr mapping in emul-handler's create path should
    distinguish ENOENT-on-parent (host ENOENT with a nonexistent
    intermediate) and must never leave IoErr()=0 on failure.
