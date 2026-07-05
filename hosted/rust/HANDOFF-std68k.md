# Rust std on m68k — handoff

Everything needed to pick up the project, and a detailed playbook for the **one open
bug**: std's `fs create/seek/read/metadata` sub-check. Written 2026-07-02.

Companion docs: [STD68K-PLAN.md](STD68K-PLAN.md) (architecture + decisions),
[FIX-PLAN.md](FIX-PLAN.md) (aarch64 pal review), [../jit68k/rust68k/README.md](../jit68k/rust68k/README.md)
(no_std corpus), [STD-PORT.md](STD-PORT.md) (aarch64 std port).

---

## 1. TL;DR — where we stand

Goal: compile Rust to Motorola 68000 and run it on Apple Silicon through the in-repo
68k JIT (`run68k`), with a matching interpreter as a cross-check oracle.

| Area | State |
|---|---|
| `no_std` core+alloc | **SOLID.** Committed corpus, `make hosted-jit68k-rust` two-engine green |
| Atomics (cas) on the JIT | **WORKS.** `casprobe` = 247, byte + long CAS, sandbox-rebased + byteswapped |
| Rust `std` | **8 of 9 subsystems pass** on the JIT: hello, Vec, HashMap, `format!`, `Instant`, `SystemTime`, thread-spawn-error, TcpStream-error, `fs remove+gone` |
| `std` open item | **`fs create/seek/read/metadata` FAILS** — a third m68k backend miscompile (see §6) |
| Compiler | Custom toolchain `m68k-ccr-fixed` = stock nightly + a 119-line LLVM patch |

Two upstream LLVM bugs filed, one worked around locally:
- **#152816** — the CCR/`copyPhysReg` miscompile. Fixed by our patch (a backport of PR
  **#168485**). PR confirmed working for us (issue comment 4864366568).
- **#207150** — compiler_builtins `memcpy` miscompile. Worked around by providing
  `mem*` in `libc68k` (gcc-compiled).
- The fs bug (§6) — **not filed** (no minimal reproducer yet).

---

## 2. The toolchain

`m68k-ccr-fixed` is a rustup toolchain; stage1 sysroot lives in `/tmp/rust-m68k`
(~747 MB, Apple-Silicon-specific). It is stock rustc `nightly-2026-06-27` rebuilt with
its bundled LLVM patched by `hosted/jit68k/rust68k/tools/m68k-ccr-minimal.patch` (a
119-line `copyPhysReg` CCR-save/restore backport of PR #168485).

**To reproduce the toolchain from scratch:** `hosted/jit68k/rust68k/tools/build-patched-rustc.sh`.

**To iterate on the LLVM patch** (learned the hard way, all four matter):
1. Edit `/tmp/rust-m68k/src/llvm-project/llvm/lib/Target/M68k/*`.
2. If you touched a `.td`: `rm /tmp/rust-m68k/build/aarch64-apple-darwin/llvm/build/lib/Target/M68k/M68kGen*.inc` to force tablegen regen.
3. `cd /tmp/rust-m68k/build/aarch64-apple-darwin/llvm/build && ninja && ninja install`
   — `ninja install` is **mandatory**; rustc links the *install prefix*, not the build tree.
4. `cd /tmp/rust-m68k && find build/aarch64-apple-darwin/stage1-rustc \( -name '*rustc_llvm*' -o -name 'rustc-main*' -o -name 'librustc_driver*' \) -delete`
   then `LZMA_API_STATIC=1 ./x build library --stage 1` (the env var dodges an
   Intel-Homebrew liblzma leak; deleting the artifacts forces the driver relink).
- Git gotcha: after `git checkout FETCH_HEAD -- <file>` (which *stages* it), use
  `git checkout HEAD -- <file>` to revert — plain `git checkout -- <file>` restores the
  staged version, not HEAD.

The pinned envelope: **opt-level=1** (O0 SIGSEGVs rustc on m68k; O3 miscompiles more).
The target spec is `hosted/rust/m68k-unknown-aros.json` (`os=aros`, `singlethread`,
`code-model=large`, `max-atomic-width=32`). Custom-target requires `-Zbuild-std`.

---

## 3. Build + run + test (reproduction)

From the repo root `.`:

```sh
# The JIT/interpreter tool (rebuild after any engine change):
make run68k                       # -> build/run68k

# no_std corpus, two-engine regression (JIT vs interpreter, byte-exact):
make hosted-jit68k-rust           # marker [rust68k] PASS
build/run68k          hosted/jit68k/rust68k/bin/hello.exe   # prints; exit = program's D0
build/run68k --interp hosted/jit68k/rust68k/bin/hello.exe   # same via the oracle

# Atomic (cas) probe — proves the JIT cas fix:
build/run68k hosted/jit68k/rust68k/casprobe/casprobe.exe    # exit 247

# std probe: rebuild from source with the patched toolchain, link, run BOTH engines:
bash hosted/jit68k/rust68k/tools/link-std68k.sh m68k-ccr-fixed
#   -> compiles std (-Zbuild-std=std, NO compiler-builtins-mem), links via
#      m68k-elf-ld + tools/elfexec2hunk.py, runs std68k.exe on JIT then --interp
```

std output today ends with `RUST-68K: STD FAIL (+)` because of the one fs sub-check.

Useful env vars on `run68k`:
- `JIT68K_STEP_CAP=<n>` — raise the runaway-instruction cap (std needs billions).
- `JIT68K_DOSLOG=1` — trace every stub-DOS LVO call (Open/Read/Write/Close/LSeek/Stat)
  to stderr. **This is the primary fs-debug tool** (see §6).
- `--interp` — run under the reference interpreter instead of the JIT.
- `--crash-dir DIR` — where crash bundles go (flight recorder + registers + repro cmd).

---

## 4. How the std bring-up works (mental model)

There is no m68k AROS userland to link against, so the OS surface is **ours**:

```
Rust std  --calls-->  aros pal (library/std/src/sys/**/aros.rs, in ../rust-aros)
          --which calls C symbols-->  libc68k (hosted/jit68k/rust68k/libc68k/, gcc-compiled)
          --which calls-->  stub-DOS LVOs (hosted/jit68k/apps68k/stublib.c, host-POSIX-backed)
          --run by-->  run68k JIT / interpreter (hosted/jit68k/)
```

- `libc68k` provides `open/read/write/lseek/close/stat/malloc/clock_gettime/...` plus
  the `mem*`/`strlen` intrinsics (gcc, to dodge #207150). `crt0`/veneers in `lvo.s`.
- `stublib.c` implements LVOs 5/33/35 (PutChar/AllocMem/FreeMem) and 10..21 (the
  stub-DOS fs/time/entropy set), backed by real host syscalls, args in d1/d2/d3,
  negative-errno returns.
- The aros pal in `rust-aros` is reused from the aarch64 port; m68k routes thread/net/
  process/sync/TLS to `unsupported` (run68k is single-task). **Not pushed upstream.**

---

## 5. Two-engine discipline (why it matters)

`run68k` (Emu68-based JIT) and `run68k --interp` (a hand-written reference interpreter)
are independent implementations. **Byte-identical output on both = the CPU translation
is correct.** This is how we distinguished compiler bugs (both engines agree on the
wrong result → it's the generated code) from engine bugs (engines diverge). Keep using
it: any new finding should be checked on both engines before blaming the compiler.

---

## 6. THE OPEN BUG — `fs create/seek/read/metadata` (the pickup)

### Symptom
`std::fs::File::create(p)?` returns `Err` even though the underlying `open` LVO returns
a valid fd (3). So the fs round-trip closure bails at the first line; the check fails.
Everything else in std works.

### What is RULED OUT (do not re-investigate)
- **Not the engines.** JIT and interpreter produce byte-identical behavior.
- **Not libc68k / the ABI.** Calling `open`/`write`/`close`/`stat` **directly** from
  Rust (both a plain `extern fn open(..)->i32` and the exact variadic
  `fn open(.., ...) -> i32` std uses) works: returns 3, writes 9 bytes, etc. Verified
  with an in-probe ABI test.
- **Not the `fd < 0` check.** Disassembling the pal `File::open`, the check is clean:
  `cmpil #0,%d0` immediately followed by `bmis`, no flag-clobber between, and the Ok
  path writes the fd and a `0xff` tag correctly. (So it is NOT the CCR bug.)
- **Not the CCR fix being incomplete.** Applied the *full* PR #168485 (copyPhysReg
  rewrite + the tablegen liveness changes: `IsM68000` predicate + removing the
  `Defs/Uses [CCR]/[SR]` wrappers around the CCR/SR MOVE classes; SR-subreg was already
  present). Rebuilt, no regression — **fs still fails identically.** It is a *separate*
  third bug.
- **Not a minimal Rust pattern.** A hand-inlined copy of `File::open`'s tail
  (`let r = if fd<0 {Err(last_os_error())} else {Ok(fd)}`) returns `Ok(3)` correctly.

### What is KNOWN
- `File::create` fails **in isolation** too (a bare `File::create(p).is_ok()` prints
  false), so it is not context/pressure from the rest of the probe.
- Instrumenting it is treacherous: adding `{:?}` Debug formatting or an `ErrorKind`
  match to inspect the error triggers a *different* crash (bus error) — the classic
  placement-luck signature of the m68k backend's fragility. Use **lightweight markers
  only** (plain `println!` string literals, or `putchar`-style output), never Debug.
- The failing path is the generic `OpenOptions` → pal `File::open` glue that the
  minimal replicas don't exercise. The pal writes the `io::Result<File>` as a tagged
  struct: **Ok tag = `0xff`, Err tag = `0`, value at offset 2** (fd or packed errno).

### The concrete next step (highest-value first)
The `fd<0` branch is clean, so the prime remaining suspect is the **`io::Result<File>`
discriminant / layout store or the caller's read of it**. Plan:

1. **Reproduce + trace.** Rebuild the probe (`link-std68k.sh`), run with `JIT68K_DOSLOG=1`.
   Confirm: `Open(...create...) -> 3`, then no `Write fd=3`, then the FAIL line. That is
   the current baseline.
2. **Disassemble the caller.** In a fresh link
   (`m68k-elf-ld -q --gc-sections ... -o /tmp/s.elf`), find `File::create` /
   `OpenOptions::open` and the code that reads the `Result` returned by the pal
   `..._3sys2fs4aros...File4open`. Check the discriminant read: does the caller test the
   tag against `0xff` (Ok) the way the pal writes it? Look for a flag-killing copy
   between the tag load and its branch, or a wrong tag constant, or a niche mismatch.
   The pal `File::open` is at symbol `_RNvMs...3sys2fs4arosNtB5_4File4open`; the Ok/Err
   tag store is near its `jsr <open>` (`cmpil #0,%d0; bmis`; Ok path `moveb #-1,%a2@`,
   Err path `moveb #0,%a2@`).
3. **Bisect to a minimal `.rs`.** Try to reproduce with the *smallest* std call that
   returns `io::Result<File>` or `io::Result<T>` through a generic wrapper. Candidate
   reductions: a `#[inline(never)]` fn returning `io::Result<File>` from a raw fd; an
   `OpenOptions::new().write(true).create(true).open(path)` alone. Vary opt-level and
   codegen-units (placement luck — it may flip). A standalone `.rs` that reproduces is
   the artifact upstream needs.
4. **If you get a minimal repro or a pinned instruction → file it** on
   `llvm/llvm-project` (label `backend:m68k`), cross-referencing #152816 and #207150,
   and ping the active maintainers (glaubitz, dansalvato). **If you cannot reduce it,
   do NOT file** — an unreproducible report is noise. This is the standard we held for
   the two we did file.
5. **Alternative unblock (pragmatic):** provide the fs path a non-generic entry that
   dodges the miscompiled glue — e.g. a small `libc68k`/pal shim that opens+writes+reads
   via direct C calls (which are proven to work) rather than `File`. That would make the
   subsystem pass without waiting on the backend, at the cost of not exercising std's
   real `File` path.

### The instrumentation recipe (for step 1–2)
Edit `hosted/jit68k/rust68k/std68k-probe/src/lib.rs`. There is already an example
pattern (now removed but in git history / this doc): an `unsafe extern "C"` block
declaring `open/write/close`, a direct call, and lightweight `println!` markers. Keep
markers literal; never format the error with `{:?}`.

---

## 7. Repo map

```
hosted/jit68k/
  run68k.c / .md            the CLI: loads a hunk, runs JIT or --interp, exit = D0
  j5d_engine.c              the JIT block translator + dispatcher (a6/frame fix,
                            jsr/jmp (d16,An) terminators, guard=4000, cache=4096 here)
  j5d_interp.c              the reference interpreter (cas, CCR moves, general ALU/MOVE)
  emu68_darwinize.pl        rewrites vendored Emu68 for the sandbox; --cas-sandbox here
  apps68k/stublib.{c,h}     stub-DOS LVOs (fs/time/entropy) + [J3] marshaller dispatch
  apps68k/.toolchain/       vlink/vasm/vbcc (built from source)
  rust68k/
    bin/*.exe               committed no_std corpus (fib, vecsum, hello, ...)
    README.md               the no_std pipeline + the CCR canary
    UPSTREAM-LLVM-CCR-BUG.md #152816 analysis
    libc68k/                the m68k C runtime (libc68k.c, lvo.s, test_libc.c)
    std68k-probe/           the std probe crate + std68k.exe
    casprobe/ memprobe/     atomic + memcpy probes (memprobe reproduces #207150)
    tools/
      build-patched-rustc.sh   builds the m68k-ccr-fixed toolchain
      m68k-ccr-minimal.patch   the 119-line CCR backport (vendored)
      link-std68k.sh           builds+links+runs the std probe (both engines)
      build-rust68k.sh         builds the no_std corpus
      elfexec2hunk.py          m68k ELF -> single-hunk AmigaOS executable
hosted/rust/
  m68k-unknown-aros.json    the Rust target spec
  STD68K-PLAN.md            architecture, decisions, full status
  HANDOFF-std68k.md         this file
  FIX-PLAN.md / STD-PORT.md aarch64 pal review / aarch64 std port
../rust-aros   the Rust clone with the aros pal (NOT pushed);
                              m68k routing edits in library/std/src/sys/**/mod.rs
/tmp/rust-m68k                the patched-LLVM + stage1 rustc build (the toolchain)
```

Nothing is committed — it is all in the working tree. `casprobe`/`memprobe` are scratch
crates worth keeping (regression + the #207150 repro).

---

## 8. If you only do one thing

Run `bash hosted/jit68k/rust68k/tools/link-std68k.sh m68k-ccr-fixed`, confirm 8/9 pass,
then attack §6 step 2 (disassemble the `io::Result<File>` tag path in the `File::open`
caller). That is the whole remaining puzzle.
