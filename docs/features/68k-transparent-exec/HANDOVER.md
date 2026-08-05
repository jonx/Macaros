# Handover: 68k transparent execution, 2026-08-05

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
| `~/Source/aros-upstream` | the AROS OS source (branch `aarch64-darwin-graft`); OS-side code lives here, incl. `emu68k.library` |
| `~/aros-build` | the build tree (`make hostlibs-emu68k` here builds the OS-side module) |
| `~/aros-crosstools` | the prebuilt cross toolchain — never rebuild it |
| `~/Source/references/aros-m68k-20260804/libs` | the real m68k library binaries we run guest-side |
| `hosted/jit68k/` | the 68k JIT engine (`j5d_engine.c`), hunk loader (`j4_loader.c`), diagnostics (`j5n_diag.c`) |
| `hosted/emu68k/emu68k_host.c` | the host side of the bridge: exec dispatch, guest memory, guest-library loading |
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

**The waterline is complete.** Seven libraries are bridged to native AROS:
exec, dos, graphics, intuition, layers, utility, cybergraphics (plus the
timer/input/clipboard devices and a kernel.resource stub). *Everything* above
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
   - `--validate-handwritten hosted/emu68k/emu68k_host.c` — constants match the
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
   icon.library,datatypes.library,asl.library,diskfont.library,commodities.library" \
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

Waterline coverage (`--gaps`, plus exec counted by hand-dispatch):

| library | public | served |
|---|---|---|
| exec | 146 | 109 (hand-written dispatch) |
| dos | 160 | 91 |
| graphics | 183 | 140 |
| intuition | 143 | 103 |
| layers | 40 | 27 |
| utility | 42 | 17 |
| cybergraphics | 24 | 11 |

Every remaining public vector still emits a **named refusal** saying which type
it needs, so nothing fails as a bare number.

Tail set, routed guest-side (one boot, `build/t3all`):

| library | status |
|---|---|
| gadtools | LOADED, and fully exercised: gadgets + menus |
| iffparse | LOADED |
| locale | LOADED (needed exec `SetFunction`) |
| icon | LOADED |
| diskfont | needs exec `AddMemHandler` (implemented, unverified) |
| datatypes | declines: its init returns NULL around the NamedObject namespace |
| asl | declines: same shape |
| commodities | not reached yet |

Fixtures: `gengadget` PASS, `genmenuitem` PASS, `genowngadget` PASS, and three
negative controls fail closed by name (`gengadgetbad` stale token,
`genowngadgetbad` unsupported field, `genowngadgetcycle` family cycle).
`geniff` FAILs — a fixture bug of mine, not a bridge gap; its IFF chunk
arguments are still wrong.

## Repo state — IMPORTANT

Last pushed: graft `40fd924`, fork-graft `bd524a0924`. **Everything below is
uncommitted** in the working trees:

- `hosted/jit68k/j4_loader.c` + `j4_hunk.h`: HUNK_RELOC32SHORT/DREL32 support.
  This is what let five tail libraries load at all.
- `hosted/emu68k/emu68k_host.c`: the exec batch — SetFunction (patches a guest
  library's vectors; for a BRIDGED library it records an override and later
  calls redirect into the guest routine, which is how locale's RawDoFmt patch
  works), semaphore family via generated names, MakeLibrary/MakeFunctions/
  InitStruct, AllocEntry/FreeEntry, CreateIORequest, RawPutChar, TypeOfMem,
  FindName, Insert, NewMinList, registration no-ops, AddMemHandler, and a
  fallback that names the vector.
- `graft/gen-emu68k-bridge`: object inference, `--gaps`,
  `--validate-handwritten`, generated exec LVO constants + name table, the
  `cstr` facade-content kind, nested facade content, and the trailing-offset
  fix.
- `graft/emu68k-bridge-policy.json`: 32 opaque handle types + Message,
  `utility.named_object` domain, AllocNamedObjectA/RemNamedObject.
- `hosted/emu68k/nativelib/geniff.s`, `genlibsweep.s`: new fixtures.
- `Makefile`: `--validate-handwritten` wired into the t3gen gate.

**One conflict is pending and must be resolved before the next build:**
`exec.library.ObtainSemaphoreShared` and `AttemptSemaphoreShared` have BOTH a
crossing policy (`objects: sigSem`) and a new reviewed refusal, which
`--check` rejects. They are hand-written and must stay so: delete their
entries from `functions` in the policy, keep the refusals.

## Next steps, in order

1. Resolve the policy conflict above, run the four static checks, then ONE
   build and ONE test pass.
2. datatypes and asl: both decline during init around the NamedObject
   namespace. Diagnose from a traced run (`EMU68K_TRACE_CALLS=1`) and fix as
   one batch, not one vector at a time.
3. Fix `geniff`'s chunk arguments (PushChunk takes type then id; a plain chunk
   has type 0), then it becomes the iffparse behaviour gate.
4. Then commodities, then muimaster/Zune, then the Aminet corpus sweep, which
   is the goal's real proof.

Parked, unchanged: the wild jump at 68k PC `0x29F6B2`; DoIO device marshalling;
`GA_Previous` adopt with a facade previous (multi-gadget programs).
