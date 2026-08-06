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
   commodities.library,muimaster.library,stdc.library,posixc.library,\
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

The immediate blocker is now precisely identified.  TurboCalc reuses guest
`struct Image` at `0x0028600c` with a different planar size, then calls
Intuition `DrawImage` (LVO 19).  The OS-side mirror currently refuses with:

```
capability gap: Image 0028600c changed planar data size after its mirror was created
```

That refusal occurs before native `DrawImage`, so it is safe and is likely the
cause of both the black cell-selection rendering and the subsequent process
exit.  Fix it generically in
`~/Source/aros-upstream/arch/all-darwin/libs/emu68k/emu68k_oscall.c`: keep the
native `struct Image` address stable (menus/gadgets may retain it), but own its
endian-converted `ImageData` as a separately replaceable allocation.  On each
crossing validate the new geometry, allocate the new buffer first, then swap
and free the old buffer, copy the guest words, and retain the existing Image
object identity.  Update rollback and teardown to free both allocations.  A
large patch attempting this was **not applied** (`apply_patch` context check
failed), so the source still contains the original refusal and is a clean
starting point for this fix.

The decisive failed-run trace is
`~/AROS/Shared/turbocalc5-bridge-51.trace.jsonl`; screenshots are under
`run/darwin-aarch64/turbocalc51-*.png`.  Trace 51 shows the menu result being
consumed before the Image-size refusal.  At handover time Macaros is running a
plain AROS shell, not TurboCalc; leave the final application test instance
alive for the user.  Intermediate restarts are explicitly allowed.

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

## Repo state — IMPORTANT

Current graft HEAD is pushed `1ea5065` (`origin/main`).  The AROS checkout HEAD
is `026038f40e` on `exfat-handler`; the bridge work belongs on
`aarch64-darwin-graft`, so reconcile the dirty OS tree before committing.  The
runtime work below is **uncommitted** in both working trees.  There are also
unrelated existing changes (notably Moonstone audio in this repo and the exFAT
branch context in the OS checkout); preserve them.  The user has authorized
committing and pushing work in their own `jonx` repositories once it is stable,
but that is not permission to publish elsewhere or to mix bridge changes into
the wrong AROS branch.

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

Latest verified state: `make emu68k-dylib`, explicit
`make hostlibs-emu68k`, `make hosted-emu68k-t0p3`, and
`make hosted-emu68k-t2guard` pass.  The current full
`hosted-emu68k-t3gen` run has one negative-control mismatch:
`T3OWNGADBAD` still refuses the unsafe input, but the fixture expects a
different field/error.  Do not weaken the refusal to make the assertion pass;
update either the focused validation or its expected diagnostic after the
Image work.  The deployed `~/lib/libemu68k.dylib` matches the build artifact
at SHA-256
`bc0fb37074b5b72a38c148e14136ab3b6ed0ae33ae02af4ccc4e0b079396f269`.

## Next steps, in order

1. Implement mutable, stable-address guest Image mirrors as described in the
   TurboCalc section; rebuild/deploy, reproduce New/menu/cell selection and
   keyboard entry, and leave the working TurboCalc instance alive.
2. Restore the full `hosted-emu68k-t3gen` gate without weakening its unsafe
   input negative controls.  Run `git diff --check` in both trees.
3. Move the dirty OS bridge changes safely from the current `exfat-handler`
   checkout to the intended bridge branch before committing anything there.
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
