# std for 68k — architecture + build state

> Started 2026-07-02. Goal: `std` Rust programs compiled for m68k, running under
> `run68k` (the 68k JIT + interpreter), verified two-engine.
> **New here → read [HANDOFF-std68k.md](HANDOFF-std68k.md)** (full state + the pickup
> playbook for the one open bug). Companion docs:
> [FIX-PLAN.md](FIX-PLAN.md) (aarch64 pal), [../jit68k/rust68k/](../jit68k/rust68k/README.md)
> (no_std corpus), [../jit68k/rust68k/UPSTREAM-LLVM-CCR-BUG.md](../jit68k/rust68k/UPSTREAM-LLVM-CCR-BUG.md).

## What "std" means here (decided)

std needs an OS. There is no m68k AROS userland to link (checked: `../aros-upstream`
has the m68k SOURCE (`arch/m68k-amiga`, `arch/m68k-all`, posixc) but no built m68k
SDK and no m68k crosstools on this machine; building them is the Route-B/C world).
So the OS surface is OURS: **run68k's stub library grows into a small "stub DOS"**
(the vamos idea, native): fs/time/entropy LVOs backed by host POSIX, and a tiny
**libc68k** (C, compiled by m68k-elf-gcc into the same vlink hunk pipeline) exposes
them as the POSIX-ish symbols the EXISTING aros pal already calls (`open`, `read`,
`write`, `malloc`, `__stdc_geterrnoptr`, `clock_gettime`, `arc4random_buf`, ...).
That way `library/std/src/sys/**/aros.rs` is REUSED nearly as-is; only genuinely
missing subsystems get the std `unsupported` wiring for this target: **thread, TLS,
net, process** (run68k is single-task; spawn/connect return errors at runtime or
compile to the unsupported pal).

`../aros-upstream` contributions: posixc sources as the SEMANTIC REFERENCE
(errno numbering, open flags — same NetBSD-flavored values the aarch64 pal already
encodes), `arch/m68k-all/include` as the ABI ground truth, `tools/elf2hunk` as the
fallback link route (vlink is primary).

## Compiler facts (probed, they shape everything)

- Backend: experimental LLVM M68k in stock nightly; the CCR bug
  (llvm/llvm-project#152816) is OPEN → **opt-level=1 only** and every artifact
  must pass the **two-engine check** (`run68k` vs `run68k --interp`).
- **opt-level=0 is NOT an escape hatch**: rustc SIGSEGVs compiling
  compiler_builtins for m68k at O0 regardless of debuginfo (probed 2026-07-02
  with `-C strip=debuginfo`; worth its own rust-lang issue).
- Atomics: m68k targets declare CAS atomics → LLVM would emit 68020 `cas.l`,
  which the engines don't decode. Our target sets **`"singlethread": true`** so
  LLVM lowers atomics to plain ops — correct in run68k's single-task world.
- data-layout (from `m68k-unknown-none-elf`, must byte-match LLVM):
  `E-m:e-p:32:16:32-i8:8:8-i16:16:16-i32:16:32-n8:16:32-a:0:16-S16`.

## The pieces (build order, each verified before the next)

| # | piece | where | state |
|---|-------|-------|-------|
| 1 | Sandbox growth: 16 MiB, program loads at 0x250000 above the fixed runtime blocks; interp step cap now honours `JIT68K_STEP_CAP` | `run68k.c`, `j5d_interp.c` | **DONE**, corpus green |
| 2 | Stub-DOS LVOs 10..21 (Open/Close/Read/Write/LSeek/Delete/MkDir/RmDir/Stat/FStat/GetTime/Entropy), host-POSIX-backed, args d1/d2/d3, negative-errno returns, 20-byte BE stat record | `apps68k/stublib.{h,c}` | **DONE** |
| 3 | libc68k (malloc free-list @0xC00000, POSIX wrappers, errno, clock_gettime, arc4random_buf, getcwd, argc/argv) + crt0/veneers (lvo.s) + `test_libc.c` smoke | `rust68k/libc68k/` | **DONE**: smoke test green under BOTH engines |
| 3b | Engine gaps gcc/rustc exposed, all fixed: `jsr/jmp (d16,PC)` terminators + EMIT_JSR raw-push sandboxed (host SIGSEGV) in the JIT; ~20 new oracle forms incl. link/unlk, Scc, cmpm, mulu.l-64, a general MOVE fallback | `j5d_engine.c`, `emu68_darwinize.pl`, `j5d_interp.c` | **DONE**, whole [J5*]+apps+rust matrix re-verified |
| 4 | `m68k-unknown-aros.json` (os=aros, singlethread=true — atomics lower to plain ops, no CAS; **code-model=large** — PC16 constant-pool refs cannot span a std-sized image) + pal cfg: thread/net/process/sync-pthread/TLS-key → no_threads/unsupported for m68k | `hosted/rust/`, rust-aros sys mod.rs files | **DONE**: std COMPILES for m68k, zero errors |
| 5 | `std68k-probe` + `tools/link-std68k.sh` (full GNU ld link + `tools/elfexec2hunk.py` single-hunk converter — vlink -mtype crashes on std-sized input and unmerged PC16 can't be represented in hunk) | `rust68k/std68k-probe/` | **links & loads; BLOCKED at runtime by the upstream CCR bug** (below) |

## Where it stands (2026-07-02, memcpy bug fixed): 8/9 std subsystems PASS clean; one fs sub-check left

After extending BOTH engines and finding the second compiler bug, the std probe runs
**clean** (no corruption) and PASSES: hello, Vec/sum=91, HashMap, format!, Instant,
SystemTime, thread::spawn-errors, TcpStream-errors, and **fs remove+gone**. Only
`fs create/seek/read/metadata` still fails.

**The corruption was a SECOND m68k codegen bug: LLVM miscompiles compiler_builtins'
`memcpy`** (it fails to advance the source pointer — copies the first byte across the
whole destination; proven by a 40-byte `memprobe` printing 40 identical bytes, and
IDENTICAL on both engines = a compiler bug, not an engine bug). Every String/CString/
format buffer in std was corrupted, which also broke the file path. **Fix:** provide
`memcpy`/`memmove`/`memset`/`memcmp`/`strlen` in `libc68k.c` (gcc-compiled, correct)
and build std WITHOUT `-Zbuild-std-features=compiler-builtins-mem` (done in
`tools/link-std68k.sh`). Filed upstream as **llvm/llvm-project#207150** (distinct from the CCR bug #152816 — it
reproduces with the CCR fix applied). The reproducer: `copy_from_slice`/`memcpy`
fills the destination with the first source byte; a hand-written index loop is
correct; identical on both engines; a gcc-compiled memcpy on the same engines is
correct. The CCR PR #168485 (dansalvato's) was confirmed working for us (comment
issuecomment-4864366568).

**Remaining (root-caused): a THIRD residual m68k codegen miscompile, in std's
`File::open`/`OpenOptions` path.** Debugged exhaustively (probe instrumentation +
`JIT68K_DOSLOG` LVO trace):
- the `open` LVO returns fd **3** for `File::create` (path + flags `0x242` correct);
- calling libc68k `open`/`write`/`close`/`stat` DIRECTLY from Rust (both non-variadic
  and the exact variadic decl std uses) works — returns 3, writes, etc.;
- a hand-inlined copy of `File::open`'s tail (`let r = if fd<0 {Err(last_os_error())}
  else {Ok(fd)}`) returns `Ok(3)` correctly;
- but the REAL `std::fs::File::create(p)` returns `Err` (its `open` LVO returns 3, no
  post-open LVO, yet the result is Err) — and it fails in isolation too, so it is not
  context-specific.
So it is neither the engines (both agree byte-for-byte) nor libc68k nor the ABI.
**NOT ATTRIBUTED to a specific defect** — disassembling `File::open` shows the obvious
suspect (the `fd < 0` check) is CLEAN: `cmpil #0,%d0` immediately followed by `bmis`,
no flag-clobber between, and the Ok path writes `%d0`(=fd) and tag `0xff` correctly.
So the earlier "residual CCR-class miscompile" guess is unconfirmed. The result-tag /
io::Result<File> layout path is the next thing to check (Ok tag = 0xff, Err tag = 0 in
the emitted struct), but it hasn't been reduced to a minimal reproducer or a pinned
miscompiled instruction. **Deliberately NOT filed upstream** — an unreproducible
"std File::create returns Err under pressure, cause unknown" report would be noise,
especially after two solid ones (#152816, #207150).

**TESTED the fuller PR #168485 (2026-07-02): it does NOT fix fs.** Applied the PR's
real content onto the nightly's LLVM snapshot — dansalvato's exact `copyPhysReg`
rewrite + `M68kSubtarget.h` + the tablegen liveness changes (SR-gets-CCR-subreg was
already present in the snapshot; added the `IsM68000` predicate and removed the
`Defs/Uses [CCR]/[SR]` wrappers around the CCR/SR MOVE classes). Rebuilt LLVM+rustc,
no regression (fib/core correct, casprobe=247, 8/9 std subsystems pass), and
`fs create/seek/read/metadata` **still fails identically**. So fs is a genuinely
SEPARATE third bug — NOT the CCR issue. (The PR and my 119-line minimal port give
byte-identical output; the toolchain was reverted to the minimal port since it's
self-contained and reproduces the same result.) Still not filed: no minimal
reproducer, and the obvious suspect asm (the `fd<0` check) is clean, so I can't point
at a specific defect. To make it fileable: reduce to a minimal `.rs`, or bisect the
exact wrong instruction (next suspect: the `io::Result<File>` tag/layout store path).

### The two engine fixes (persisted)
- **JIT `cas`**: `emu68_darwinize.pl --cas-sandbox` (wired into every LINE0 darwinize
  step) rebases the EA to a host pointer in a separate reg (never clobbering a mapped
  An) + byteswaps .w/.l. Verified: `rust68k/casprobe` (AtomicU32 cmpxchg+fetch_add,
  AtomicU8) = 247 on the JIT.
- **A6 frame-pointer collision**: `jsr (d16,a6)` is an LVO call only when a6==libbase;
  else a normal indirect call (LLVM uses a6 as frame pointer). New `jsr/jmp (d16,An)`
  terminators + dispatcher handlers in `j5d_engine.c` and `j5d_interp.c`.
- **Block limits**: decode guard 256→4000, block cache 256→4096 (LLVM-unrolled code).
- **Interpreter**: cas, CCR moves, byte/word reg-reg + immediate ALU, register-count
  shifts, a general `<op>.<sz> <ea>,Dn` handler, general cmpi — runs the std prefix.

## Superseded: the earlier "shared-C bug" framing

Both CPU engines were extended and now execute std end-to-end:

- **JIT `cas` fixed** (`emu68_darwinize.pl` `--cas-sandbox`, wired into every LINE0
  darwinize step in the Makefile): EMIT_CAS now rebases the EA to a host pointer in
  a separate reg (never clobbering a mapped An) and byteswaps the .w/.l compare/store
  values. Verified: `casprobe.exe` (AtomicU32 compare_exchange + fetch_add + AtomicU8)
  returns **247 on the JIT**.
- **A6 frame-pointer vs library-base collision fixed** (`j5d_engine.c` + `j5d_interp.c`):
  `jsr (d16,a6)` is an LVO library call ONLY when a6 == the library base; otherwise
  it's a normal indirect call through a frame slot (LLVM uses a6 as frame pointer).
  Added `jsr/jmp (d16,An)` general terminators + dispatcher handlers.
- **Block limits raised** for LLVM-unrolled code: the per-block decode guard 256→4000
  (SipHash finalization is a ~360-instruction straight-line block) and the block
  cache 256→4096 blocks (std translates thousands of distinct blocks).

Result: **the JIT runs the WHOLE std68k probe** — hello, Vec/sum=91, HashMap, format!,
Instant, SystemTime, and the unsupported-edge checks (thread::spawn / TcpStream error)
all pass; only the two `fs` checks fail. The interpreter runs the same prefix (it still
lacks a few opcodes like `not.b` for the very end).

**The one remaining bug is in SHARED C, not the engines.** Both engines produce
BYTE-IDENTICAL output including an identical single-byte corruption (a `0x63 'c'` →
`0x00` in a rodata string literal, and similar single-byte zeroing in later lines) and
identical `fs ... FAIL`. Two independent engines agreeing bit-for-bit proves the CPU
translation is correct; the defect is in `libc68k` / `apps68k/stublib.c` / the fs pal —
something an fs operation does writes zero bytes into a rodata region and makes the
File round-trip fail. Note the C-level `libc68k/test_libc.c` fs suite (open/write/lseek/
read/stat) PASSES on both engines, so it is specifically the std fs-pal + allocator
interaction (leading suspects: `read_to_string`/`String` growth via `realloc`, or the
`metadata`→`aros_stat` Attr-layout/size path). This is ordinary host-C debugging with
lldb/asan on run68k, no engine work.

## Earlier state (2026-07-02): compiler-blocked, then interpreter-only

The compiler blocker is GONE. `tools/build-patched-rustc.sh` built toolchain
`m68k-ccr-fixed` (nightly LLVM snapshot + the vendored `tools/m68k-ccr-minimal.patch`
— a surgical port of dansalvato's copyPhysReg CCR-preserve fix: save/restore a live
CCR around register copies, via a free data register, else an address-register
park, else a stack spill with `move.w %ccr,-(sp)` / `(sp)+`). With it: `fib` and
`core` compile with correct loops, and **std compiles, links, and RUNS on the 68k
JIT tooling**. Under `run68k --interp` the probe prints and PASSES:
`hello`, `Vec/iter/sum=91`, `HashMap`, `format!`, `Instant`, `SystemTime`.

Two bounded ENGINE items remain (neither is a compiler or architecture blocker):

1. **JIT `cas` (atomics) needs sandbox rebasing + byteswap.** `singlethread` does
   NOT stop LLVM emitting the 68020 `cas` for `core::sync::atomic` (52 sites in
   std; `max-atomic-width:0` is not an option — it removes the Atomic types that
   std_detect needs). Emu68's `EMIT_CAS` (`emu68/M68k_LINE0.c`) does `ldxrb(ea,..)`
   treating `ea` as a host pointer, so the JIT host-faults. Fix: a darwinize
   `--cas-sandbox` pass that (a) rebases `ea` via the base-adjust reg `x12`
   (`J5D_BASEADJ`) like `j5d_ea_helpers.c` does, and (b) `REV`s the loaded/stored
   values (sandbox is big-endian, registers hold architectural values). The
   interpreter already does `cas` correctly (added here). Until the JIT `cas`
   lands, the JIT can't run std; the interpreter can.
2. **Interpreter std-path bug (output corruption + fs FAIL).** `run68k --interp`
   runs std through several subsystems then drops output bytes (`Instant`→`Insan t`)
   and fails the fs checks. The verified two-engine corpus (`make hosted-jit68k-rust`
   / `-apps` / `-j5m` / `-j5t`) still passes byte-exact, so the ~30 opcodes hand-added
   for std (cas, CCR moves, byte/word reg-reg + immediate ALU, register-count shifts,
   a general `<op>.<sz> <ea>,Dn` handler, general cmpi) don't regress what's checked —
   but the std-only paths are UNVERIFIED because the JIT can't yet cross-check them.
   The corruption starts right after the first fs LVO call, so the likely cause is
   the interpreter's [J3] bridge/stub dispatch not preserving 68k state across an
   LVO call at std scale (not necessarily the ALU adds). Pin it down once the JIT
   `cas` fix lets both engines run std and diff.

Bottom line: **Rust std demonstrably runs on 68k** (compiler fixed, std built, booted,
multiple subsystems passing). A fully clean two-engine `RUST-68K: STD PASS` is gated
on the JIT `cas` fix (item 1), which then also becomes the oracle to fix item 2.
No `hosted-jit68k-std` Makefile row yet — add it only when the run is clean under
both engines, like the other corpus rows.

## Earlier milestone (2026-07-02): the compiler-blocked state, now resolved

Everything through the link works. At RUNTIME the probe faults in
`std::sync::ReentrantLock::lock` on the FIRST `println!`: LLVM schedules a
`move.l` between the `sub.l #3,d1` sentinel check of `std::thread::current::CURRENT`
and its `bcs` — MOVE clobbers the carry, the sentinel (0) gets dereferenced.
That is llvm/llvm-project#152816 again, now proven to block std wholesale
(evidence posted: issue comment 4862576848). No source-level dodge exists at
std's size. The unblock: **`tools/build-patched-rustc.sh`** builds a rustc whose
LLVM carries dansalvato's copyPhysReg workaround (toolchain `m68k-ccr-fixed`),
then `tools/link-std68k.sh m68k-ccr-fixed` rebuilds + runs two-engine. When the
probe passes: add a `hosted-jit68k-std` Makefile row (committed binary, both
engines, output-asserted like the others).

## Honest limits (v1)

- One task: `thread::spawn` fails, Mutex/Condvar are trivially uncontended.
- No net, no process, no env vars (getenv = NotFound) in v1.
- The compiler stays untrusted: any new binary is only believed after the
  two-engine run agrees. The CCR canary tells us when that era ends.
