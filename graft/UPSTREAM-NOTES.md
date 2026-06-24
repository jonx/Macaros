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

---

*Status:* items 1, 5, 6, 10, 11 already have working fixes on the
`aarch64-darwin-graft` branch of our AROS checkout; the rest are documented for a
proper upstream contribution pass once the toolchain + a first module build are green.
