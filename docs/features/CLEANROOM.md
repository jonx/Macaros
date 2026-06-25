# Clean-room process — how we use GPL references without contaminating AROS

> Drafted 2026-06-24 · Applies to every `spec.md` under `docs/features/*/`

## Why this exists

The best worked examples of "bridge an Amiga-like to macOS" live in GPL-2.0 projects
— the **UAE family** (WinUAE, FS-UAE, Amiberry, E-UAE/Janus-UAE) and **vAmiga**. AROS
is **APL / LGPL-2.1**. We cannot import, paraphrase, or structurally mirror GPL code
into AROS: that would impose GPL on those files and is a licence violation if we then
ship under AROS terms. But we *can* learn from them — legally — through a clean-room
(Chinese-wall) split. This doc is the firewall.

> Not legal advice. This is an engineering process to minimise copyright risk; the
> project owner confirms the licence strategy before anything ships.

## The two roles

**Role A — Spec Author ("sees everything").** May read the GPL references, the public
API docs, the AROS tree, and our own spikes. Produces a **functional specification**:
interface contracts, required observable behaviour, algorithms described as
*requirements in our own words*, and acceptance tests. Records **provenance** for every
requirement. Writes the `spec.md`. *(This is the role the reading-agent plays.)*

**Role B — Implementer ("never saw the GPL code").** Has **not** read UAE/vAmiga/any
GPL reference. Implements **solely** from (1) Role A's `spec.md` and (2) the approved
public sources below. Produces the AROS-licensed code. *(A fresh agent with no GPL
context in its window.)*

The wall is between B and the GPL code. A's spec is the only thing that crosses it, and
a spec carries *facts and contracts*, never *expression*.

## What a spec MAY contain

- **External interface signatures** — Apple framework APIs (AppKit/Metal/CoreAudio/…),
  AROS in-tree headers (APL/LGPL — already ours to use), POSIX. These are interfaces,
  not the GPL code's expression.
- **Data layouts dictated by an external standard** — IFF FTXT, AmigaOS hunk, `fd_set`,
  the AHI sub-driver struct. Mandated by the format/API, not authored by UAE.
- **Algorithms as requirements**, in our own prose or neutral pseudocode — *what* must
  happen and *why*, derived from an independent constraint.
- **Acceptance tests** — the unattended-loop markers and pixel/byte/RMS asserts.
- **Provenance tags** (below) on every non-trivial requirement.

## What a spec MUST NOT contain

- Verbatim or paraphrased GPL source, or its comments.
- A GPL file's identifier names, call sequence, or file/function layout used as a
  template. (Names dictated by an *external* API — `nextDrawable`, `WaitSelect` — are
  fine; names that are the GPL author's invention are not.)
- Any requirement whose *only* justification is "because UAE does it that way." If a
  behaviour can't be re-derived from a public source or our own constraints, it doesn't
  belong in the spec.

## Provenance tags (use them in every spec)

Tag each substantive requirement so Role B knows its footing:

- **`[PUB]`** — public API / published standard (Apple docs, POSIX, the IFF spec). Free
  to implement directly.
- **`[AROS]`** — an in-tree AROS header/driver (APL/LGPL). Cite the path; ours to use.
- **`[OURS]`** — this project's own spikes (`hosted/*`, `graft/*`, the H-series).
- **`[REF-CONFIRM]`** — a GPL reference *confirmed the approach is viable or surfaced
  that a problem exists*, but the requirement is restated here with an independent
  `[PUB]`/`[AROS]`/`[OURS]` justification. Role B implements from that justification,
  **not** from the reference. If you can't attach an independent justification, cut it.

Role B may rely on `[PUB]`/`[AROS]`/`[OURS]` freely. `[REF-CONFIRM]` items must each
stand on their independent justification alone.

## Licence map (what's clean-room vs. adoptable)

| Source | Licence | How we may use it |
|--------|---------|-------------------|
| UAE family (WinUAE/FS-UAE/Amiberry/E-UAE) | GPL-2.0 | **Clean-room only.** Role A reads; Role B never does. Reference for *shape*, never code. |
| vAmiga | GPL-2.0 | **Clean-room only.** Same. |
| **Emu68** | **MPL-2.0** | **Adoptable as isolated files** (file-level copyleft): keep its files verbatim with their MPL header in a quarantined dir, don't intermix with AROS source. Clean-room *not* required, but a clean integration boundary is. See `68k-jit/design.md` `[J0]`. |
| AROS tree | APL / LGPL-2.1 | Ours. Cite and build on directly. |
| Apple frameworks | proprietary, public API | Link against; implement to the published interface. |

## Spec status

| Spec | Feature | Clean-room driver (Role A reads) | Status |
|------|---------|----------------------------------|--------|
| [cocoa-metal-display/spec.md](cocoa-metal-display/spec.md) | Metal display | vAmiga (Metal present path) | **`[D1]`/`[D2]`/`[D]` host shim GREEN** (`hosted/cocoametal/`, review-fixed: render-pass present, scalable, shader stage) · AROS binding next |
| [bsdsocket-net/spec.md](bsdsocket-net/spec.md) | Socket WaitSelect↔Signal pump | UAE `bsdsocket.cpp` pump | **`[N]` host pump GREEN** (`hosted/bsdsocket/`, review-fixed) · AROS binding next |
| [coreaudio-audio/spec.md](coreaudio-audio/spec.md) | CoreAudio SPSC ring + AHI sub-driver | FS-UAE/WinUAE audio (ring sizing + RT hand-off shape only) | **`[A]` host ring GREEN** (`hosted/coreaudio/`) · AROS AHI sub-driver next |
| [clipboard-bridge/spec.md](clipboard-bridge/spec.md) | IFF-FTXT + transcode + changeCount | WinUAE/Amiberry clip | **`[C]` host shim GREEN** (`hosted/clipboard/`, review-fixed) · AROS sync task next |
| [host-volume/spec.md](host-volume/spec.md) | Sidecar metadata + normalization | UAE directory-HD | **`[V]` host glue GREEN** (`hosted/hostvolume/`, review-fixed) · emul-handler overlay next |
| [68k-jit/spec.md](68k-jit/spec.md) | translator integration | Emu68 (MPL — adopted, not clean-roomed) | **`[J0]`→adapt Emu68 · `[J1]` MAP_JIT exec-memory GREEN · `[J2]` Emu68 emitter hosted GREEN · `[J3]` 68k→native LVO-call bridge GREEN · `[J4]` load→relocate→run chain GREEN** (`hosted/jit68k/`, real big-endian hunk binary parsed by OUR minimal loader + `HUNK_RELOC32` relocator grounded against `doshunks.h` + `internalloadseg_aos.c:292-332` — BE32-read/add-target-base/BE32-write — into a 32-bit sandbox; entry translated via the adopted Emu68 emitter + run under MAP_JIT; relocated pointer byte-exact + `d0` value-asserted incl. a skip-reloc negative control; AAPCS64 callee-saved x19..x26 preserve fix; quarantine `emu68/` + NOTICE) · **`[J5a]` memory load/store + sandbox-pointer boundary GREEN** (`hosted/jit68k/j5a_*`, block `move.l (a0),d0 ; addq #1,d0 ; move.l d0,(a0) ; rts` translated via the adopted Emu68 *emitter* with a HAND-ROLLED EA path: `An`→host `host_mem−origin + An` UXTW, unsigned bounds-check `(An−origin)>u(size−4)` → clean fault on OOB, big-endian `REV`; registers AND sandbox memory byte-exact vs an independent interpreter; skip-store / wrong-endianness / out-of-range negative controls all bite. **RA/EA adoption finding: Emu68's `M68k_EA.c` + `RegisterAllocator64.c` do NOT lift incrementally** — `An`-is-host-pointer 1:1-MMU (`M68k_EA.c:635-639`), ext words via the `ICACHE` software cache (`:760+`), SR/CTX in EL0 system registers (`RegisterAllocator64.c:175-185,288-303`); so the register allocator is OURS around the emitter, and **no new Emu68 file vendored** — Exhibit-B unchanged/clean) · **`[J5b]` control flow — a real loop with a conditional backward branch + genuine condition codes GREEN** (`hosted/jit68k/j5b_*`, loop `moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ; subq.l #1,d1 ; bne.s L ; rts` → `d0=15` over 5 iterations, `d1=0`, CCR=0x04 Z-set, translated via the adopted Emu68 *emitter* into a SINGLE `jit_region` with an INTERNAL backward branch. **Real CCR:** `subq.l` → `subs` (flag-setting); the `bne.s` is an AArch64 `b.ne` (Z==0) reading the live NZCV straight from the `subs`; the full 68k N/Z/V/C/X is also recomputed into the CCR with non-flag-setting ops — `cset`/`orr`/`str` — emitted between the `subs` and the `b.ne` so adjacency holds (68k subtract C = AArch64 carry-CLEAR borrow; a 0−1 borrow cross-check asserts CCR=0x19=N\|C\|X). Single-region backward branch: loop-top is a recorded output-word index, the `b.ne` offset is negative; registers + full CCR + iteration count + termination byte-exact vs an independent interpreter; a broken-branch always-taken negative control hangs and is caught by a forked-child watchdog. **No new Emu68 file vendored — only existing `A64.h` encoders — Exhibit-B unchanged/clean**) · **`[J5c]` RE-HOST Emu68's REAL decoder + register allocator GREEN** (`hosted/jit68k/j5c_*`, the make-or-break experiment: DRIVE Emu68's REAL per-opcode decoders — vendored verbatim `emu68/M68k_LINE{8,9,B,C,D}.c` + `M68k_MOVE.c` + `M68k_MULDIV.c` + `M68k_EA.c` — behind hosted replacements for the three `[J5a]` couplings, to translate a richer block `moveq #-5,d2 ; add.l d2,d0 ; sub.l d3,d0 ; and.l d4,d0 ; or.l d5,d0 ; eor.l d6,d1 ; muls.w d7,d1 ; cmp.l d1,d0 ; and.l d2,d2 ; rts` — 8 opcodes across 6 real LINE decoders + REAL `moveq` sign-extend (`d2=0xFFFFFFFB`) + REAL condition-code derivation, all `d0..d7`/CCR byte-exact vs an independent interpreter, corrupt-decode negative control bites. **HOOK 2** `cache_read_16` = big-endian fetch from the host stream (no `cache.c`; splices back the 32-bit-truncated host pointer — a deeper fetch coupling). **HOOK 3** OUR `j5c_ra.c` register allocator implementing the `RA_*` interface with a MEMORY-BACKED CCR (`ldr/str` from the state struct, NOT `mrs/msr TPIDR_EL0`). **HOOK 1** sandbox/EA: register-direct opcodes drive the REAL EA cleanly; the memory-EA modes stay blocked. **VERDICT: re-hosting WORKS for the register/ALU/control class** — broad coverage of it = vendor more `M68k_LINE*.c` + extend the oracle; the memory-EA modes remain blocked by EA's no-sandbox-base + big-endian-CPU emit (Emu68 sets `SCTLR.EE` — edit `M68k_EA.c`'s ~6 load/store sites, the `[J5a]` logic). FOURTH coupling found: GNU `__attribute__((alias))` decoders are not byte-portable to clang/Mach-O — `emu68_darwinize.pl` (OURS) rewrites alias chains to plain forwarders in build-dir copies, quarantine stays byte-verbatim. New Emu68 files vendored: M68k_EA/LINE8/9/B/C/D/MOVE/MULDIV + headers — **Exhibit-B re-run clean**) · **`[J5d]` the WHOLE apps68k corpus through the JIT GREEN** (`hosted/jit68k/j5d_*` + the apps68k runner: a little engine — a per-basic-block translator DRIVING the REAL Emu68 decoders for every ALU/move/memory opcode + OUR re-hosted dispatcher ("MainLoop") owning inter-block control flow, the `(An)` sandbox-memory EA, and the `jsr`-through-vector → `[J3]` bridge. All FOUR real vasm-assembled hunk binaries run through the real-decoder JIT, each register file + sandbox memory byte-exact vs an INDEPENDENT from-scratch interpreter (`j5d_interp.c`, OURS, no Emu68): **mul=42**, **fact=120** (reg-to-reg `move.l` + `cmp.l` + nested loops), **arraysum=150** (relocated `lea` DATA + `add.l (a0)+,d0` through the REAL EA decoder — 2 sandbox accesses), **libcall=0** (`AllocMem`/`PutChar`/`FreeMem` via `jsr -off(a6)` decoded from the stream → the REAL `[J3]` marshaller into recording stubs, args/returns asserted). Corrupt-decode negative control bites; watchdog 15–20s → FAIL. **Vendored verbatim:** `M68k_LINE5.c` (ADDQ/SUBQ) + `M68k_CC.c` (link-only) — Exhibit-B re-grep clean. **The `[J5a]`/`M68k_EA.c` edit, realised as a build-dir transform:** `emu68_darwinize.pl --ea-sandbox` rewrites each `(An)`-class load/store in the BUILD-DIR copy to OUR `j5d_ea_mem` (sandbox-base add + `REV` byteswap + post/pre index); the quarantine `M68k_EA.c` stays BYTE-VERBATIM. **Dispatcher owns control flow:** rather than re-host LINE4/6's REG_PC/branch-target funnel, the engine decodes `Bcc`/`BRA`/`JSR`/`LEA`/`RTS` itself — Emu68's own block-exits-to-MainLoop model, re-hosted as a per-PC block cache + C dispatcher) · **`[J5e]` block-scoped register allocator (the "optimize" pass) GREEN** (`hosted/jit68k/j5e_test.c` + `j5c_ra.c` liveness + `j5d_engine.c` minimal frame: the REAL Emu68 decoders already keep the 68k Dn/An in fixed host regs across a block — D0..D7=w19..w26, An=w13..w17,w27..w29, no per-op spill inside the decoders — but the naive `[J5d]` frame bracketed EVERY block with a FIXED prologue/epilogue loading all 16 Dn/An + storing all 16 back unconditionally, 32 state ldr/str per block. The OURS RA now records, as the decoders call `RA_MapM68kRegister`/`RA_MapM68kRegisterForWrite`/`RA_SetDirtyM68kRegister`, which regs are READ-before-written (live-in) and WRITTEN (dirty); the engine emits the decoder body FIRST, then loads ONLY live-in regs in the prologue + stores back ONLY dirty regs in the epilogue. Read-before-write is the correctness point: a full `.L` write (`moveq`/`move.l #imm`) is NOT live-in so its prologue load is skipped, while a partial `.W`/`.B` write reads the reg first (`bfi`) so it is live-in AND dirty. **SPILL POLICY** at boundaries: every block exit (RTS/branch/the `jsr`→`[J3]` library bridge) stores dirty regs to the state struct BEFORE the boundary, so the bridge marshals from a fully-consistent memory state and `(An)` accesses (which go through the sandbox EA helper, not the state struct) don't race the deferred file. **MEASURED (byte-exact corpus gates the marker, not the smaller count):** AArch64 words 918→451 (−51%), state-struct ldr/str 512→45 (−91%) across mul/fact/arraysum/libcall; each program byte-exact vs the independent interpreter, negative control still bites. **No new Emu68 file vendored — the RA is OURS — Exhibit-B unchanged/clean.** Deferred: cross-block reg caching + linear-scan spilling under register pressure) · NEXT (a *bigger* app): cross-region chaining with a real return stack (computed `jmp`/`bsr`/`rts` nesting, `bcc.W/.L`), our own SR/exception model (host SIGSEGV → 68k vector), dirty-page SMC invalidation, the FPU/privileged ISA, and a sandbox-backed allocator for native-call return pointers outside the sandbox |

Specs are written one at a time, Role A → reviewed → handed to a fresh Role B agent. Host drivers
with a background host thread also conform to [host-wake-pattern.md](host-wake-pattern.md).

**Spike vs. production:** a GREEN host shim proves the mechanism in the unattended loop; it is
not yet the production contract. Each spec carries a "Spike status vs production contract" note
listing the open policy items (full Unicode tables, errno translation, AROS-side loop-break
tokens, etc.) that the AROS-side binding must still settle.
