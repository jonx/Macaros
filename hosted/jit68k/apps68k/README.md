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

## 2. The programs (source `*.s` + assembled `bin/*.exe`)

| program        | exercises                                              | exit | status |
|----------------|--------------------------------------------------------|------|--------|
| `mul.s`        | moveq, add.l Dm,Dn, **subq.l (real flags)**, **bne.s backward branch**, rts | d0=42 | **JIT NOW** |
| `fact.s`       | nested additive loops + `move.l Dm,Dn` + `cmp.l`+`bne` | d0=120 | needs `[J5c]` |
| `arraysum.s`   | `add.l (a0)+,d0` (**post-increment memory**) in a loop, **`lea` + HUNK_RELOC32** | d0=150 | needs `[J5c]` |
| `libcall.s`    | **`jsr -off(a6)`** library calls (AllocMem/PutChar/FreeMem) via the negative-offset LVO ABI | d0=0 | needs `[J5c]` |

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
