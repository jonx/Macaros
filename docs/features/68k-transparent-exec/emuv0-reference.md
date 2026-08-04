# EmuV0 (AoA) as a reference project

Reference checkout: `/Users/jkn/Source/references/deadwood-aros` (sparse clone
of github.com/deadwood2/AROS, only `workbench/tools/AoA`, ~13k lines).

Provenance: this is APL-licensed AROS code, offered explicitly for study and
reuse by its author (Krzysztof, AROS developer, message of 2026-08-04). Unlike
the GPL emulators it is safe to read and to copy solutions from, with normal
AROS attribution. It is NOT part of our cleanroom disclaimers.

## What EmuV0 is

Runs 32-bit **x86 LE** AROS (ABIv0) programs on 64-bit x86_64 AROS. No CPU
emulation: the CPU is switched between long mode and 32-bit compatibility mode
(`ENTER32`/`ENTER64` far jumps), so guest code runs natively. Everything lives
in one address space; guest allocations use `MEMF_31BIT`. Same endianness,
same structure field values, only pointer width and struct layout differ.

## Their architecture in one paragraph

Only **five libraries are proxied** (exec, dos, graphics, intuition, utility,
plus cybergraphics/layers "partial"): hand-written per-function proxies that
translate V0 structures to native and call the real 64-bit library. **Every
other library runs as genuine 32-bit code** (real ABIv0 builds loaded from
`LIBSV0:` with a 32-bit ELF loader) and bottoms out in the five proxies.
Datatypes, Zune/MUI classes, icon.library, iffparse — all real 32-bit
binaries. Anything not implemented hits `unhandledCodePath()`: a requester
showing function/code-path/value, then suspend. Users screenshot it and post
to the forum; that is their entire triage pipeline.

## Solution-by-solution comparison with emu68k

| problem | EmuV0 | us |
|---|---|---|
| object identity | proxy struct: guest-layout `base` + `native` pointer, guest address IS the identity | same idea (facades); we also key adopted guest-owned objects by guest address |
| native->guest events | per-port `translate` vector set at port creation; translation happens in GetMsg | central IDCMP pump at Wait; translation keyed by bound window |
| guest-owned gadgets | wrap each in a native BOOPSI `gadgetwrapper` class instance; GM_RENDER etc. flow back through the wrapper to 32-bit dispatch | adopt + mirror natively, re-sync each crossing |
| DoIO | **never generic**: per-device, per-command marshalling (timer TR_ADDREQUEST, input IND_ADDHANDLER, ahi CMD_WRITE), everything else refused loudly | same conclusion reached independently (device.open contract first, commands by name) |
| SendIO/WaitIO | allocate a native request, link it to the V0 one via `mn_Node`, reply-port proxy gets a request-specific `translate` (TRIO/AHIIO) | not built yet — this is the model to copy when we get there |
| input handlers (tier 3 callbacks) | native handler translates the InputEvent chain, then calls the 32-bit handler on a dedicated 31-bit stack (`CALL32` + NewStackSwap) | not built yet |
| processes | real native AROS processes running 32-bit entry via trampoline -> **real preemption** | cooperative contexts switching at Wait -> our frame-wait starvation contract |
| unknown territory | `unhandledCodePath` requester + suspend + community screenshots | scanner verdicts + Bridge Lab traces + contract registry |
| coverage strategy | purely demand-driven, hand-written, quirk comments name the app that needed it | importer generates from `.conf`; overrides are exceptions |

## What their technique cannot give us

The core trick — run the guest ISA natively — does not exist for us: 68k is
big-endian and there is no compatibility mode. Every byte the guest reads or
writes needs swapping, which is exactly why our importer/facade machinery has
to exist. Their "only proxy 5 libraries" economy also depends on running real
32-bit library binaries; for us that would mean running the whole 68k
library stack under JIT (AROS m68k builds exist, so it is *possible*) — a
plausible long-term lever, not the current path: each JITted library multiplies
translated code and every one still bottoms out in the same 5-library bridge.

## What is directly worth stealing

1. **Per-request translate vectors on reply ports** (their SendIO/WaitIO
   model) when we build async device IO.
2. **The input-handler pattern** (native handler -> translate chain -> call
   guest on its own stack) for IND_ADDHANDLER and hooks generally.
3. **Their quirk inventory** — every `/* AppName does X */` comment is a
   known-hard case with a known fix (locally created RastPorts recreated
   before pixel calls; messages arriving after CloseWindow filtered via a
   closed-window list; WFLG_RMBTRAP removed post-open; MouseX/Y synced across
   all windows on MOUSEMOVE; popup-screen mouse sync on RAWMOUSE). Grep for
   `bug(` and app names before debugging any GUI misbehaviour they already met.
4. **The suspend-requester UX** for unhandled paths (we abort; they suspend
   and show exactly what was hit — better for field reports).
5. Their thread posts (arosworld.org thread 1724) as a **test corpus list**:
   the programs users actually tried.

## What we have that they don't

Automation (importer + policy + provenance), evidence discipline (Bridge Lab,
contracts, gates), byte-order correctness machinery, and a JIT. Their whole
project is the manual grind we were explicitly steered away from — viable for
one heroic maintainer on an easy substrate (no endian gap), not for ours.

## Interpreter mode (Krzysztof's ask: run on all AROS hosts)

The bridge stack (emu68k.library, importer output, marshalling, facades,
contracts) is portable C with no aarch64 in it; the only host-specific piece
is the translation core, which is vendored Emu68 (MPL-2.0) and emits A64
only. Three routes to non-aarch64 hosts, in ascending cost:

1. **Full 68k interpreter of our own.** We already author interpreter
   semantics cleanly from the 68000 PRM — the `j5*_interp.c` oracles do it
   per-generation for the JIT's driven subsets. Extending that to the full
   ISA is bounded, and we hold a unique validation asset: the byte-identical
   legacy corpus plus conformance gates let us differential-test interpreter
   vs JIT on aarch64 until they agree byte-exact, then trust the interpreter
   alone on x86_64. This is the credible route.
2. **Vendor an existing permissively-licensed interpreter core** behind the
   same engine interface. Fastest, but a second provenance surface and a
   second CCR/exception model to reconcile with ours.
3. **x86_64 backend for the JIT.** A new code generator; the most work, only
   worth it if x86_64 performance ever matters.

Not scheduled; recorded so the ask has an honest answer. Any code exchange or
public reply to Krzysztof is John's call.

## Verdict

Architecturally we converged on the same shapes independently (proxy+sync,
translate-at-boundary, per-command device marshalling, refuse loudly). Their
technique is not "better", it is the same boundary design on a much easier
substrate, executed manually. The transferable value is their catalogue of
GUI quirks and the async-IO/callback patterns, which are ahead of ours.
