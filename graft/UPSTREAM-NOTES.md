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

---

*Status — `exec.library` LINKS for darwin-aarch64* (175KB aarch64 AROS ELF). Items
1, 5, 6, 10–19 have working fixes on the `aarch64-darwin-graft` branch; item 20 lists
build-dir-only fixes still needing a permanent home. The first piece of AROS itself is
now built for Apple Silicon — next is the rest of the core (kernel.resource, more
modules) toward a booting hosted image.
