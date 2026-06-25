# apps68k — testing the 68k JIT with REAL small 68k Amiga programs

This directory stands up the ability to test the 68k→AArch64 JIT (`hosted/jit68k/`,
markers `[J1]`–`[J5b]`) with **real, vasm-assembled, big-endian AmigaOS hunk
executables** — not hand-assembled byte arrays. It contains the toolchain, a couple
of real programs, a stub library environment, and a runner.

Run it: `make hosted-jit68k-apps` (from the repo root). Marker: `[apps68k] PASS`.

## 1. Toolchain — what produces hunk binaries on this Mac

No `vasm`/`vbcc`/`m68k-amigaos-gcc` is installed, and there is **no brew formula**
for vasm. We build **vasm** (Volker Barthelmann / Frank Wille) **from source** — it
is small, dependency-free C, and emits real Amiga hunk executables:

```
tools/build-vasm.sh     # fetch + build vasmm68k_mot into .toolchain/ (not committed)
tools/assemble.sh       # (re)assemble *.s -> bin/*.exe with the built vasm
```

`vasmm68k_mot -Fhunkexe -nosym` produces a `HUNK_HEADER`…`HUNK_END` big-endian
executable that the **existing `[J4]` loader reads unmodified** (it already masks the
`acrx` advisory flag bits vasm sets). The committed artifacts are the small `*.exe`
hunk binaries in `bin/`; the 650 KB vasm binary is gitignored and rebuilt on demand.

Verified on this Mac (macOS 26.x, Apple clang): vasm **2.0e** builds with warnings
only (in vasm's own code), no errors. vasm license: free for non-commercial use and
for use/distribution in or with vasm (Barthelmann/Wille) — built from source, not
modified.

### A C cross-compiler too (`[J5m]`): vbcc + vlink from source

For `[J5m]` — running **compiler-generated** 68k code through the JIT — we also build
a full C cross-toolchain from source on this Mac:

```
tools/build-vbcc.sh     # fetch + build vbcc (C compiler) + vlink (linker) into .toolchain/
tools/compile-j5m.sh    # vbcc -> vasm -> vlink : j5m.c (+ crt0.s) -> bin/j5m.exe
```

- **vbcc** (Volker Barthelmann, V0.9i / m68k code-gen V1.15) — the portable C compiler,
  same author as vasm, targeting **m68k/AmigaOS**. The one build subtlety: vbcc's
  `dtgen` asks "Are you building a cross-compiler?" interactively; per vbcc's own
  `doc/interface.texi` the answer is **`n`** when a native host compiler (clang) builds
  a vbcc that runs on the host (host-native integer types — correct for integer code;
  the program is integer-only so host-endian FP folding never applies). The script pipes
  `n` to keep the build non-interactive.
- **vlink** (Frank Wille, V0.18a) — the linker, emits the `-bamigahunk` executable.
- The pipeline: `vbcc (C → vasm-mot asm) → vasm (asm → vobj) → vlink (vobj → hunk .exe)`.
- Licenses: vbcc is the same family as vasm (free for non-commercial + an explicit
  **commercial exception for M68k/AmigaOS targets** — exactly our use); vlink is freeware.
  Both are TOOLS (compiler/linker), not emulators — no GPL/MPL emulator source involved.
  License text is captured in `.toolchain/{VBCC,VLINK}-LICENSE.txt`. Binaries are
  gitignored under `.toolchain/`; only the small `bin/j5m.exe` is committed.

## 2. The programs (source `*.s` + assembled `bin/*.exe`)

| program        | exercises                                              | exit | status |
|----------------|--------------------------------------------------------|------|--------|
| `mul.s`        | moveq, add.l Dm,Dn, **subq.l (real flags)**, **bne.s backward branch**, rts | d0=42 | **JIT NOW** |
| `fact.s`       | nested additive loops + `move.l Dm,Dn` + `cmp.l`+`bne` | d0=120 | needs `[J5c]` |
| `arraysum.s`   | `add.l (a0)+,d0` (**post-increment memory**) in a loop, **`lea` + HUNK_RELOC32** | d0=150 | needs `[J5c]` |
| `libcall.s`    | **`jsr -off(a6)`** library calls (AllocMem/PutChar/FreeMem) via the negative-offset LVO ABI | d0=0 | needs `[J5c]` |
| `sumsq.s`      | **nested `bsr`/`jsr`/`rts` over a REAL return stack** + a **computed `jsr (a0)`** + a `cmp.l`/`bne.s` loop (a `square` subroutine nesting a `mul` helper, called from a loop) | d0=55 | **JIT `[J5f]`** |
| `mandel.s`     | **the `[J5j]` CAPSTONE** — fixed-point Mandelbrot ASCII renderer: three nested loops, `muls.w` Q11 fixed point + `asr.l` shifts, `add`/`sub`/`cmp` reg+**`#imm`**, `Bcc`, **`(d16,a5)` displacement EA** (load+store), and a `PutChar` per cell via `jsr -off(a6)`. Prints the recognisable fractal; the 1690-byte output stream + regs + memory are byte-exact vs the oracle | d0=0 | **JIT `[J5j]`** |
| `j5m.c`+`crt0.s` | **the `[J5m]` MILESTONE — REAL C COMPILED by vbcc** (not hand-asm): iterative+recursive Fibonacci, factorial table, bubble sort, integer printing, a 32-bit checksum. The compiler lowers it to the full m68k convention — `movem.l` prologues, stack frames (`suba.w`/`(d,a7)`), `jsr`/`bsr`, `Bcc`, `pea`/`lea`, indexed `(d8,An,Xn)` array EAs, byte/word mem, 68020 `mulu.l`/`divu.l`/`divul.l`/`divsl.l`. Run through the JIT, byte-exact (regs+memory+the 235-byte PutChar stream+d0) vs the oracle; output `fib…/fact…/sorted…/checksum=…` | d0=13281 | **JIT `[J5m]`** (`make hosted-jit68k-j5m`) |

`mul.exe` stays entirely inside the opcode subset the `[J5b]` single-block decoder
handles, so the runner **translates it to AArch64 and runs it under W^X TODAY**,
value-asserting `d0==42` against the independent reference. That is a genuine
native-app-scale JIT pass.

## 3. The stub library environment (`stublib.[ch]`)

A minimal fake `exec`-like library inside the sandbox:

- a **negative-offset JumpVec table** built downward from the library base, with the
  68k-ABI stride **6** (`0x4EF9 <sentinel>` per vector), grounded byte-for-byte on
  `arch/m68k-all/include/aros/cpu.h` (`__AROS_GETJUMPVEC`, `LIB_VECTSIZE==6`) — the
  same contract the `[J3]` bridge uses;
- native recording stubs for **AllocMem** (`d0=size,d1=flags -> d0=ptr`, bump
  allocator over a sandbox heap), **FreeMem** (`a1=block,d0=size`), and **PutChar**
  (an observable "print" sink). Each call is logged with its exact args + return, so
  a library-calling program's behaviour is observable and assertable.

Dispatch goes through the **real `[J3]` marshaller** (`j3_build_marshal_thunk`): the
68k registers are marshalled into AAPCS64 and the native stub is `blr`'d via a thunk
emitted into a MAP_JIT region — exactly the production bridge.

## 4. The runner (`runner.c`)

Loads each program into the `[J4]` sandbox and reports, per program:

- **`[JIT NOW]`** — `mul.exe`: real `[J5b]` translate→emit→run, asserted vs. the
  reference.
- **`[NEEDS J5c]`** — `fact`/`arraysum`/`libcall`: the current single-block decoder
  **rejects** them (the runner prints the exact opcode that blocks the JIT — it does
  **not** fake a pass), and their expected results + observable effects are produced
  by an **independent reference** (`refcpu.c`) over the **same sandbox** (so the
  loader's relocation and the memory loads are real, and `libcall` genuinely drives
  the `[J3]` bridge into the recording stubs).

## What a realistic "native app" test looks like at this stage, and the gap

These are **real hunk programs at a TESTABLE scale** + a stub OS environment — not
full Workbench applications. A full app needs:

1. the **broad decoder** (`[J5c]`): the rest of `move`, the `(d16,An)`/`(d8,An,Xn)`/
   absolute/PC-relative EA modes, post-increment/decrement, `cmp`/full `Bcc`/`DBcc`,
   `mulu`/`muls`, reg-to-reg `move`, **and `jsr`-through-vector decoded from the
   instruction stream** wired to the `[J3]` recognition math + sandbox-pointer arg
   marshalling — plus cross-region block chaining and our own register allocator;
2. the **real AROS library environment** (boot-gated), not the stub here.

Each program above names exactly which `[J5c]` capability it needs to flip from
"reference-confirmed" to a true JIT pass. When `[J5c]` lands, the same `bin/*.exe`
binaries become the regression corpus — the toolchain and runner already produce and
load them.
