Start the new chat with:
Read docs/features/68k-transparent-exec/HANDOVER.md completely, inspect both dirty working trees, then continue from “TurboCalc live breadth probe — resume here.”

# Handover: 68k transparent execution, 2026-08-06

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
| `~/Source/aros-upstream` | the AROS OS source; OS-side code lives here, incl. `emu68k.library`. **Current checkout is dirty on `exfat-handler`, not the intended bridge branch `aarch64-darwin-graft`; do not commit the bridge changes there without first moving them safely.** |
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
guest-readable facade, and disposes it back through the same chain. Nested
native-to-guest callbacks use depth-indexed callback stacks, so recursive
superclass dispatch does not overwrite an outer callback frame.

Fixtures: the complete `hosted-emu68k-t3gen` gate passes, including
`genexecfull`, `gengadget`, `genmenuitem`, `genowngadget`, and the negative
controls for stale facades, unsupported fields, invalid callbacks and cyclic
families. `geniff` PASSes its real write/read round trip and `genmui` PASSes
create/dispose. The ten-library sweep reports `LOADED` for every listed tail
library. `stdc.library`, `posixc.library`, `rexxsyslib.library` and
`rexxsupport.library` are installed guest-side as dependencies rather than
counted as separate sweep targets.

The overall goal is **not achieved yet**. The original tail libraries now all
load and MUI has a real lifecycle proof, but the external Zune class set and
the Aminet breadth corpus have not completed a clean sweep, and verification
is not yet a single fresh-nightly command. The waterline inventory itself is
complete: 738/738 vectors are classified and there are no unknown crossings.

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

### TurboCalc live breadth probe — resume here

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
while its converted planar data is safely replaceable.  TurboCalc then exposed
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
time), and judge only `STAGE2-PASS`/`FAIL` plus the trace: expect a
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
restarted when needed; remove or rename the previous
`Regina68k/stage2.result` before judging a new run.  A successful boot alone is
not a Stage 2 proof: retain the fresh Bridge Lab trace and verify that the
result file contains `STAGE2-PASS` with both formula and value checks.

If a boot reaches only a blank Cocoa screen and logs `Could not open version
36 or higher of library "dos.library"`, it is an independent staged-OS boot
artifact issue: rebuild only `kernel-dos` through `graft/rebuild-aros.sh`, then
restart.  Do not misclassify that pre-Startup failure as an emu68k result.

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

## Corpus sweep, 2026-08-07 (full tail set, one boot)

`CORPUS_REPLACE_LIVE=1 graft/68k-corpus build/t3all` with all fifteen tail
libraries routed guest-side.  Result file `~/AROS/Shared/corpus-final.txt`.
Every outcome is a NAMED verdict; none is an unexplained hang, and none is a
frozen instance (which before the idle-starvation fix is what several of
these would have been):

| fixture | verdict |
|---|---|
| `genlibsweep` | PASS - gadtools, iffparse, locale, icon, datatypes, asl, diskfont, commodities all LOADED guest-side |
| `genmenuitem` | PASS |
| `genowngadget` | PASS |
| `gengadgetbad` | refused as designed (unknown object token: memory the program owns, not a token this run issued) |
| `genowngadgetbad` | refused as designed (Border outside guest memory) |
| `genowngadgetcycle` | refused as designed (Gadget family exceeds 4096 members or contains a cycle) |
| `gengadget` | FAIL - `stale or unknown GA_Previous object token 7a2e666f`.  That token is ASCII `z.fo`, i.e. the tail of "topaz.font": a font NAME is being read where a Gadget token belongs.  This is the parked GA_Previous/facade-previous item and it is NOT fixed; the ASCII value names the confusion exactly. |
| `geniff` | FAIL - known FIXTURE bug, not a bridge gap: `geniff.s` passes PushChunk's arguments in the wrong order (type then id; a plain chunk has type 0). |
| `genexecfull` | FAIL - `[T3EXECFULL]`, unresolved; next to triage. |

Three real defects, each named; three negative controls all failing closed;
three passes.  The two FAILs worth engineering are `gengadget` (GA_Previous)
and `genexecfull`; `geniff` is a one-line fixture correction.

## Repo state — IMPORTANT

The pre-TurboCalc graft baseline was pushed `1ea5065`; the interactive bridge
checkpoint is pushed as `5ef3cfb` on `origin/main`.  The current AROS checkout
HEAD is `026038f40e` on `exfat-handler` and remains dirty so parallel work is
not disturbed.  Its exact 15-file emu68k diff is safely committed and pushed
as `589340275f` on `fork/checkpoint/emu68k-interactive-20260806`, based on that
exFAT HEAD.  Reconcile that checkpoint with the intended
`aarch64-darwin-graft` history before making it the permanent bridge branch.
There are unrelated existing changes (notably Moonstone audio in this repo and
the exFAT branch context in the OS checkout); preserve them.  The user has
authorized committing and pushing work in their own `jonx` repositories once
it is stable, but that is not permission to publish elsewhere or to mix bridge
changes into the wrong AROS branch.

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
  `genmui.s`: new fixtures.
- `Makefile`: every per-library handler is compiled and all seven waterline
  validators plus the GadTools adapter validator are wired into the generation
  gate. The refusal negative control now targets the still-unsafe raw
  `GT_FilterIMsg`, not the now-supported `GT_GetIMsg` path.

Latest verified state: `make emu68k-dylib`,
`make hosted-emu68k-t3setsignal`, `make hosted-emu68k-t3hello`, and
`make hosted-jit68k-j5d` pass.  The current full
`hosted-emu68k-t3gen` run has one negative-control mismatch:
`T3OWNGADBAD` still refuses the unsafe input, but the fixture expects a
different field/error.  Do not weaken the refusal to make the assertion pass;
update either the focused validation or its expected diagnostic after the
Image work.  The deployed `~/lib/libemu68k.dylib` matches the build artifact
at SHA-256
`bc0fb37074b5b72a38c148e14136ab3b6ed0ae33ae02af4ccc4e0b079396f269`.

## Next steps, in order

1. With an explicitly allowed Macaros restart, deploy the rebuilt dylib and
   rerun the private Stage 2 tree with reply-port tracing.  Follow the exact
   request/reply chain from root reply port through RexxMast/RX/TurboCalc; do
   not add an application-specific workaround.
2. Restore the full `hosted-emu68k-t3gen` gate without weakening its unsafe
   input negative controls.  Run `git diff --check` in both trees.
3. Reconcile pushed checkpoint `589340275f` from its current `exfat-handler`
   base onto the intended bridge branch; do not disturb the shared dirty
   checkout while parallel work is active.
4. Copy/route the external Zune class binaries and extend `genmui` from the
   built-in `Text.mui` lifecycle to at least one separately loaded class.
5. Build the Aminet breadth corpus and make every remaining failure a named
   routing verdict rather than a bridge capability gap. This is the goal's
   real proof.
6. Turn the ten-library load sweep, `geniff`, `genmui`, generator checks and
   breadth corpus into one fresh-nightly command.
7. Add focused behavioural fixtures for the new DOS command/packet paths,
   GELS, Intuition retained lists, Layers callback adapters, cybergraphics
   staging locks, task.resource storage and timer.device arithmetic.

Parked, unchanged: the wild jump at 68k PC `0x29F6B2`; DoIO device marshalling.
`GA_Previous`/facade-previous adoption is no longer parked: PhotoDemo's
multi-gadget Palette and Tools lists exercise it successfully.
