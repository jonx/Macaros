Start the new chat with:
Read docs/features/68k-transparent-exec/HANDOVER.md completely, inspect both
working trees, then continue with the Aminet breadth corpus from the live
Imagine 4 run described below.  Wordworth 7 remains parked at its registration
screen; do not bypass that check.  Do not restart from the historical TurboCalc
ARexx section: its end-to-end result and superseded investigation are recorded
below.

# Handover: 68k transparent execution, 2026-08-07

Read this before touching the bridge. It says what we are building, where it
stands, and — most importantly — **the method**, which is the part that keeps
getting violated and costs the most time.

## What this is, for someone arriving cold

AROS is an open-source reimplementation of AmigaOS. This project runs it
**hosted on an Apple Silicon Mac**: AROS is compiled for aarch64 and runs as a
macOS process, drawing into a Cocoa window.

Classic Amiga software is **68000 machine code** and cannot run on that CPU
directly. So the project carries a **68k JIT** that translates 68k code to
aarch64 at run time, and around it a **bridge**: when the translated program
calls an Amiga library function, the bridge answers it with the real, native
AROS implementation. The program thinks it is talking to AmigaOS 3.x; it is
actually talking to 64-bit AROS through a translation layer.

The hard part is not the CPU, it is the **boundary**. A 68k program's world is
32-bit, big-endian, and lives inside one flat memory arena we give it. AROS's
world is 64-bit, little-endian, with real pointers. So every value crossing the
boundary has to be converted: a native pointer cannot be handed to the program
(it does not fit in a 32-bit register), a structure the program allocated
cannot be handed to AROS (wrong layout and byte order). The bridge therefore
deals in **tokens** (an opaque 32-bit handle standing for a native object),
**facades** (a guest-readable copy of a native structure, at a guest address),
and **mirrors** (a native copy of a structure the program owns). None of this
is guessed: anything the bridge does not know how to convert fails loudly as a
named "capability gap" rather than passing something through and corrupting
memory.

Most of that boundary code is **generated**, not written by hand — from the
same `.conf` interface files AROS itself uses to build its libraries, plus a
reviewed policy file that records the decisions a machine cannot infer (is this
pointer an opaque handle or a structure the program reads?). That is why the
method below matters so much: the work is describing types and regenerating,
not writing crossings one at a time.

**The waterline idea** (the current strategy): stop bridging every library.
Bridge only the bottom seven, and run every *other* Amiga library as its real
m68k binary inside the guest — the same code a real Amiga ran. Those upper
libraries then get their behaviour for free, bug for bug, and the only thing we
maintain is the bottom boundary.

### Where things are

| path | what it is |
|---|---|
| `~/Source/aros-aarch64` | **this repo** — the host/graft layer: JIT, bridge, tooling, docs |
| `~/Source/aros-upstream` | the AROS OS source; OS-side code lives here, incl. `emu68k.library`. Current checkout is `checkpoint/emu68k-progdir-20260807` at `b594c9ba09` plus the uncommitted generated/runtime side of this update. Do not publish it: commits and pushes are authorized only to the user's own `jonx` repositories. |
| `~/aros-build` | the build tree (`make hostlibs-emu68k` here builds the OS-side module) |
| `~/aros-crosstools` | the prebuilt cross toolchain — never rebuild it |
| `~/Source/references/aros-m68k-20260804/libs` | the real m68k library binaries we run guest-side |
| `hosted/jit68k/` | the 68k JIT engine (`j5d_engine.c`), hunk loader (`j4_loader.c`), diagnostics (`j5n_diag.c`) |
| `hosted/emu68k/emu68k_host.c` | host runtime: run lifecycle, guest memory, contexts, and guest-library loading |
| `hosted/emu68k/emu68k_exec.c` | the separately compiled handwritten `exec.library` guest semantics |
| `hosted/emu68k/emu68k_{dos,graphics,intuition,layers,utility,cybergraphics}.c` | separately compiled handwritten semantics for the other six waterline libraries |
| `hosted/emu68k/emu68k_gadtools.c` | the guest-side GadTools event adapter (`GT_GetIMsg`/`GT_ReplyIMsg`) |
| `hosted/emu68k/emu68k_{taskresource,timerdevice}.c` | direct resource/device vectors whose bases are not normal library facades |
| `hosted/emu68k/emu68k_internal.h` | private runtime interface used by per-library host handlers |
| `hosted/emu68k/nativelib/*.s` | 68k test fixtures (hand-written assembly), one per behaviour |
| `graft/gen-emu68k-bridge` | **the generator** — reads the `.conf` files + policy, writes the crossings |
| `graft/gen-struct-layouts` | generates 68k-vs-native structure layouts from the AROS headers |
| `graft/emu68k-bridge-policy.json` | **the policy** — the reviewed decisions the generator cannot infer |
| `graft/68k-corpus` | the test harness: boots AROS, runs a directory of 68k programs, collects verdicts |
| `arch/all-darwin/libs/emu68k/` (in `aros-upstream`) | the OS-side module + the generated crossing files |
| `docs/features/68k-transparent-exec/` | design, plan, and this handover |

Two ground rules that predate this document: **never run a bare `make`** in the
build tree (it tries to rebuild the toolchain), and OS-side edits go in
`aros-upstream`, not here.

## The goal

**Target state: the waterline is complete.** Seven libraries are bridged to native AROS:
exec, dos, graphics, intuition, layers, utility, cybergraphics (plus the
timer/input/clipboard devices and resource entry points programs reach). *Everything* above
that runs as real m68k guest code, or is a recorded policy exception. Then any
classic program is served by the same generic machinery — loader, facades,
object table, tag domains. Nothing per-program, nothing per-function.

Done means three things, none of them about one program:

1. **The tail set ships guest-side**: gadtools, iffparse, locale, icon,
   datatypes, asl, diskfont, commodities, plus muimaster/Zune.
2. **Breadth is proven by a corpus**, not claimed: an Aminet GUI sweep where
   every remaining failure is a named routing verdict (needs real hardware,
   needs a library we chose not to ship), never a capability gap.
3. **Re-runnable**: one command re-verifies the whole stack against a fresh
   m68k nightly.

Photogenics interactive is the depth milestone and the demo, not the goal.

## THE METHOD — follow this exactly

The unit of work is **a library**, never a function.

1. **The `.conf` is the interface oracle.** Every vector number, signature and
   register map comes from `rom/*/`*.conf* / `workbench/libs/*/`*.conf*. Never
   write an LVO or an offset from memory. Two bugs from exactly that:
   `Disable`/`Enable` were wired to `SetIntVector`/`AddIntServer` for months,
   and a test fixture called `InitIFFasClip` believing it was `InitIFFasDOS`.
   Vector numbers now come from a GENERATED header
   (`hosted/emu68k/emu68k_genlibs.h`, `LVO_*` + `EMU68K_EXEC_LVO_NAMES`), and
   `--validate-handwritten` fails the build if a hand-written constant
   disagrees with the conf.
2. **Ask the tool for the whole list**: `graft/gen-emu68k-bridge --gaps
   <library>` prints every unserved public vector and ranks the TYPES that
   block them. Work the top of that list; describing one type serves every
   vector that uses it (declaring `Layer`+`Layer_Info` took layers.library from
   0 crossings to 26).
3. **Describe types in ONE policy edit**, then regenerate. Object inference
   generates any vector whose pointers all map to exactly one declared object
   type — no per-function policy. Hand policy always wins over inference.
4. **Hand-written code only for what cannot be generated** (guest memory,
   guest-owned structures, callbacks), and even then derived from reading the
   source or a trace — never from memory.
5. **Validate statically. This costs seconds and needs no build:**
   - `--check <gendir>` — policy valid, generated sources in sync
   - `--gaps <library>` — coverage and blocking types
   - `--validate-handwritten hosted/emu68k/emu68k_exec.c` — constants match the
     conf, and no derived crossing shadows a hand-written decision
   - a `grep -c` for duplicate `case` labels beats finding them in a compiler
     error
6. **THEN one build.** Building is the slow step. Batch every change first.
   `make emu68k-dylib` (host side) and `make hostlibs-emu68k` in `~/aros-build`
   (OS side); deploy with `cp build/libemu68k.dylib ~/lib/`.
7. **THEN one test pass.** Put every fixture in ONE directory
   (`build/t3all`) and run the corpus once with the whole tail set routed
   guest-side — one boot covers everything:
   ```
   EMU68K_GUESTSIDE_LIBS="gadtools.library,iffparse.library,locale.library,\
   icon.library,datatypes.library,coolimages.library,asl.library,diskfont.library,\
   commodities.library,muimaster.library,stdc.library,posixc.library,fd.library,\
   rexxsyslib.library,rexxsupport.library" \
   EMU68K_LIBS_PATH=/Users/jkn/Source/references/aros-m68k-20260804/libs \
   EMU68K_MAX_SECONDS=40 CORPUS_TIMEOUT=300 \
   /Users/jkn/Source/aros-aarch64/graft/68k-corpus \
   /Users/jkn/Source/aros-aarch64/build/t3all <out>
   ```
   Use absolute paths: a compound command that `cd`s elsewhere silently breaks
   relative ones and the corpus never runs (cost an hour twice).
8. **Count at each step** so the method is visible: vectors served before and
   after, gaps outstanding, fixtures passing.

**Anti-patterns that have actually happened, all of them expensive:**
adding one policy entry and building; discovering the next gap by running
instead of by `--gaps`; writing a constant from memory; running a build to find
a duplicate `case`.

## Where it stands

Waterline coverage (`--gaps`; “callable” includes generated and handwritten
crossings, while a reviewed refusal deliberately fails closed):

| library | public | callable | reviewed refusal | unknown |
|---|---:|---:|---:|---:|
| exec | 146 | 144 | 2 | 0 |
| dos | 160 | 160 | 0 | 0 |
| graphics | 183 | 183 | 0 | 0 |
| intuition | 143 | 143 | 0 | 0 |
| layers | 40 | 40 | 0 | 0 |
| utility | 42 | 42 | 0 | 0 |
| cybergraphics | 24 | 24 | 0 | 0 |
| **total** | **738** | **736** | **2** | **0** |

The special-case pass cleared all 76 former public refusals in the six non-Exec
libraries. Their 592 public vectors now split into 419 generated crossings and
173 handwritten semantic crossings. The two remaining refusals are Exec
routing operations which do not belong at the host/native boundary.

“Callable” is an ABI/routing claim, not a promise that every feature succeeds.
The handwritten paths use three honest outcomes: real translated behaviour
(for example DOS packets, `RunCommand`, `ExAll`, CMode lists and bitmap staging),
a guest/native facade or callback adapter, or the API's documented negative
result where hosted AROS cannot provide the facility. Examples of the last
category are installing a native display driver from guest code, shell-wide
disk mutation such as `Format`, and private HIDD callback APIs. These now fail
as ordinary library results rather than stopping the program with a bridge
capability gap. The GELS animation and Layers callback paths are useful
approximations, not yet pixel-for-pixel/native-iteration implementations.

Tail set, routed guest-side (one boot, `build/t3libsweep`):

| library | status |
|---|---|
| gadtools | LOADED, and exercised: gadgets, menus and filtered event/reply lifecycle |
| iffparse | LOADED; `geniff` write/read round trip PASS |
| locale | LOADED (needed exec `SetFunction`) |
| icon | LOADED |
| datatypes | LOADED |
| coolimages | LOADED |
| asl | LOADED |
| diskfont | LOADED |
| commodities | LOADED |
| muimaster | LOADED; real `Text.mui` create/dispose lifecycle PASS |

The MUI lifecycle is a behavioural test, not just an open: guest
`muimaster.library` creates a built-in `Text.mui` object through the guest
Text -> Area -> Notify -> native rootclass superclass chain, receives a
guest-readable facade, and disposes it back through the same chain.  The
separate `hosted-emu68k-t3mui` gate also loads the real 68k `Busy.mcc` from
PROGDIR, runs its init/query path, creates an object through its guest
dispatcher and disposes it. Nested native-to-guest callbacks use depth-indexed
callback stacks, so recursive superclass dispatch does not overwrite an outer
callback frame.

Fixtures: the complete `hosted-emu68k-t3gen` gate passes, including
`genexecfull`, `gengadget`, `genmenuitem`, `genowngadget`, and the negative
controls for stale facades, unsupported fields, invalid callbacks and cyclic
families. `geniff` PASSes its real write/read round trip and `genmui` PASSes
built-in and separately loaded class create/dispose. The ten-library sweep reports `LOADED` for every listed tail
library. `stdc.library`, `posixc.library`, `rexxsyslib.library` and
`rexxsupport.library` are installed guest-side as dependencies rather than
counted as separate sweep targets.

The overall goal is **not achieved yet**. The original tail libraries now all
load and both built-in and external Zune class lifecycles are proven, but the
Aminet breadth corpus has not completed a clean sweep, and verification is not
yet a single fresh-nightly command. The waterline inventory itself is complete:
738/738 vectors are classified and there are no unknown crossings.

### Interactive event boundary follow-up

The host now has one per-run, typed event broker rather than a PhotoDemo stub.
`ModifyIDCMP` binds native window sources to a guest port; `Wait`, `WaitPort`
and `GetMsg` pump only sources registered for that destination or signal mask.
Several windows may share one guest port, while an ordinary process mailbox is
never guessed to contain native messages.  The OS side pairs every delivered
native `IntuiMessage` with its guest facade until reply and maps
`IDCMPWindow`/`IAddress` back through the run's object table, preserving the
guest's Window/Gadget identity instead of truncating native pointers.

The GadTools path has paired `GT_GetIMsg`/`GT_ReplyIMsg` handling around the
native filter/post-filter lifetime. Its Bridge Lab `event.filter` record includes Class, Code,
Qualifier, coordinates, Window and IAddress identities, so bad message content
is distinguishable from missing delivery.  The focused T3 event gate passes,
including shared-port typed routing.

PhotoDemo supplied the live depth proof.  Its GadTools results are now issued
as readable guest `struct Gadget` facades rather than opaque tokens.  The
generator carries `NextGadget` identity and the original 32-bit `UserData`;
scalar layout conversion carries `GadgetID`.  Consequently both
`IDCMP_GADGETDOWN` and `IDCMP_GADGETUP` now put a dereferenceable gadget in
`IAddress`, as classic applications require.  The diagnostic comparison was
native/guest ID `5/5` for the Palette list and `15/15` for the Tools palette.

The end-to-end interactive proof now covers New Black, opening Palette,
selecting Burnt Umber, selecting a Tools gadget, and dragging an AirBrush
stroke.  The black canvas survives palette activation and selection, the brown
stroke appears on the canvas and thumbnail, and the instance remains live with
no capability gap or crash. Selecting Project/Open produces
`IDCMP_MENUPICK` code `0xf820` inside the program; the tail sweep now confirms
that the intended guest-side `asl.library` itself initializes. The retained diagnostic traces are
`~/AROS/Shared/photodemo-identity-20260805.trace.jsonl` and
`~/AROS/Shared/photodemo-waterline-events-20260805.trace.jsonl`.

### TurboCalc Stage 2 — current result (2026-08-07)

Stage 2 is now an end-to-end pass, not an application-side dispatch blocker.
The real 68k RexxMast, RX and TurboCalc processes share one guest arena; RX
addresses `TCALC`, writes `10` and `1.25`, installs `=A1+A2`, reads the formula
and value back, and produces:

```
STAGE2-PASS TCALC formula==A1+A2 value=11.25
```

The generic fixes exposed by this run are:

- dynamic indexed `(d8,An,Xn)` library calls are recognized by the JIT;
- classic `RexxMsg.rm_LibBase` calls route through a per-run alias while
  Regina retains its AROS-private word at the same offset;
- `console.device/RawKeyConvert` converts a generated `InputEvent` layout;
- `mathieeedoubtrans/IEEEDPPow` has a safe native implementation, and both
  IEEE-double library families restore their documented 68k CCR side effects;
- returning from a root command no longer frees the arena under live
  `SYS_Asynch` guest processes; the run owns the process group until its last
  child exits or it is explicitly killed;
- a blocked guest `Wait()` polls its typed native event sources before the
  scheduler decides it is still asleep, so mouse buttons and menus wake an
  application after the launcher has returned;
- the OS-side runner distinguishes an ordinary JIT quantum from a fully idle
  process group. Ordinary work only reschedules; idle groups sleep one AROS
  timer tick. This keeps initial execution fast and the persistent GUI near
  idle CPU without starving Intuition or the hosted control task.

The live proof after `STAGE2-PASS` dismisses the demo requester, selects a
cell, and opens the Project menu. The instance remains responsive and the
sheet redraw is complete; the large gray missing/backfill rectangles from the
earlier run do not reproduce on the corrected scheduler/event path.

The Print Sheet path is a second deterministic UI proof. `PRINT` originally
left an empty requester and large staircase-shaped gray backing blocks because
TurboCalc reused one classic `struct Image` after changing its dimensions.
The mirror had allocated the planar payload inline and therefore treated the
first size as immutable; its named refusal contained the TurboCalc task after
the requester window had opened. Image mirrors now keep the native `Image`
address stable (Intuition may retain it) while owning the endian-converted
structure fields and a separately replaceable planar buffer. The same request
now renders its range, quality, layout, preview/file/cancel and print controls
completely, without a capability gap. `genwindow` exercises the generic contract by
drawing the same guest Image address, growing its planar payload, and drawing
it again; the focused corpus reports `[T3WINDOW] PASS`.

The planar payload itself is an MSB-first bit stream, not an array of numeric
`UWORD` values.  Word-wise endian conversion had exchanged the left and right
eight-pixel halves of every 16-pixel row: Zoom/Print radio buttons appeared as
a separated dash and crescent, and toolbar icons looked shifted and
overlapping.  Image mirrors now preserve the bytes exactly, matching the
existing guest-owned `BitMap` path and native graphics.library's planar
contract.  `genwindow` draws the asymmetric source bytes `$80,$01` into a
guest-owned plane and asserts the destination bytes remain `$80,$01`, so a
half-word swap fails deterministically.  The real TurboCalc toolbar, both
requesters and selected radio state are visually correct after the change.

Requester and sheet clicks are also verified on the real application.  The
remaining failure was not native input delivery: Bridge Lab showed both
`SELECTDOWN` and `SELECTUP` reaching the correct guest window, port and Gadget.
The OS-side event pump had applied both edges while draining the native queue,
however, so TurboCalc's handler observed the final released state when it
polled classic CIA-A `$BFE001`; child Tasks also had separate JIT engines whose
CIA view was never updated.  Mouse state now changes when guest `GetMsg`
consumes each IDCMP edge and is synchronized across every engine in the run.
A fresh run dismisses Continue, executes the complete ARexx proof, dismisses
the fully drawn Print Sheet requester with Cancel, and selects an ordinary
cell without a black fill or crash.  The retained live diagnostic is
`/tmp/bridge-click-cia.trace.jsonl`; its three DOWN/UP pairs identify the demo
requester, Print requester and sheet window respectively.

The Stage 2 launcher deletes its previous result before starting RX, and the
script writes the result stream sequentially. An old PASS can therefore no
longer make a new launch return early or leave stale trailing diagnostic lines.

Bridge Lab traces are bounded even when a new idle-loop bug is introduced.
Runtime detail stops at 32 MiB by default, one `trace.truncated` record names
the limit, and 64 KiB remains reserved for summary/failure and `run.end`
records. `EMU68K_BRIDGE_TRACE_MAX_BYTES` overrides the limit; `0` explicitly
selects unlimited output. The cap never changes guest execution.

The raw `EMU68K_TRACE_CALLS` stream is separate from Bridge Lab and is captured
in `/tmp/aros-window.log`.  It is now bounded per run to 10,000 calls by
default, including its detailed sub-messages; it emits one
`call trace truncated` marker and stops. `EMU68K_TRACE_CALLS_MAX` overrides the
count and `0` explicitly means unlimited.  Both controls are forwarded through
`aros-ctl run`.  This second cap matters: a corrupt-list loop once grew the raw
Imagine diagnostic to roughly 49 GiB even though the structured trace remained
small.

The investigation log below is retained for provenance. It is superseded by
the result above; do not resume its intermediate “remaining blocker” steps.

### Native ASL Open/Save crossing — current result (2026-08-07)

Classic file requesters are public result structures, not opaque handles.
Bridging `AllocAslRequest()` as a plain object token opened a native requester,
but TurboCalc crashed on return when it read `fr_File` and `fr_Drawer`.  The
bridge now gives file requesters a generated 56-byte classic
`struct FileRequester` facade.  `AslRequest()` synchronizes its public scalar
fields and copies File, Drawer and Pattern into guest strings before returning;
the native request remains owned by the facade until `FreeAslRequest()`.

The real TurboCalc Open command has been exercised both ways: Cancel returns
to a responsive sheet, and selecting
`MacRW:TurboCalc5/TurboCalc/Tutorial/Tutorial1.TCD` closes the requester and
loads the tutorial successfully.  That load also found and fixed a generic
generator bug: array bounds derived from a BYTE/WORD argument must discard the
undefined upper bits of the 68k data register before widening.  `LoadRGB4` now
sees its declared `WORD count`, rather than caller scratch bits, and the same
rule applies to every generated array crossing.

TurboCalc Demo disables saving, so `hosted/emu68k/nativelib/genaslsave.s` is
the focused proof for save mode.  It requests `ASLFR_DoSaveMode`, displays the
native Save requester with initial drawer/file values, and on acceptance reads
the returned guest `fr_File` and `fr_Drawer`.  The live result was
`[T3ASL] SAVE ACCEPTED`, with Macaros still running.  Multi-select remains an
intentional named gap because its retained `WBArg` array and BPTR locks need a
reviewed guest representation; no native pointer is leaked for that case.

To reproduce the interactive save proof, assemble `genaslsave.s` with the
repository's `vasmm68k_mot` using `-Fhunkexe -nosym -kick1hunks`, place the
result on `MacRW:`, and launch it through `aros-ctl` with native ASL routing
(no `EMU68K_GUESTSIDE_LIBS=asl.library`). Accept prints the acceptance token above;
Cancel prints `[T3ASL] SAVE CANCELLED` and is also a clean return.

### TurboCalc investigation log (historical, superseded)

TurboCalc 5 from Aminet is installed at
`~/AROS/Shared/TurboCalc5/TurboCalc/TurboCalc`; the startup script is
`~/AROS/Shared/turbocalc5-startup`.  It has exposed several generic bridge
gaps in sequence; none of the fixes is TurboCalc-specific:

- A blocking 68k continuation regression was fixed in
  `hosted/emu68k/t0p3_engine.c` (BSR displacement is 6, not 4).  The focused
  `hosted-emu68k-t0p3` gate passes.
- TurboCalc probes the classic left mouse button with the exact instruction
  `BTST #6,$BFE001`.  `hosted/jit68k/j5d_engine.c` now recognizes only that
  read-only CIA-A probe and answers it from per-engine button state supplied
  by the normal IDCMP broker.  The rest of the hardware page stays protected
  and still fails loudly.  `hosted-emu68k-t2guard` proves that generic
  `$BFE001` reads and address-register accesses remain classified as hardware.
- `SetPointer`/`ClearPointer` now retain an endian-converted per-window sprite
  shadow until replacement, clear, or run teardown.  This fixed the first
  crash after selecting a cell.
- Native Intuition produced `IDCMP_MENUPICK`, but the guest task handling it
  received only one 64-block quantum and was stranded.  Both external Wait
  pumps in `hosted/emu68k/emu68k_exec.c` now continue already-runnable sibling
  tasks across frames.  The trace proves Project menu choices are both
  delivered and consumed (for example `0xf800` for New and `0xf9e0` for
  About), rather than merely highlighting a row.

That Image-mirror issue is now fixed: the native Image identity remains stable
while its mirrored planar data is safely replaceable.  TurboCalc then exposed
a second generic representation error: it reads public fields of the
`locale.library` `struct Locale` returned by `OpenLocale`.  `Locale` is now a
generated guest-readable facade (168-byte 68k layout), instead of an opaque
token.  It moved the application past the raw-pointer fault at `0x003d9180`.

The current Stage 2 blocker is an ARexx scheduling/protocol stall.  The latest
traces show the real guest
RexxMast/RX/TurboCalc chain publishing `REXX` and `TCALC`, finding both ports,
and putting requests on TurboCalc's public `TCALC` port with the expected
`RXCOMM|RXFF_RESULT` RexxMsg layout.  TurboCalc's main task observes the public
signal but continues its internal `TurboCalc Parent Port`/`TurboCalc WINDOW-Port`
loop; it never consumes the queued ARexx message or replies on RX's port.  The
deterministic result file therefore stops at `STEP getcursorpos` (or, on the
older private-port experiment, records empty results), never `STAGE2-PASS`.

The launcher now starts RX asynchronously, inherits the invoking CLI streams,
waits for TurboCalc's `TurboCalc WINDOW-Port` before sending RX, and stays at
cooperative scheduler waits until the result file reaches `PASS`/`FAIL`; DOS
`Delay()` now honors its guest tick argument with a bounded sequence of
cooperative sibling turns instead of silently collapsing every delay to one
turn.  This is a generic scheduler fix, but it does not by itself clear the
TurboCalc stall: a fresh run with the corrected delay still stops at
`STEP getcursorpos`.
These are generic fixes and are deployed in the dylib.  `port.publish`,
`port.put`, `port.get.state`, `port.reply`, and `signal.wait.check` record the
raw guest port, owner task, signal bit, queue links, and Rexx words needed to
distinguish delivery from consumption.  An opt-in private-port experiment was
useful diagnostically but produced empty replies and is not part of the generic
bridge.  The next step is to resolve TurboCalc's own ARexx-port consumer/ABI
contract rather than add another blind routing shim.  Do not call the partial
result a Stage 2 pass.

The latest diagnostic (`~/AROS/Shared/Regina68k/bridge-stage2-delay512.trace.jsonl`)
is the clean discriminant.  The public `TCALC` message is correctly laid out and
queued (`RXCOMM|RXFF_RESULT`, length 128); TurboCalc's task receives the public
signal and then continues consuming its private `TurboCalc Parent Port` and
`TurboCalc WINDOW-Port` traffic.  There is no guest `GetMsg(TCALC)` and no reply
on RX's port.  Increasing the readiness delay therefore changes scheduling
volume, not the outcome.  The generic bridge is delivering the request; the
remaining work is to identify why this binary's ARexx dispatch path never
dequeues it (likely its application-side handoff, not a queue or signal
corruption).

This evidence does not yet exclude a JIT control-flow or continuation error
after `Wait`; a differential interpreter run or instruction-level trace is still
required before assigning the fault to TurboCalc.

A full-disassembly pass over the binary then narrowed all of this.  The
`0x3f034c` "callback" is a continuation-capture trampoline and it executed
CORRECTLY in the callback-4 trace (captured no-op continuation `0x3f02e6`,
resumed at `0x3f02c8` exactly as the code says: the `movel sp@+,sp@` stack
rewrite, the moveml pairs and the deferral PutMsg all behaved).  Not polling
TCALC there is by design: `GetMsg(TCALC)` (routine raw `0x3505c`) is called
only from the main task's two top-level idle loops (raw `0x34ea2` and
`0x34fbe`), which poll internal queue -> TCALC -> parent port -> Wait, so the
already-consumed signal edge is harmless once the task reaches an idle loop.
It never does, because startup's synchronous parent-port exchanges stop right
after the second window opens: the window task processes a worker msg, opens
window:2 (`port.bind` seq 15072), replies msg:13; main takes it and waits
forever for a further reply.  From that point the trace shows window:1 get
IDCMP_INACTIVEWINDOW and then every `event.pump` reads
`matched_sources:2, delivered:0` to the end of the run: native Intuition
delivered the LOSING half of the focus pair and never anything for window:2 -
no ACTIVEWINDOW, ever.  The window task has an explicit IDCMP_ACTIVEWINDOW
(0x40000) dispatch branch at raw `0xdee` (`cmpl #262144`), so the missing
activation edge is the prime suspect for the unsent startup reply.

Root cause (generic, not TurboCalc-specific): the classic pattern opens a
window with IDCMP flags 0, stores its shared port in UserPort, then calls
ModifyIDCMP to enable delivery.  On hardware the program wins the race against
input.device's asynchronous activation; under emu68k the native activation
completes whole quanta before the guest can run ModifyIDCMP, so the
IDCMP_ACTIVEWINDOW edge is lost with flags still 0.

Fix, deployed in both trees (2026-08-06 evening):

- `emu68k_oscall.c`: when ModifyIDCMP newly enables IDCMP_ACTIVEWINDOW
  (old native flags lacked it) on the window that IS
  `IntuitionBase->ActiveWindow` and that activation epoch delivered no
  ACTIVEWINDOW yet, the pump replays exactly one synthesized ACTIVEWINDOW
  (AllocIntuiMessage-backed, `allocated=TRUE` pairing; the guest's ReplyMsg
  now frees an allocated pairing via FreeIntuiMessage instead of replying it
  to a port Intuition never used).  Epoch tracking: `active_seen` set on any
  delivered ACTIVEWINDOW, cleared on delivered INACTIVEWINDOW.  The pump's
  guest AddTail+signal block is factored into `idcmp_queue_guest()`.
- Host side: `emu68k_event_bind()` now takes and logs the IDCMP class mask
  (`"classes"` in `event.source.bind`), so the next trace shows what a window
  actually requested.  Both call sites pass it (OpenWindow: facade
  IDCMPFlags; ModifyIDCMP: D0).

Both sides built clean (`make hostlibs-emu68k` in `~/aros-build`,
`make emu68k-dylib` here); dylib deployed to `~/lib` and re-signed;
`hosted-emu68k-t3setsignal` and `t3hello` PASS.  The first rerun attempt
proves nothing: a staged macOS update consumed the container's free space
mid-run and writes were failing; its artifacts were removed.  Rerun recipe:
remove `Regina68k/stage2.result`, boot with
`AROS_CTL_STARTUP_FILE=~/AROS/Shared/regina-stage2-startup` and a FRESH
`EMU68K_BRIDGE_TRACE` (runtime level is enough; avoid `EMU68K_TRACE_CALLS=1`
on a full boot - it floods `/private/tmp/aros-sidecar.log`, 800 MB last
time). Bridge Lab itself defaults to a 32 MiB cap; set
`EMU68K_BRIDGE_TRACE_MAX_BYTES` only when a smaller diagnostic budget is
needed. Also export
`EMU68K_LIBS_PATH=~/AROS/Shared/Regina68k/libs`: now that child `PROGDIR:` is
correctly the executable's own `commands` drawer, relying on the old inherited
parent drawer no longer finds the launcher's guest C runtime by accident.
Judge only a freshly written `STAGE2-PASS`/`FAIL` plus the trace: expect a
synthesized ACTIVEWINDOW delivery for window:2 right after its ModifyIDCMP,
then the parent-port reply chain resuming, the main task reaching its idle
loop, and a guest `port.get` on TCALC.  If the stall persists with the
ACTIVEWINDOW delivered, fall back to the differential JIT fixture - scoped to
the reply-match loop raw `0x34f20`-`0x34f5a` (`cmpal a0@(20),a1` identity
check), since the Wait/trampoline path is now trace-verified; a memory
snapshot of the deferred-list head at `a5+2088` distinguishes "msg:13
mismatched" from "msg:13 matched, next reply never owed" without any fixture.

IDLE-GUEST STARVATION: FIXED (2026-08-06 night).  Symptom: once a 68k GUI
program went fully idle waiting for input, the WHOLE instance froze - no
input, no screen updates, no menus, nothing, and it never recovered.  Not a
deadlock: the host process sat at 0.6% CPU, sleeping.

Diagnosis, and the tool that gave it: `aros-ctl tasks` on the frozen
instance.

```
-- current --
task '(unnamed)' state=RUN   pc=__semwait_signal   <- emu68k.library
-- ready --
task 'cocoa.hidd input'  state=READY pri=50
task 'input.device'      state=READY pri=20
task 'WANDERER:Wanderer' state=READY
```

The emu68k task was RUN inside `nanosleep` while the tasks that PRODUCE input
sat READY and starved.  AROS schedules cooperatively: a task keeps the CPU
until it makes an OS wait.  A host sleep parks the thread while AROS still
counts the task as running, so `cocoa.hidd input` and `input.device` never
ran, no IDCMP was ever produced, and the interactive wait loop in
`emu68k_exec.c` waited forever for an event it was itself preventing.  A
livelock that sustains itself: once entered, nothing can break it.  (A host
`sample` alone was misleading - `nanosleep` shows as `__semwait_signal`, which
looks like a deadlock on a semaphore.)

Fix: idle through the OS, never through the host thread.  The loop now calls
native `dos.Delay(1)` through the oscall, which blocks the task on
timer.device exactly as every other hosted idle task does (the same dump
shows `cocoa.hidd input`, the clipboard task and KeymapWatch all parked in
`Dos_33_Delay`).  `nanosleep` remains only for standalone host fixtures where
there is no OS behind us and the thread is the only thing to yield.  WaitTOF
is still avoided: on a hosted display it can be a successful no-op and spin.

Verified live: screen order changes on a depth-gadget click, the AROS shell
receives and echoes typed characters, guest clicks produce ~26 KB of guest
activity, and the task dump now shows the emu68k task in state WAIT with the
ready list EMPTY.  Any host-side sleep added to a path that runs on the AROS
task is this bug; check `aros-ctl tasks` for a READY list behind a RUN task.

REGRESSION: `make hosted-emu68k-idle` (booted, RESTARTS the instance).
`idle_window.c` opens a window, announces `IDLE-READY`, and goes idle in
`Wait`; the harness then waits TEN SECONDS before clicking, because racing
the idle would prove nothing.  Negative control actually run: with the native
idle swapped back for a host sleep, the dump shows
`state=RUN` with `cocoa.hidd input` (pri 50) and `input.device` (pri 20)
READY behind it, and the click never arrives - the result file stays at
`IDLE-READY`.  With the fix: `IDLE-PASS click delivered after idling`.

DEVPROC / DISKFONT: CLOSED (2026-08-06 night).  `DevProc` is now
`kind: "facade"` with a generated layout, and the generator learned the one
capability it was missing: `EMU_F_BPTR`, a facade field kind that maps a
native BPTR through the SAME handle table BPTR arguments and results already
use (`emu68k_handle_token` / `emu68k_handle_bptr` in `emu68k_marshal.c`'s two
walkers).  Which fields are BPTRs is DECLARED, never guessed, in
`BPTR_FIELDS` in `graft/gen-struct-layouts` - a pointer-shaped field is only a
BPTR when the header says so.  Generated result:
`{ 4, 8, 1, 4, 8, EMU_F_BPTR } /* dvp_Lock BPTR */`, exactly the offset
diskfont reads; `dvp_Port` and `dvp_DevNode` remain honest refusals.

Verified live: `diskfont.library` now loads, inits AND opens guest-side
(`state=2`), `GetDeviceProc` returns a facade instead of faulting, the guest
reads it and passes it straight back into a second `GetDeviceProc` (the
multi-assign walk that used to fault at `move.l 4(a5),d1`), zero faults, and
TurboCalc renders its full sheet - toolbar, grid, headers - with diskfont as
real m68k guest code.  All four static checks stayed green
(`--check` on both generators, `--validate-handwritten` on all eight
handlers).  The tail set routed guest-side is now: gadtools, iffparse,
locale, icon, datatypes, coolimages, asl, commodities, muimaster, stdc,
posixc, fd, rexxsyslib, rexxsupport, diskfont.

The original report, kept for the reasoning: `dos.library.GetDeviceProc`
returned `DevProc` as an OPAQUE token, but real callers read its fields - diskfont.library's guest
init resolves FONTS: and walks the multi-assign by reading `dvp_Lock` at
offset 4, so routing diskfont guest-side faults deterministically
(`move.l 4(a5),d1` with A5 = the token; crash bundle
`jit68k-crash-20260806T192641Z`).  The generic fix is the Locale pattern:
`kind: "facade"` with a generated layout - but `dvp_Lock` is a BPTR, and
facade fields currently support only SCALAR/BYTES/ARRAY/GUESTPTR, so this
needs one new generator capability: a facade field kind that maps a native
BPTR through the existing handle table (the same table BPTR arguments and
results already use).  Until then stage 2 must NOT route diskfont.library
guest-side; TurboCalc's GUI opens it during startup.

AVAILFONTS OUTPUT ABI: FIXED, STATIC/BUILD PROOF (2026-08-07).  TurboCalc's
About path exposed that `diskfont.library.AvailFonts` is not a byte-buffer API
despite its `STRPTR buffer` declaration.  Native AROS writes native-layout
`AvailFontsHeader` + `AvailFonts[]` records containing native pointers to
trailing names; the old generated crossing handed those records directly to
the big-endian 32-bit guest, which eventually dereferenced a byte-swapped host
pointer.  The policy now excludes this vector from automatic generation, the
layout generator owns `TTextAttr`, `AvailFontsHeader`, `AvailFonts` and
`TAvailFonts`, and the OS-side handwritten crossing calls native `AvailFonts`
into private memory then repacks classic 10-byte records, big-endian scalar
fields, guest pointers and trailing strings.  Tagged results remain a named
gap until their indirect `TagItem` payloads have a typed copy policy.  The
targeted emu68k host-library rebuild and both generator `--check` gates pass;
TurboCalc consumes the About command without the former host SIGSEGV.  The
popup itself was not made visible before the deliberate breadth pivot, so do
not promote this to a behavioural proof yet.

RERUN VERDICT (trace `Regina68k/bridge-stage2-synth-5.trace.jsonl`,
2026-08-06 late): the ACTIVEWINDOW fix WORKS - `event.source.bind` now
records both windows requesting classes `0x024da77e` (ACTIVEWINDOW
included); the synthesized ACTIVEWINDOW was delivered to and consumed by
BOTH windows (seq 10502 window:1 - its handler even forwards it to the main
task; seq 13425 window:2 - its per-window context has the forward-flag bits
at ctx+53 clear, so it legitimately just marks ctx+54 active).  No fault,
`run.end ok`.  But the result file still stops at `STEP getcursorpos`:
ACTIVEWINDOW was a real generic gap, not the startup-completing trigger.

The stall is now decoded to one precise app mechanism.  TurboCalc's two
tasks use a continuation-post protocol (all guest addresses OLD-run base
0x3bb33a; the synth-5 run is the same code at +0x26BD8):

- `0x3f034c` posts "run function F with these regs" to the WINDOW task;
  `0x3bbe46` is the mirror posting to the MAIN task's Parent Port.  The
  window dispatch (`0x3bc600`) runs F and frees the message; replies are
  separate posts.
- The main task's synchronous call is: post F, then `lea <base+0x3500>,a0;
  bsr 0x3f0274` - block reading Parent Port, comparing each arriving
  message pointer against the RENDEZVOUS SLOT at `base+0x3514` (struct
  base+0x3500, offset 20; both `lea 0x3500` operands are relocated, checked
  against the HUNK reloc table).  Non-matching messages (event forwards)
  are deferred to the internal list at `a5+2088` - by design; they are
  consumed by the top-level idle loops (`0x3f01dc`/`0x3f02f8`), the ONLY
  places that also poll `GetMsg(TCALC)`.
- In synth-5 the request/reply ledger is: msg:1->2 matched, msg:5->7
  matched, msg:6 = deferral no-op, msg:11 = ACTIVEWINDOW forward
  (deferred, correct), msg:12 (F=`0x3c0814`, opens window:2) -> msg:14
  (w44=0x30 event post, MISMATCHED and deferred).  The main task then
  waits forever at `0x3f02a8` for the rendezvous message; the window task
  idles normally.  Identical shape in callback-4 (msg:13 there).

DECISIVE ISOLATION TEST (`ECHO`, new, 2026-08-06 night): rather than keep
reading TurboCalc's internals, split the question with the smallest program
that speaks the same protocol.  `hosted/emu68k/regina/echo_host.c` is a
minimal ARexx host (CreatePort("ECHO"), GetMsg, CreateArgstring,
ReplyMsg); `echo_launcher.c` starts RexxMast + ECHO + RX in one arena and
`Regina68k/echo.rexx` sends `'ping'` and checks `result == 'PONG:ping'`.
Startup file `~/AROS/Shared/regina-echo-startup`; both fixtures build from
`make hosted-emu68k-regina-fixtures` and are copied to
`Regina68k/commands/`.

Result: **ECHO-PASS answer=PONG:ping**.  The whole guest ARexx transport is
therefore PROVEN inside emu68k: RexxMast, RX, rexxsyslib guest-side, public
port publish/find, the RXCOMM|RXFF_RESULT RexxMsg layout, GetMsg, the result
argstring, ReplyMsg routing back through RX into a Rexx variable, and the
DOS write of the result file.  Nothing in stage 2's transport is unproven.
Keep this fixture as the permanent ARexx regression: it costs one boot and
fails loudly if any of that regresses.

The remaining TurboCalc stall is therefore in TurboCalc's own dispatch
STATE, not in the transport - and the interactive run pins it exactly.
Running TurboCalc alone (startup file `~/AROS/Shared/turbocalc5-startup`,
no ARexx at all) produces a small, complete trace and a screenshot showing
the application fully up and healthy: title bar `TurboCalcDemo V5.0
(c)1993-98 M.Friedrich - AREXX-Port: TCALC`, sheet window `Folder1` with
toolbar, column/row headers and the `Sheet1` tab.  No requester, nothing
modal on screen.  In that healthy state the main task STILL never polls
TCALC: its only `GetMsg` sites are the parent port (pcs `0x3f02ba` and
`0x3f02d4`, both inside the nested loop), and it waits at `0x3f02a8` on
`0xc0000000`.

The blocking call is now identified exactly, at `0x3f13e4`:

```
3f13e4  moveml d2-d7/a2-fp,-(sp)
3f13e8  lea  (pc,0x3f1416),a3      ; completion callback for the window task
3f13ec  pea  0x54aa                ; F = 0x3c0814, the sheet-window opener
3f13f2  jsr  a5@(142)              ; post it to the WINDOW task
3f13f6  lea  (pc,0x3ed602),a0
3f13fa  bsr  0x3f0274              ; WAIT for the ack (F == 0x3ed602)
3f13fe  tstl d0                    ; d0 = ack message's saved d0
3f1400  beq  0x3f1412              ; ack d0 == 0 -> return -1 IMMEDIATELY
3f1402  lea  (pc,0x3f1410),a0
3f1406  bsr  0x3f0274              ; else WAIT for completion (F == 0x3f1410)
```

The rendezvous test in `0x3f0274` is `cmpal a0@(20),a1`: "does this message's
continuation word (msg+20) equal the address I passed in A0"; unmatched
messages are deferred to `a5+2088` by design.  Both `0x3ed602`, `0x3f1410`
and `0x3f02e6` are bare `rts` instructions used purely as rendezvous tokens.
The window task's ack comes from `0x3c0ad2`: it creates the sheet, sets
`d0 = a4` (the new object) and posts token `0x3ed602`; the completion token
`0x3f1410` is only posted by `0x3f1416`, the callback the window task stores
at `obj+990` and calls when that window is finished.

So the trace reads: ack received (msg:12/msg:14, `word24` nonzero = the sheet
object), main takes the `0x3f1402` branch and waits for the sheet window's
COMPLETION - which never comes while the sheet is open.  Every later message
(ACTIVEWINDOW forwards, event posts) correctly mismatches and is deferred.
Meanwhile the deferred queue and TCALC are only drained by the idle loops
`0x3f01dc`/`0x3f02f8`, which main cannot reach while blocked here.  When the
ARexx signal does arrive in this state, `0x3f02c0` posts the no-op
continuation `0x3f02e6` to the window task, which runs it and frees it: the
ARexx message stays queued.  That is consistent in all three traces.

HARNESS DEFECT FOUND BY COMPARING THE TWO RUNS (the reason stage 2 differs
from the healthy interactive run at all): the interactive startup file does
`CD MacRW:TurboCalc5/TurboCalc` before running the program, while the stage 2
launcher called `SystemTags("MacRW:TurboCalc5/TurboCalc/TurboCalc", ...)`
from `MacRW:Regina68k`.  A classic application reads its settings, catalogs
and startup assets relative to the CURRENT DIRECTORY, so started from
elsewhere it silently takes a different startup path.  The traces show it
exactly: the interactive run's MAIN task opens an extra window at seq 13/14
requesting IDCMP classes `0x00000008` (MOUSEBUTTONS only - a click-to-dismiss
splash) and closes it again at seq 650; the stage 2 run never opens that
window at all, and its screen renders EMPTY (screen title bar present, no
window drawn) while the interactive run draws the full `Folder1` sheet.
`stage2_launcher.c` now Locks the application's directory and CurrentDir()s
into it just for that launch, restoring the Regina directory before RX
starts.  Launch a corpus application the way a user does; do not assume
PROGDIR: is enough.

SECOND REAL BRIDGE GAP, FOUND BY THE CALL-LEVEL DIFF AND FIXED (2026-08-06
night).  Running the healthy and stalled startups with call tracing and
diffing TurboCalc's OWN call sequence (filter by pc inside the program image,
normalise both to the disassembly base) gives a single first divergence at
call #31: both runs reach the same `dos.Open`, which returns a handle
interactively and ZERO under stage 2.  Both passed the same name pointer
(identical image-relative offset 0x37b1c); reading the binary there gives
`PROGDIR:TurboCalc.data`.

Root cause, generic: every guest context of a run shares ONE native AROS
process, so `PROGDIR:` was resolved against whatever program that process was
created for.  Any 68k program started BY another 68k program therefore
inherited the launcher's PROGDIR: - TurboCalc looked for its data file in
`Regina68k/commands/`.  This affects every classic application launched from
a guest program, not only this one.

Fix: `struct emu68k_ctx` now carries `progdir` (the directory its program was
loaded from; a context that is not a loaded program inherits its creator's,
which is what a child of that program should see).  The host exports
`emu68k_run_progdir()` (symbol 13 in `emu68k_init.c`'s table, and a new
`run_progdir` member of `struct Emu68kHostIf` / `Emu68kOSCallCtx`), and
`Emu68k_OSCall`'s prelude applies it to the native process with
`SetProgramDir()` when it changes, caching the lock; `Emu68k_OSCallEndRun`
restores the process's own directory and frees ours.

Verified: `PROGDIR:TurboCalc.data` now opens (`d0=00084860` at the exact site
that returned 0); the stage 2 startup now matches the healthy interactive run
bind-for-bind - splash window opens (main task, IDCMP classes `0x00000008`),
the application dismisses it itself, then the sheet window opens; and the
screen title finally carries the free-memory counter
(`... AREXX-Port: TCALC  6716444 free`), the healthy-state marker that was
absent from every stalled run.

REGRESSION, proven in both directions: `make hosted-emu68k-progdir` (needs a
booted instance and RESTARTS it, so it is not in the default gate).
`progdir_parent.c` starts `progdir_child.c`, which lives in a DIFFERENT drawer
(`Regina68k/progdir/`) and opens `PROGDIR:progdirchild.data` next to itself.
With the fix: `PROGDIR-PASS`.  With the OS-side apply reverted and the module
rebuilt: `PROGDIR-FAIL could not open PROGDIR:progdirchild.data`.  A test that
cannot fail proves nothing, so that negative control was actually run, not
assumed.

The stage 2 launcher's `Delay(1000)` readiness pause is GONE.  An ARexx
message queues on the public port and is served whenever the application next
polls it, so sleeping before sending buys nothing and hides whether the
application serves the port at all.  Waiting for the port to EXIST is a real
precondition and stays; waiting on a stopwatch is not.

That also REFUTES the "the command was sent too early" theory, which the
timing invited (RX puts the message at seq 22548, the sheet opens at 29498):
the message stayed queued for roughly 295,000 further events, long after the
application reached its healthy state, and TCALC was never polled once
(`GetMsg(TCALC)` count 0 for the whole run).  So this build genuinely does
not serve its public port while the main task sits in the sheet rendezvous.

VERDICT (settled 2026-08-07, and it CORRECTS the `obj+990` lead below).  That
lead was wrong and is retracted: `obj+990` is a PER-OBJECT handler slot, not
the rendezvous callback.  The sibling path at `0x3c2484` shows what a real
one is - `lea (pc,0x3c2492),a3` then `movel a3,a4@(990)`: a PC-RELATIVE LOCAL
handler belonging to the creating code, never the main task's `0x3f1416`.  So
the `subal a3,a3` at `0x3c0ad2` is DELIBERATE ("this creation path has no
per-object handler"), not a register the bridge lost, and the two `jsr (a3)`
sites in the binary (`0x3c0f4e`, `0x3d51a0`) are local render/format
callbacks, unrelated to the rendezvous protocol.

So the named routing verdict for TurboCalc 5 Demo is:

  APPLICATION BEHAVIOUR, NOT A BRIDGE GAP.  Its main task blocks in the
  rendezvous wait at `0x3f0274` for the sheet window's completion token
  (`0x3f1410`), which is only posted when that window finishes.  Its public
  ARexx port is drained ONLY by `GetMsg(TCALC)` at `0x3f0396`, called from the
  two top-level idle loops (`0x3f01dc`, `0x3f02f8`) that the main task cannot
  reach while it is blocked there.  A request therefore stays queued - it is
  never lost, and never served while a sheet is open.  Evidence: identical in
  three independent traces, including a run with NO ARexx involved at all, and
  the message stayed queued for ~295,000 scheduler events after the
  application was fully healthy with `GetMsg(TCALC)` count 0.

Everything the bridge owes this application is delivered and proven: the
ARexx transport passes end-to-end (ECHO fixture), the message is correctly
laid out and queued on the right port with the right signal, the application
starts up identically to a hand-launched run, and it renders and responds.
Do NOT synthesize a completion message, route the request to a private port,
or add an application-specific shim to force a STAGE2-PASS: all three would
be lying about a program that is behaving as written.  If a scripted
spreadsheet proof is wanted, drive a DIFFERENT application whose idle loop
serves its port, or drive this one through its GUI (menus and keys already
work).  Stage 2 as written cannot pass against this binary, and that is a
result, not a defect.

The CurrentDir fix is right on its own merits but did NOT change the
outcome: the corrected run still opens only the two window-task windows,
still renders an empty screen, still stops at `STEP getcursorpos`.  So the
startup paths diverge for another reason, and the remaining structural
difference between the two runs is the interesting one: interactively
TurboCalc is the only program in its OWN emu68k arena, while stage 2 starts
it via SystemTags as a CHILD CONTEXT inside the launcher's arena (shared on
purpose, so the public ports are mutually visible).  The next concrete step
is a call-level diff of the two startups: run each with
`EMU68K_TRACE_CALLS=1` and compare the OS-call sequence up to the point
where the interactive run opens the splash window (main task, IDCMP classes
`0x8`) and the stage 2 run does not.  The screen title is a free progress
indicator: the healthy run shows `... AREXX-Port: TCALC  9196796 free`
(the app updates the free-memory counter once it is running), the stalled
run shows the same line WITHOUT the counter.

Second named gap found from the same log, unrelated to the stall but real,
and now CLOSED: our guest-side `posixc.library` calls
`OpenLibrary("fd.library")` and it failed 1028 times in one run.

`fd.library` is ABOVE the waterline.  It is not one of the bottom seven; it
is an AROS-specific library that exists only to back posixc's file-descriptor
table, and no classic Amiga program opens it - only our guest-side
posixc/stdc do.  So it belongs to the tail set and ships as real m68k guest
code, exactly like the libraries that depend on it: no bridge policy, no
crossings, nothing to declare.  Built with `make workbench-libs-fd` in
`~/aros-m68k-build` (its source is `workbench/libs/fd/`), then converted with
`elf2hunk` - a module built for m68k is ELF and the guest loader needs HUNK,
which is why dropping the build output straight into the library directory
still failed to load.  Verified: failures 1028 -> 0, `guestlib init.done` /
`open.done` for `fd.library`, ECHO still PASS.

That conversion step is the general rule for extending the tail set: build
the m68k module, run it through `elf2hunk`, and only then put it in
`EMU68K_LIBS_PATH`.

With those corrected, the open question is sharp and app-shaped, not
transport-shaped: why does this build take the "wait for completion" branch -
i.e. why is the ack's `d0` nonzero here?  Two candidates, both cheap to test next:
(1) the ack legitimately carries the object and a REAL Amiga also blocks,
    with ARexx served because the completion arrives promptly (something in
    the window task's `0x3bc9d8` chain finishing the sheet's "run" phase);
(2) the ack's `d0` should have been zero for this startup path, which would
    make the main task return immediately to its idle loop.
The discriminator: watch whether the window task's `0x3c0aee` chain ever
calls `obj+990` (the completion callback); a temporary Bridge Lab event on
the poster at `0x3bbe46` recording the token value would show it directly.
Do NOT synthesize a completion message: that would be an app-specific shim.

An older framing of the same stall follows (kept because the addresses are
still useful): the rendezvous slot `TurboCalc_base+0x3514` at stall time.  Zero -> F=`0x3c0814`'s
continuation chain never registered a reply (find what that chain still
waits for - one such F waits at `0x3be81a` on a mask built from another
port's sigbit before closing windows and posting the rendezvous reply from
`0x3be860` `pea 0x3500`); nonzero -> the reply was registered but its
message never arrived or the identity compare failed, which points back at
delivery or the JIT (`cmpal a0@(20),a1` loop `0x3f025a-0x3f0294`).  Next
run: snapshot or peek that slot when the result file stalls, before any
differential fixture work.

The follow-up disassembly/trace pass makes that distinction concrete.  In the
fresh `bridge-stage2-callback-4.trace.jsonl` run, the public signal is returned
at TurboCalc's `Wait` site (`0x003f02a8`, guest raw `0x34f72`), after which the
binary's callback posts an internal message to `TurboCalc WINDOW-Port` and waits
on the private parent-port bit.  The callback target is a normal guest routine
(`0x003f034c`, first opcode `48e7fffa`); it is not a missing native bridge
vector.  Static disassembly of the same binary shows the only public-port
`GetMsg` routine at raw `0x3505c`, while the active callback/private loop never
calls it.  No trace contains a `GetMsg(TCALC)` or an RX reply.  This rules out a
malformed signal/queue/message crossing, but not a JIT control-flow error after
the crossing.  Do not route
the message to the private port: that diagnostic experiment produced empty
replies and would be an application-specific shim.  The remaining question is
whether TurboCalc expects an additional application-side event/host handoff;
the bridge should stay generic until that contract is evidenced.

For a long-lived diagnostic boot, make the launcher the synchronous startup
command rather than `Run`ning it asynchronously.  An asynchronous one-shot
payload can let the initial CLI finish and tear down the hosted instance while
its child contexts are still waiting; the direct command keeps the instance
alive long enough to inspect the trace.  This is a harness-lifecycle detail,
not a TurboCalc bridge workaround.

One concrete cause of such inconsistent guest IPC state has been fixed and
regression-tested: `exec.SetSignal` was an old zero-returning startup stub even
though `PutMsg`, `Signal`, and IDCMP now update `tc_SigRecvd`.  It now performs
the normal masked update and returns the previous signal word.  The standalone
`make hosted-emu68k-t3setsignal` fixture proves the exact low-bit behaviour.

Host controls are now reliable inside booted AROS too.  `libemu68k.dylib`
resolves its environment lookups past AROSBootstrap's exported guest-libc
`getenv`, to macOS's real process environment.  Before that repair the normal
`EMU68K_*` controls could silently read guest `ENV:` instead, making a supplied
trace path appear to be ignored.  The same focused fixture verifies the host
lookup before it exercises the signal contract.

The harness no longer kills an existing Macaros process unless explicitly
requested (`AROS_CTL_RESTART=1`, `stop`, or `kill`).  A Stage 2 run may be
restarted when needed; the launcher removes the previous result before it
starts RX. A successful boot alone is not a Stage 2 proof: retain the fresh
Bridge Lab trace and verify that the result file contains `STAGE2-PASS` with
both formula and value checks.

`aros-ctl run` is now owned by a non-keepalive launchd plist rather than the
calling shell or a submitted inferred-keepalive job. It launches `Macaros`
directly, carries only explicitly enabled optional trace variables, and sets
the working directory to the selected boot directory. Consequently a payload
return does not kill the OS, empty diagnostic variables do not accidentally
enable full tracing, and EMU/MacRO/MacRW bootability no longer depends on the
terminal's current directory. Only `stop`, `kill`, a guest power request, or an
explicit replacement ends the session. See the control-harness README for the
payload-vs-replacement contract.

The early bootstrap line `Could not open version 36 or higher of library
"dos.library"` is a misleading probe and is not, by itself, a failure: normal
boots in the successful Stage 2 runs print it and continue. Diagnose only the
first later fatal event, crash bundle, or missing startup milestone; do not
rebuild DOS merely because this line appeared.

For repeat runs use the tail-set environment shown in method step 7, plus:

```
AROS_CTL_STARTUP_FILE=/Users/jkn/AROS/Shared/turbocalc5-startup
EMU68K_TRACE_FAULT=1 EMU68K_TRACE_CALLS=1
EMU68K_BRIDGE_TRACE=/Users/jkn/AROS/Shared/turbocalc5-bridge-N.trace.jsonl
EMU68K_BRIDGE_TRACE_LEVEL=debug
```

The proposed faster debugging loop is not implemented yet.  The useful design
is: pause on a pre-call refusal, capture registers/arguments/relevant guest
memory and recent events, load a versioned late-bound override and retry only
when no native side effect occurred.  Otherwise restart and replay a recorded
launch/input script.  Rewinding CPU state alone is unsafe because AROS windows,
ports, files and allocations are native state.  The standalone JIT runner is
appropriate for CPU/hunk/isolated fixtures; GUI breadth testing still needs a
scripted Macaros instance.

### Wordworth 7 breadth run — current result (2026-08-07)

The user supplied a private retail ISO from the Internet Archive item
`wordworth-7`.  It is mounted read-only from
`/Users/jkn/Downloads/Wordworth 7.iso` and extracted only under
`/Users/jkn/AROS/Shared/Wordworth7-private`; no product binary, registration
material, or extracted data belongs in either repository.  The ISO SHA-256 is
`9b3459983ab87ad71ab9bf78faf9c7f0d404e7b0584b2daefcc4a9f9648761bf`.
The launcher SHA-256 is
`f659a805fc9307ec77053648a45cc23069a78a7899829069430e6c17dfe7a8d6` and
`WwProg` is
`7aac6b826c0af9f70de474fdbd6e3349cd530337fbae5daa339906cbde71af35`.

The compatibility run also inspected the four-disk FTH specimen advertised by
Amiga City as "Abandonware".  That site label is not a licence grant.  The
four public records are 1693 (disk 1), 1677 (disk 2), 1691 (disk 3), and 1684
(disk 4); their ZIP SHA-256 values are, in disk order,
`9b65ff3ed481743384d9efa9a5d5cbf54f4163a89de6a4836951946ab1e7a042`,
`b5ce8f54d5140c2882d2e8858bf61cf1b3e5b46120944a68d4805fd1ced4b6e1`,
`e38ac46612b1c44d61b2901cd7630b6fd44beea92bd2026eca3df5a806aa347b`,
and `5359244f23d09f714d5fb789e325c1f4d4dac5d758aa5b4400246fcfce3f7e62`.
They and their extracted ADFs remain outside the repositories under
`/Users/jkn/AROS/Shared/Wordworth7-FTH-private`.

The FTH Workbench launcher is byte-identical to the retail launcher.  Its
`WwProg` is 28 bytes shorter and has SHA-256
`444efdeedca1289355d817327497904c20cf7518ba06cc7125f2d1b618e18d1e`.
It is not actually an already-unlocked executable: it presents the same Name,
Organisation, and Licence gate, and a blank licence is not accepted.  The disk
also contains a separate `FAITH` utility and its NFO calls the release "pseudo
registration".  Do not execute that utility, derive a serial, copy a serial
into this document, or change the bridge to bypass the gate.  It is neither
needed nor appropriate for compatibility work.

This run found and fixed two generic contracts:

- A native Workbench process receives `WBStartup` on `pr_MsgPort`, the same
  port native DOS uses for packet replies.  The bridge now removes and retains
  that native message before its first DOS call, creates a big-endian 68k
  `WBStartup`/`WBArg` tree with issued BPTR lock tokens, sets guest `pr_CLI` to
  zero, and queues the mirror on the classic embedded Process port.  The native
  message is replied exactly once after guest/bridge teardown.  The standalone
  `make hosted-emu68k-t3workbench` fixture proves `pr_CLI`, queueing, both locks
  and both names before instruction zero.  This removes the former misleading
  `dopacket.c` "unexpected DOS Packet Received" alert for every Workbench-
  launched 68k program.
- `WwProg` contains one exact `move.w Dn,$dff180` calibration loop.  That
  COLOR00 write form is now a flag-correct dispatcher sink and is excluded
  from the static hardware-banger verdict.  Every other custom register, plus
  computed access to `$dff180`, remains behind the `PROT_NONE` runtime guard.
  `color00.s`, the shifted `$dff182` positive control, and the computed-address
  negative control pass in `make hosted-emu68k-t2guard`.

The first live Workbench run then exposed a policy typo:
`RefreshGadgets.requester` was non-null even though the Amiga API permits NULL.
It now matches `RefreshGList` as a nullable `Requester` mirror, and regenerated
sources/build checks pass.  With these repairs the real `Wordworth` launcher
consumes its Workbench message and renders the retail Wordworth 7 registration
screen; Name, Organisation and Licence gadgets accept focus and text.  The
same result is proven with the FTH `WwProg`; neither run produces a bridge
refusal or Macaros crash before the gate.  A prior test instance was
intentionally parked at the FTH specimen's Licence field.  Its bounded trace is
`/Users/jkn/AROS/Shared/wordworth7-unlocked-bridge.trace.jsonl` and its private
payload is `/Users/jkn/AROS/Shared/wordworth7-unlocked-startup`.  Keep that
instance running for the user.  The disc requires the owner's licence number
(the included Read_Me explicitly refers to existing licence numbers), so
compatibility work must not invent, extract, or bypass one.

To continue the executable path, obtain runtime evidence from a licence owner
or a copy that the owner has already registered; do not ask the bridge to
manufacture registration state.  Finish the normal path first and then the
ARexx path.  The disc already supplies 21 scripts in
`Wordworth7-private/WwRexx`, while `Help/Editing.guide` documents 142 commands
and the application ports `WORDWORTH.1`, `WORDWORTH.2`, and so on.  Use a small
safe documented command as a deterministic request/reply/result oracle, retain
the Bridge Lab trace, and only then try one bundled script.  The untouched
retail launch payload remains `/Users/jkn/AROS/Shared/wordworth7-startup`.

### Imagine 4 breadth run — current result (2026-08-07)

CU Amiga's January 1997 cover CD supplied the complete Imagine 4.0 integer and
FPU builds.  The ISO and extracted application remain private under
`/Users/jkn/AROS/Shared/Imagine4-private`; no product bytes belong in the
repositories.  Use `/Users/jkn/AROS/Shared/imagine4-startup`, which selects the
NTSC `Imagine.fp` build.  The integer build deliberately rejects the current
NTSC graphics facade and then divides by zero; that is the application's own
PAL/NTSC check, not the compatibility target.

The FPU build first exposed the missing `fmove.l #0,fpcr` form.  Immediate
longword moves to FPCR/FPSR/FPIAR are now implemented in both the JIT dispatcher
and the independent interpreter and covered by `j5r.exe` (11 system-register
operations).  It then appeared to hang during startup.  A bounded call trace
showed `RemHead`/`ReplyMsg` repeating on a list whose `lh_Head` pointed to the
list header itself.  `RemHead` now refuses that corrupt shape rather than
allowing an unbounded loop.

The generic diagnostic memory watchpoint (`JIT68K_WATCH_GUEST=<addr>`, optional
`JIT68K_WATCH_VALUE=<value>`, with `EMU68K_JIT_DIAG=1`) located the first bad
write at guest block `00331868..00331878`.  This is the canonical `NewList`
tail, ending in `move.l a0,-(a0)`.  The 68k evaluates the source before the
destination predecrement; the hosted decoder borrowed the live A0 register, so
the AArch64 pre-index store consumed the decremented value and created a
self-linked list.  The build-dir MOVE rewrite now snapshots a direct An source
when the same An is a postincrement/predecrement destination.  Focused tests
prove both `MOVE.L A0,-(A0)` and `MOVE.L A0,(A0)+` byte-exact against the
independent interpreter.

After that fix, the real `Imagine.fp` reaches its complete 800x600 project/
render screen and waits normally on its IDCMP port.  Mouse DOWN and UP events
are pumped, taken and replied; toggling `Auto Dither` visibly changes the
control.  There is no bridge refusal, JIT fault or Macaros crash in the current
run.  Its bounded trace is
`/Users/jkn/AROS/Shared/imagine4-after-move.trace.jsonl`.  The live instance was
left running for interactive testing.  Continue breadth testing from this
working screen; do not return to the already-fixed list symptom.

## JIT staging-buffer overflow (2026-08-08) - FIXED

Photogenics regressed to `Error 0x80000009 - Trace error` immediately after
`Input()` returned.  SEVEN A/Bs (each a real rebuild + rerun) cleared: segment
frame 8/4/ZERO bytes, the PROGDIR prelude, the whole OS module at 026038f40e,
the aliased-MOVE codegen rewrite, and the OS crossings at d3a62c845a
(pre-regeneration).  None of them.

The tell: the fault PC `0x18C88E140` was BYTE-IDENTICAL across every dylib
rebuild - so it is not in our binary.  `atos` gives
`__chk_fail_overflow (libsystem_c.dylib)`, the _FORTIFY_SOURCE guard.  lldb
ATTACHED to a correctly-booted instance (launching under lldb fails - the
harness sets DYLD_FALLBACK_LIBRARY_PATH and entitlements; and AROS's own
SIGUSR1/SIGALRM must be passed through with
`process handle -n false -p true -s false ...`, then `thread select 2`) gives:

    __chk_fail_overflow <- __memcpy_chk <- libemu68k.dylib`j5d_run_inner+4212

In `hosted/jit68k/j5d_engine.c`:

    uint32_t staging[8192];        /*  32768 B - DESTINATION            */
    static uint32_t body[65536];   /* 262144 B - source                 */
    memcpy(ptr, body, body_words * sizeof(uint32_t));   /* unbounded    */

The decode loop bounded `body` against ITS OWN size; the composed block is
copied into the caller's `staging`, eight times smaller.  Disassembly confirms
it: `mov w3, #0x7f44` = 32580 = staging minus the 47 prologue words already
emitted.  A STACK BUFFER OVERFLOW that had been silently corrupting memory
under every large block; it only surfaced because the SDK moved mid-session
(Xcode -> CommandLineTools) and the rebuild enabled _FORTIFY_SOURCE.  Suspect
this as a cause of unexplained flakiness before 2026-08-08.

Fix: the decoder bounds itself against the DESTINATION
(`J5D_STAGING_WORDS - J5D_COMPOSE_RESERVE`), a hard check guards the memcpy so
an oversized block is a NAMED refusal, and `staging` is sized to match `body`
and made `static` (256 KB on an AROS task stack would be its own crash; the
engine is documented single-runner, which is what makes a shared buffer sound).

Verified against `graft/68k-corpus` (genexecfull PASS, the three negative
controls still failing closed, no other line changed) and TurboCalc 5, which
draws its full spreadsheet and registers the TCALC port.  The sweep also shows
what the corruption had been costing: `gengadget` used to report a bare "host
fault in translated code" and now names its real gap again ("stale or unknown
GA_Previous object token"), so a silent overflow had been eating a diagnosis.

OPEN: `pgsdemo.library` (PhotoDemo's own, in `PhotoDemo/libs/`) is not found.
Neither `Assign LIBS: ... ADD` nor `EMU68K_LIBS_PATH` made it resolve, and NO
`OpenLibrary("pgsdemo...")` appears in the log - so it is opened by a path form
neither route covers.  Launch/resolution question, not the JIT bug.

## The Workbench startup message was never taken (2026-08-08) - FIXED

Found while verifying the above: Imagine 4 launched with `WBRun` died on
`0x07000004 - unexpected DOS packet received` in dos.library `dopacket`, while
the SAME program launched from the shell ran perfectly.  That difference is the
whole diagnosis - it is the launch, not the program.

`dopacket` alerts `AN_AsyncPkt` when `internal_WaitPkt` returns something other
than the packet it just sent.  A Workbench launch (`OpenWorkbenchObject` ->
workbench.library `handler.c`) does `CreateNewProc` and then `PutMsg`es a
WBStartup to the new process's own port.  Native programs take it in their
startup code (`compiler/autoinit/fromwb.c`); a routed 68k program had nobody to
do it, so the message sat on `pr_MsgPort` and the first DOS call on that process
collected it instead of its reply.

It only became visible on 2026-08-08 because the PROGDIR prelude - which issues
a `Lock` at the top of the first OS call - was the first thing to make a DOS
call that early.  Before that the collision happened later or not at all.  Note
the trap when dating this: the AROS-side `emu68k.library` in the boot image is
built separately, so a source commit can be days older than the first run that
has it.  Compare the MODULE's mtime, not the commit date.

`Emu68k_RunSeg` now does what a Workbench program does: takes the message,
adopts `sm_ArgList[0].wa_Lock` as the current directory (a Workbench process
starts with none), hands the argument names and lock tokens to the guest
through `emu68k_run_set_workbench` - built earlier and until now never called
from AROS - and replies the message under `Forbid()` at end of run, including
on the two early-exit paths.  The wait accepts `SIGBREAKF_CTRL_C` as well as
the port signal: a process can reach here with no CLI and no Workbench behind
it, and an unkillable launch is worse than one without its arguments.

## Photogenics: a program that loads its own font (2026-08-08) - OPEN

`Could not open pgsdemo.library!` was never a resolution bug in the bridge.  The
trace (`EMU68K_TRACE_CALLS=1`, then grep `OpenLibrary`) shows the program asking
for `photodemo:demodata/libs/pgsdemo.library` - its own volume name, by a path
nothing had assigned.  Two lines of launch recipe fix it, and they belong with
any package that was shipped as a bootable disk:

    Assign PhotoDemo: MacRW:PhotoDemo
    Assign LIBS:  PhotoDemo:libs  ADD
    Assign FONTS: PhotoDemo:fonts ADD

The lesson is cheap to reuse: when a library "will not resolve", get the NAME
the program actually passed before touching the resolver.  It asked for
something nobody had assigned; no search path could have found it.

With those, pgsdemo, asl, gadtools, colorwheel.gadget and workbench.library all
open and the program reaches its real blocker:

    LoadSeg("FONTS:photogenics/8") -> entry=00397818
    graphics.library LVO 136 (-816) ExtendFont  a0=00397852   -> capability gap
    ... then: stale or unknown TextFont object token 00397852

Photogenics loads a bitmap font itself and hands graphics the `struct TextFont`
inside the segment it just loaded.  That font is guest memory with guest
pointers in it (tf_CharData, tf_CharLoc, tf_CharSpace, tf_CharKern), and
graphics is bridged, so the crossing wants a native TextFont it issued.  This is
a facade in the direction we have not built: a structure the GUEST owns crossing
into native code.  Serving it means constructing a native TextFont over
host-visible copies of the guest glyph data, keying it to the guest address, and
implementing ExtendFont.  Routing diskfont natively instead does NOT help - the
font never went through diskfont - and makes it fail later and less clearly.

Any program that ships its own bitmap fonts lands here, so it is worth doing
properly rather than per-application.

## Seglist framing for the top-level program (2026-08-07) - FIXED

Found by running an arbitrary Aminet-style tool (`~/Downloads/Test`, a
self-decrunching executable).  It was refused with "needs the Amiga hardware
(machine address $025EDC)", which READS like a routing verdict and is not
one.  That message came from the RUNTIME guard, not the static scan - the two
texts differ only by an "it ", so check which one you are reading.

The program is crunched: the header declares hunk 1 as 0x1DAA4 bytes while
only 0x12508 are in the file (the rest is the decrunch target), and there are
ZERO RELOC32 blocks.  A program with no relocations can find its later hunks
only one way, by walking its own DOS seglist:

```
250008  lea (pc,0x24fffc),a4   ; own hunk start MINUS 4 = the link word
25000c  move.l (a4),a0         ; the next-hunk BPTR
25000e  add.l a0,a0
250010  add.l a0,a0            ; BPTR<<2
250012  addq.w #4,a0           ; skip the link -> next hunk's payload
```

`emu68k_dos_loadseg` (a guest's own LoadSeg) builds exactly that chain through
`j4_load_hunks_bptr`, which reserves a 4-byte link word before each hunk and
chains them.  The TOP-LEVEL program was loaded with plain `j4_load_hunks`,
zero headroom, so no link words existed: the program read whatever preceded
its hunk (`0x4E77`) as the BPTR, computed a wild address, and the guard
stopped it.  Fixed by loading the top-level program through the framed loader
as well, keeping `seg_bptr` on the run.

Measured effect: the program now finds hunk 1 at its real address and
decrunches, and in the corpus `genexecfull` went FAIL -> PASS.  So the gap was
never specific to crunched programs.  Self-extracting archives, most Aminet
game and demo releases, and anything overlaid all depend on this.

CAVEAT, measured not assumed: the same corpus run shows `gengadget` changing
from a NAMED gap (`stale or unknown GA_Previous object token 7a2e666f`) to a
bare "host fault in translated code".  It failed before and fails now, but the
diagnosis got WORSE, and the framing shifted every address (library base
`0x222000` -> `0x262040`).  Check that first when picking up GA_Previous.

The tool still does not finish.  It now dies later, in the decruncher's
copy-back loop at `0x2500f4` (`move.l (a0)+,(a2)+ ; subq.l #1,d1 ; bne`),
whose count is computed at `0x2500e0` as `d1 = (d3 - a0) >> 2`.  At the fault
`D3 = 0x25027C` (the end of its own stub hunk) and `A0 = 0x00BF187C` (near the
arena TOP, an allocation), so the subtraction underflows to `0x3FD97A80` - a
billion longwords - and the copy runs off the arena.  `a0` therefore holds a
HIGH allocation where the code expects a pointer inside its own loaded image.
Establish where `a0` gets that value, and whether the program relies on
allocations landing adjacent to its hunks the way a single-pool Amiga gives
them, BEFORE moving the heap to suit it.

`EMU68K_SCAN_OVERRIDE=1` was added while chasing this: it runs a program the
static scan calls a hardware banger, loudly, so a real refusal can be told
from a linear-decode artefact on a data payload.  Diagnostic only; the runtime
guard remains the authority.

SECOND HALF OF THE FRAME (2026-08-07 evening): the link word alone was NOT
the full contract.  Real LoadSeg allocates each segment as
`[length][link BPTR][payload]` - the length longword at `BADDR(seg)-4` is how
UnLoadSeg frees a segment, and programs READ it: this same decrunch stub does
`move.l -8(payload),d3; add.l a0,d3` to find the END of its own crunched
hunk.  With only the 4-byte link framed, that read returned garbage, `d3`
came out as ~stub-end instead of ~stream-end, and the per-hunk compaction
count `(d3-a0)>>2` underflowed to a billion longwords; the runaway copy
marched into the arena guard page (the crash dump's `x13=0xBFC000` names it).
`j4_load_hunks_bptr` now frames EIGHT bytes and writes `hunk_size+8` at
`link-4`, for the top-level program and guest LoadSeg alike.  Corpus verified
identical before/after the widening (genexecfull still PASS, all three
negative controls still fail closed).

ENV FORWARDING TRAP (cost one wrong conclusion, now fixed): `aros-ctl run`
launches through launchd, which STRIPS the environment; only the variables
listed in the plist block around line 698 of `graft/aros-ctl` reach the
instance.  `EMU68K_SCAN_OVERRIDE` and `EMU68K_DEAD_CHIPS` are now in that
list.  Any new `EMU68K_*` control MUST be added there or it silently does
nothing under `aros-ctl run` while working fine in standalone harnesses -
check the banner line in the log, not the shell environment, when a control
seems ignored.

`EMU68K_DEAD_CHIPS=1` (new, diagnostic): leaves the hardware windows as
inert zeroed RAM instead of PROT_NONE fault holes.  A program that only POKES
the chips then runs; reads see zeroes.  For finding out what a hardware
program IS, not for pretending the hardware exists - a busy-wait on a
changing register (VPOSR, CIA timers) spins forever and that too is an
answer.  It prints a banner and suppresses all chip-access reporting for the
run.

FINAL VERDICT ON THE TOOL (`~/Downloads/Test`): it is a HARDWARE-LEVEL
music/demo program, exactly as suspected.  Fully decrunched and running, its
entire OS footprint is: AllocMem/FreeMem (its own re-loader), `Forbid`,
`OpenLibrary("graphics.library")`, `LoadView` (display takeover), two
`WaitTOF` calls to settle the frame - and then NOTHING: its main loop lives
entirely in the custom chips through `A5=$DFF000`, at 96% CPU against dead
registers.  Under `EMU68K_DEAD_CHIPS` it runs but can produce no sound (Paula
is inert) and can never be exited by input (the CIAs are dead too).  This is
the genuine "needs a full Amiga emulator" class - the WATERLINE boundary, not
a bridge gap - now demonstrated at runtime instead of asserted by a scan that
was, on this file, wrong about WHERE (it flagged the crunched payload, not
the real chip code).  Do not grow Paula/CIA emulation into the bridge for it;
that is the standing waterline decision.

Loose end, recorded not chased: one earlier run with the FULL fifteen-library
tail set ended in a clean instance EXIT (launchd status 0, no crash bundle,
no diagnostic report) after the tool ran under dead chips.  Not reproduced
with the minimal set.  If it recurs, suspect the payload's exit path (a
classic tool may jump through a reset vector) interacting with layout.

## Regression sweep, 2026-08-07 (current result)

Run `make hosted-emu68k-t3gen` for the generated/handwritten boundary suite and
`make hosted-emu68k-t3mui` for the separately loaded Zune class.  The corpus
harness refuses to replace a live Macaros instance unless an operator opts in;
do not set that override for routine verification.  Every outcome below is a
named verdict, none an unexplained hang:

| fixture | verdict |
|---|---|
| `genlibsweep` | PASS - gadtools, iffparse, locale, icon, datatypes, asl, diskfont, commodities all LOADED guest-side |
| `genmenuitem` | PASS |
| `genowngadget` | PASS |
| `gengadgetbad` | refused as designed (unknown object token: memory the program owns, not a token this run issued) |
| `genowngadgetbad` | refused as designed (Image outside guest memory) |
| `genowngadgetcycle` | refused as designed (Gadget family exceeds 4096 members or contains a cycle) |
| `gengadget` | PASS - `GA_Previous`/linked facade identity is adopted correctly |
| `geniff` | PASS - real write/read round trip |
| `genexecfull` | PASS - including AllocTrap/FreeTrap through an extended Task's ETask |
| `genmui` + `Busy.mcc` | PASS - external 68k MCC init, create through guest dispatcher, and dispose |

The previously recorded `gengadget`, `geniff` and `genexecfull` failures are
fixed.  The full `hosted-emu68k-t3gen` gate and the separate external-class
`hosted-emu68k-t3mui` gate are green; unsafe-input controls still fail closed.
The remaining corpus milestone is application breadth, not these fixtures.

## Repo state — IMPORTANT

The host/graft repo is `main` in the user's own `jonx/AROS-AArch64` repository;
the pre-this-update commit is `599036e`. The AROS checkout remains based at
`b594c9ba09` on `checkpoint/emu68k-progdir-20260807`, but has the uncommitted
OS-side generated files and runtime changes paired with this host update.
Do not commit or push that checkout: authorization is only for the user's own
`jonx` repositories. Preserve any new parallel work discovered in either tree.

- `hosted/jit68k/j4_loader.c` + `j4_hunk.h`: HUNK_RELOC32SHORT/DREL32 support.
  This is what let five tail libraries load at all.
- `hosted/emu68k/emu68k_exec.c`: the exec batch — SetFunction (patches a guest
  library's vectors; for a BRIDGED library it records an override and later
  calls redirect into the guest routine, which is how locale's RawDoFmt patch
  works), semaphore family via generated names, MakeLibrary/MakeFunctions/
  InitStruct, AllocEntry/FreeEntry, CreateIORequest, RawPutChar, TypeOfMem,
  FindName, Insert, NewMinList, registration no-ops, AddMemHandler, and a
  fallback that names the vector.
- `hosted/emu68k/emu68k_{dos,graphics,intuition,layers,utility,cybergraphics}.c`
  plus `emu68k_internal.h`: library-local handwritten semantics extracted from
  the host core. `emu68k_host.c` is now about 3,000 lines, not the old roughly
  4,700.
- `hosted/emu68k/emu68k_taskresource.c` and `emu68k_timerdevice.c`: task
  storage/hooks and timer arithmetic/time queries are separate generic modules;
  task-list inspection still fails explicitly because native task identities
  are not retained guest objects.
- `graft/gen-emu68k-bridge`: truthful callable/refusal reporting, checked
  generic buffers and arrays, deep structure conversion, guest-owned nested
  objects, returned guest pointers and C strings, generated OS LVO constants,
  readable returned Gadget facades (including linked identity and guest-value
  fields), reviewed partial-overlay ownership, and handwritten validation
  across every waterline library plus the GadTools adapter.
- `graft/gen-struct-layouts`, `graft/emu68k-bridge-policy.json`, generated host
  headers, and generated files under
  `aros-upstream/arch/all-darwin/libs/emu68k/`: the complete six-library type,
  object, layout, function, and refusal inventory.
- `aros-upstream/.../emu68k_oscall.c`: native-side special cases including DOS
  guest-pointer handling, graphics pixel buffers, Intuition retained facades,
  cybergraphics CMode lists/framebuffer locks, recursive BOOPSI superclass
  creation/disposal, persistent guest-class dispatchers for later native input
  methods, and released-facade tombstones, with paired end-of-run cleanup. A
  released facade can no longer be mistaken for a guest-owned structure merely
  because both addresses are readable inside the arena.
- `hosted/emu68k/nativelib/genexecfull.s`, `geniff.s`, `genlibsweep.s`,
  `genmui.s`, `genaslsave.s`: new fixtures. `genaslsave` is interactive and
  validates both ASL save mode and the returned FileRequester facade strings.
- Native library/device facades no longer occupy a fixed sixteen-slot address
  range.  Each gets a private dynamically allocated 4 KiB negative-vector
  window plus readable base fields; the runtime holds 64 native facades and the
  JIT recognizes 128 total native/guest bases.  This is what let the external
  Busy class open its seventeenth native dependency (`layers.library`) without
  colliding with the arguments or in-guest OS-code regions.
- `Makefile`: every per-library handler is compiled and all seven waterline
  validators plus the GadTools adapter validator are wired into the generation
  gate. The refusal negative control now targets the still-unsafe raw
  `GT_FilterIMsg`, not the now-supported `GT_GetIMsg` path.

Latest verified state: `make emu68k-dylib`, `make hosted-jit68k-j5d`,
`make hosted-jit68k-j5m`, `make hosted-jit68k-j5r`,
`make hosted-emu68k-t3setsignal`, `make hosted-emu68k-t3workbench`, the complete
`make hosted-emu68k-t3gen`, focused `geniff`, and
`make hosted-emu68k-t3mui` pass.  The latter builds only the named
`workbench-classes-zune-busy` m68k target, converts the ELF to HUNK, packages
the MCC as a PROGDIR fixture and runs it inside booted AROS.  Deploying ad-hoc
signs the dylib, so the installed file's hash is expected to differ from the
unsigned build artifact even when its code is current.

## Next steps, in order

1. Build the Aminet breadth corpus and make every remaining failure a named
   routing verdict rather than a bridge capability gap. This is the goal's
   real proof.
2. Turn the ten-library load sweep, `geniff`, built-in/external `genmui`,
   generator checks and
   breadth corpus into one fresh-nightly command.
3. Add focused behavioural fixtures for the new DOS command/packet paths,
   GELS, Intuition retained lists, Layers callback adapters, cybergraphics
   staging locks, task.resource storage and timer.device arithmetic.
4. Revisit TurboCalc application-side ARexx dispatch only with a replayable
   application oracle.  Generic ARexx request/reply transport is already
   proven; the demo binary's main task does not drain `TCALC` while a sheet is
   open, so do not fake an app-specific completion in the bridge.

Parked, unchanged: the wild jump at 68k PC `0x29F6B2`; DoIO device marshalling.
`GA_Previous`/facade-previous adoption is no longer parked: PhotoDemo's
multi-gadget Palette and Tools lists exercise it successfully.
