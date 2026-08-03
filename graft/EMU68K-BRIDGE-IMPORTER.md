# Source-driven 68k bridge importer

The importer turns an AROS native library implementation into evidence for the
68k-to-native bridge. Its unit of work is a library, not the next missing LVO
encountered by an application.

The stable import command is:

```sh
graft/gen-emu68k-bridge --import-library gadtools
```

It writes six artifacts under `build/emu68k-import/gadtools/` by default:

- `analysis.json` is the complete public-vector and reachable-helper evidence.
- `manifest.json` is the deterministic import contract: source/build hashes,
  accepted facts, per-vector outcome, and unresolved review items.
- `review.md` is the same finite review queue in a human-readable form.
- `policy.generated.json` contains the active, already-approved policy subset and
  non-activating candidate tag domains. Payloads without an approved conversion
  are emitted as `refuse`.
- `bridge.generated.c` is the compilable crossing source produced from the
  currently approved policy and ABI-only vectors. Its default path remains
  fail-closed for every other LVO.
- `tests.generated.json` contains deterministic positive, negative, object-token,
  tag-refusal, and lifecycle contracts. Contracts whose crossing is incomplete
  remain marked `blocked`; they are not reported as passing tests.

Use another output root when needed:

```sh
graft/gen-emu68k-bridge --import-library gadtools --output-dir /tmp/gadtools-import
```

Verify that checked artifacts still describe the current source and build:

```sh
graft/gen-emu68k-bridge --check-import gadtools
```

The check regenerates all six artifacts in memory and fails if any file is
missing or stale. The lower-level source-analysis stream can be inspected with:

```sh
graft/gen-emu68k-bridge --analyze-source gadtools > /tmp/gadtools-analysis.json
```

The command currently stops after candidate/code/test-contract generation. It
does not yet mutate the live bridge policy or claim certification. This document
distinguishes implemented behavior from the remaining pipeline so a successful
import is not mistaken for a certified bridge.

## Inputs and discovery

The library name is resolved by scanning every `.conf` in the AROS source tree;
it is deliberately not restricted by the runtime bridge's existing `GEN_LIBS`
allowlist. From there the tool discovers:

1. The library `.conf`, which is authoritative for public function names, LVOs,
   return and argument types, and 68k argument registers.
2. The generated `proto/<library>.h`, which identifies the native library base.
3. Public and private C headers below the library source root.
4. C implementation files below that source root.
5. The current AROS build's generated `*_deflibdefs.h`, include directories, and
   target sysroot. The importer follows `LC_LIBDEFS_FILE` into the generated
   libdefs header as well; `GM_UNIQUENAME` there is authoritative when a library
   relies on genmodule's default basename.
6. The AROS cross-Clang configured for `aarch64-unknown-aros`.

The explicit current sysroot is important: a cross compiler can retain a stale
build path in its defaults. The analyzer therefore reconstructs the real compile
environment instead of invoking a host compiler over isolated source snippets.
It also leaves normal AROS library-base selection enabled. Defining
`__NOLIBBASE__` for analysis would not reproduce the build: inline proto calls
would instead demand undeclared local base variables and could make otherwise
valid implementation files fail AST compilation.
A library `.conf` may contain later class configs with unrelated basenames. The
importer scopes a declared basename to the library's first config block and
otherwise derives it from generated libdefs, preventing class names from being
mistaken for public-vector symbol prefixes.

## Implemented analysis

For each non-private public vector, the analyzer:

- finds the `AROS_LH*` implementation and records its source location;
- compiles the real function with cross-Clang and consumes its JSON AST;
- records direct calls, indirect calls, parameter field reads, concrete casts,
  pointers stored into fields, allocators, releasers, and callback patterns;
- finds locally defined functions called by that vector and walks their direct
  call graph, currently bounded to 128 helpers and 12 levels;
- normalizes generated same-library calls such as
  `__inline_Intuition_OpenWindow` back to their public implementation and
  inherits that vector's direct source contract. A public-vector boundary is a
  deliberate stopping point: internal operations performed on a newly-created
  native object do not become the caller's entire semantic contract;
- maps helper parameters back to public ABI parameters using AST call-argument
  positions and local-variable origin flow; and
- propagates helper tag accesses, structure fields, casts, retained pointers,
  ownership calls, and callback evidence back to the public-vector report.
- builds a library-wide tag-consumer index so payload casts and typed assignments
  inside BOOPSI class dispatchers can corroborate tags forwarded by public
  wrappers even though dispatch is not an ordinary C call edge.
- searches the rest of the AROS source tree for the exact discovered tag macros,
  accepting explicit consumer casts and public-header type annotations as
  corroboration. Every external file that contributes evidence is included in
  the manifest input hashes.
- derives variadic API veneers from the `.conf` signature and types tag/value
  pairs at real call sites. The evidence is attached to that exact function, so
  an input value accepted by `CreateGadget` is not confused with an output
  storage pointer passed to `GT_GetGadgetAttrs`.
- discovers every record type in public signatures, locates the installed
  header containing its definition, and invokes the dual-target layout probe.
  The manifest records classic packed-m68k and native AArch64 sizes, field
  offsets, and the subset of fields actually reached through each argument.
- turns high-confidence producer/releaser pairs into library-wide object type
  candidates. Initially these are opaque handles; available facade layouts and
  linked-family cleanup requirements are retained as explicit promotion gates.

For example, `CreateGadgetA` does not itself spell out every gadget tag. It calls
helpers such as `makebutton`, `makecycle`, and `makelistview`. The interprocedural
pass includes those helpers and records which observations flow from the public
`taglist` and `ng` arguments.

Semantic origin and pointer identity are tracked separately. A local tag array
whose values were computed from `ng` semantically depends on `ng`, but it is not
an alias of the `ng` pointer. Only identity-preserving stores create retained-
pointer lifetime findings. This prevents call-scoped tag chains and copied
scalar fields from becoming false ownership blockers while still allowing a
taglist stored in a local wrapper record to reach a delegated public vector.
`NextTagItem`/`ti_Tag` switch cases retain the loop's local taglist origin, so a
wrapper such as `OpenWindowTagList` receives the complete `WA_*` domain from
`OpenWindow`, not merely tags mentioned by standalone `GetTagData` calls.
When a public API embeds that list in a record instead of accepting a TagItem
argument directly, the importer emits one `embedded-tag-domain` decision with
all observed tags. It does not manufacture dozens of independent “lost origin”
items; the required decision is conversion of the containing record field.

Every propagated observation retains the source file, line, and shortest known
call path in `via`. Repeated observations are retained when their provenance or
inferred kind differs; later manifest synthesis can choose the strongest
non-conflicting evidence without losing the audit trail.

Tag domains remain per public function and argument. A tag used for input by a
creation/setter API does not automatically describe an output/query API: in the
latter, `ti_Data` can instead be a guest pointer to result storage. Direction
must be proved before a candidate domain is activated.

For a proven scalar query, a derived domain maps an approved input `u32` tag to
`out_u32` and refuses every pointer-shaped or otherwise unsupported payload.
Generated code validates the guest destination, gives the native function an
aligned `IPTR` scratch slot, and copies the low 32-bit result back in guest byte
order after the call. This is the reusable `GT_GetGadgetAttrsA` pattern; it is
not a GadTools-only emitter branch. String pointers, object results, and other
output shapes remain refused until their own copyback contracts exist.

Record layouts are compiler results, not hand-counted offsets. The same public
headers are compiled for `m68k-unknown-elf` with Amiga two-byte packing and for
`aarch64-unknown-aros`. Header files contributing layouts are input-hashed. A
pointer-shaped field is marked explicitly because knowing its two widths is not
enough to decide whether it is a string, object token, callback, nested record,
or retained guest address.

Object inference is type-level. Once `CreateGadgetA → FreeGadgets` establishes
`struct Gadget *` as an owned object, the same candidate is reused for `previous`
and every other exact `struct Gadget *` crossing. A destructor that walks
`NextGadget`, `NextMenu`, or a similar linked field cannot be treated as releasing
one token; promotion remains blocked until family-token invalidation is generated.

Native objects can expose embedded OS objects without leaking a host pointer.
An `embedded_facade` nested field reserves the compiler-probed classic subrecord
inside the parent facade, copies the nested object's scalar fields there, and
registers that exact guest address as a typed alias of the native embedded
object. Releasing or consuming the parent also invalidates the alias. For
functions whose native call can mutate a facade, an object argument may request
`sync`; generated post-call code then copies the approved scalar fields back to
the same guest facade. Pointer-shaped fields remain governed by explicit nested
contracts and are never copied by the scalar layout table.

The GadTools import exercises both mechanisms through `Screen.RastPort` and
`DrawBevelBoxA`. The screen owns the RastPort identity and storage; GadTools sees
the real native RastPort, while the 68k caller sees the classic 100-byte facade
at offset 84 of its Screen facade.

`OpenWindowTagList` produces a classic 156-byte `Window` facade whose scalar
fields come from the generated dual-ABI layout. Common guest-read pointers are
typed nested objects: `MenuStrip`, `WScreen`, `RPort`, `BorderRPort`, `UserPort`,
`WindowPort`, and `IFont`. Reusing an existing child facade refreshes its scalar
fields without clearing already-generated nested tokens. `CloseWindow` releases
those references before consuming the Window identity. GadTools
`GT_BeginRefresh` and `GT_EndRefresh` unwrap that same Window, invoke the native
implementation, and synchronize scalar mutations back to the guest facade.
The public `intuition.open_window` tag domain supports source-proven scalar,
string, and Screen payloads; Gadget and callback-shaped tags remain explicit
fail-closed decisions.

### Terminated record arrays

The importer also recognizes a public structure pointer that a reachable helper
advances until a named field equals a macro sentinel. This is represented as a
`record_arrays` contract rather than as a library-specific function shim. The
manifest records the evidence path, sentinel, bounded maximum, compiler-probed
guest/native strides, sentinel offsets, and scalar width.

An active contract states the parts that source analysis cannot safely collapse
into “copy this pointer”:

```json
{
  "type": "NewMenu",
  "header": "libraries/gadtools.h",
  "direction": "in",
  "sentinel": {"field": "nm_Type", "value": "NM_END", "width": 1},
  "max_count": 1024,
  "reject": {"field": "nm_Type", "mask": 128, "width": 1},
  "fields": {
    "nm_Label": {"kind": "cstr_sentinel", "sentinel": "NM_BARLABEL"},
    "nm_CommKey": {"kind": "cstr"},
    "nm_UserData": {"kind": "guest_value"}
  }
}
```

Generated code scans only within the declared bound, validates every guest
record, includes the sentinel record, allocates native scratch, converts scalar
fields through the generated layout table, and applies the explicit pointer-field
rules. Scratch is released on both success and failure. `reject` is a deliberate
closed branch: GadTools image menu entries make `nm_Label` an Image pointer, so
they are rejected until an Image object contract exists instead of being
misread as text.

This mechanism is reusable by another library whose source proves the same
terminated-array shape; neither the emitter nor the layout generator contains a
`CreateMenusA` special case.

### Explicit function refusals

Some source-proven contracts cannot be represented by the mechanisms available
in the current bridge. `refused_functions` records those outcomes separately
from active crossings. Each refusal names a public `.conf` vector, carries a
bounded diagnostic reason and exact source lines, and may not overlap an active
function policy. Validation rejects stale vector names or evidence shapes.

The generated dispatcher emits a case for every reviewed refusal. Calling it
returns a named capability gap immediately, so a certified library never falls
through to an anonymous LVO and never forwards an unsafe native pointer. The
manifest counts this as `explicitly-refused`, its source observations remain
auditable, and its generated negative contract is runnable. A refusal closes a
review decision but does not claim functional compatibility; it can later be
replaced by a generated crossing once the missing generic representation is
implemented and tested.

### Source-proven no-op vectors

A void vector whose target implementation deliberately performs no runtime work
can use an active `noop` contract with exact source evidence. The generated case
does not translate, validate, or dereference any argument and does not call the
native function; it simply returns success. This is important for pointer-typed
legacy signatures such as compatibility stubs: requiring object machinery for
values the source explicitly ignores would create a false gap. Policy validation
forbids combining `noop` with any crossing rule or applying it to a non-void
result, and the generated test contract requires invalid pointer-shaped inputs
to return without being read.

## Confidence and fail-closed rules

The analyzer produces evidence; it does not silently make uncertain evidence a
bridge policy.

Source observations and active policy decisions remain distinct in the import
manifest. When a reviewed function/tag policy already names an observed tag, the
manifest records that decision as `policy_resolution` and removes the finding
from the review queue. The original conflicting or under-specified observations
remain present for auditability. A policy `refuse` is a resolved, fail-closed
decision; it is not an unresolved finding. Generated test contracts use that
effective policy kind and never claim an unavailable vector has a runnable tag
test.

- **High confidence** requires an explicit type-bearing construct, such as a C
  cast to a concrete pointer type or a string literal.
- **Medium confidence** identifies a real behavior whose bridge representation
  is not proved, such as an uncast `GetTagData` result or a `NULL` default.
- **Low confidence** establishes domain membership only, such as a tag handled
  by a `NextTagItem` switch without proved payload data flow.
- Indirect calls, polymorphic `APTR`, retained pointers, assembly-only behavior,
  and hardware-sensitive behavior become review items.

In particular, an uncast `GetTagData` result is not assumed to be an integer.
On the native target it is IPTR-sized, and a zero default can also represent a
pointer. Guessing `u32` there could pass a guest address to native code. The
importer must leave such a tag closed until another use site or an explicit
review proves its representation.

`FindTagItem` is treated differently from `GetTagData`: it returns a pointer to
the `TagItem`, not `ti_Data`. The type of the receiving variable is therefore
never used as payload evidence. This distinction is covered by the GadTools
import regression because confusing the two would falsely classify tags as
`pointer:struct TagItem *`.

Client expressions are held to the same rule. A string literal, explicit cast,
typed variable, address of typed storage, or nonzero scalar literal can provide
evidence. Bare numeric zero cannot distinguish a scalar from `NULL` and is never
accepted as type proof. Array declarations are tracked as array decay, preventing
an address-of-array spelling from acquiring a fictitious extra pointer level.

## Import artifacts and remaining certification pipeline

`--import-library <name>` now produces the source-hashed analysis, manifest,
review report, policy candidate, approved bridge C, and test contracts. The
remaining stages will consume them to produce:

1. Executable negative tests from the generated unsupported-tag and invalid
   object-token contracts.
2. Executable lifecycle tests for inferred producer/releaser pairs.
3. Callback/re-entry tests when callback contracts are accepted.
4. Promotion of reviewed candidates into the active bridge policy.
5. A certification result proving all public vectors are either generated or
   explicitly fail closed.

Certification is stronger than successful code generation. A library is
certified only when generated sources rebuild, its generated tests pass, existing
bridge regressions remain green, and every public vector has an explicit safe
outcome. Existing handwritten crossings are retired only after the generated
equivalent passes identical tests.

## Development and certification runbook

Use the public import/check commands as the front door for every library:

```sh
graft/gen-emu68k-bridge --import-library gadtools
sed -n '1,240p' build/emu68k-import/gadtools/review.md
graft/gen-emu68k-bridge --check-import gadtools
```

The repeatable promotion loop is:

1. Import the library and inspect the finite review queue and policy candidates.
2. Promote only contracts whose ABI, direction, ownership, and lifetime are
   proved; unresolved variants remain `refuse` or leave the vector unavailable.
3. Regenerate layouts and bridge sources from the policy.
4. Rebuild the native AROS bridge library.
5. Run positive lifecycle and negative fail-closed tests in booted AROS.
6. Run the existing generated-bridge and real-program regressions.
7. Re-import and use `--check-import` so the committed analysis remains tied to
   the exact generator, headers, sources, compiler configuration, and policy.

For this workspace the concrete commands after policy promotion are:

```sh
make struct-layouts
python3 graft/gen-emu68k-bridge --emit \
  ../aros-upstream/arch/all-darwin/libs/emu68k/
TARGETS="hostlibs-emu68k" ./graft/rebuild-aros.sh
make hosted-emu68k-t3gen
make hosted-emu68k-t3lha
```

`hosted-emu68k-t3gen` now includes two record-array programs. `genrecord.s`
passes a real classic `NewMenu[]` through native `CreateMenusA`, receives a typed
Menu token, lays it out through `LayoutMenusA`, and frees the Menu and VisualInfo
lifecycle. `genrecordbad.s` passes an image-valued record and must terminate with
the named `CreateMenusA.newmenu[0].nm_Type` capability gap before native GadTools
sees it. `genlayoutbad.s` independently proves that an Image-valued layout tag is
refused by `gadtools.layout_menus` before native `LayoutMenusA` runs.
`genrefused.s` calls a reviewed whole-function refusal with an otherwise unsafe
NULL argument and must terminate with the policy's exact
`gadtools.library.GT_GetIMsg refused` diagnostic before native GadTools runs.
`gennoop.s` does the converse for a source-proven no-op: it supplies deliberately
invalid Window and Requester addresses and must return successfully without
reading them or calling the native implementation.
The positive record/menu program also draws a bevel through the classic
`Screen.RastPort` embedded facade. `gendrawbad.s` proves an arbitrary address
cannot masquerade as that typed facade and reach native drawing.
`genwindow.s` opens a Screen and Window entirely through generated crossings,
checks the guest-readable Width/Height, nested Screen identity, and RastPort
facade, then executes `GT_BeginRefresh`/`GT_EndRefresh` and closes both objects.
`genwindowbad.s` supplies a forged Window token and must stop with the exact
typed-object diagnostic before native GadTools is entered.
`gengadget.s` covers the first generated linked-object family. It proves that
`CreateContext` can return a Gadget token and store that same token through a
classic pointer-to-pointer argument, then rebuilds a 30-byte `NewGadget` with
its nested `TextAttr`, strings, VisualInfo object, and guest-only user cookie.
It creates a native button and destroys the family through its context head.
`gengadgetbad.s` keeps the returned child token, frees the head, and then proves
that family destruction invalidated the child before another native call can
observe it. The implementation is driven by reusable `linked_field`,
`linked_limit`, `store_arg`, nested-structure field, and `consume_family`
metadata rather than GadTools-specific generator branches.
The positive Gadget program also exercises generated `GT_SetGadgetAttrsA` and
queries immutable `GA_ID` through `GT_GetGadgetAttrsA`, proving scalar output
tag copyback. Gadget tests run in their own boot to avoid carrying native GUI
state between otherwise independent lifecycle fixtures.

`genmenuitem.s` proves the generated guest-readable `Menu` facade and its typed
`FirstItem` field. It reads the classic field at its compiler-probed offset,
passes the resulting `MenuItem` facade to `LayoutMenuItemsA`, and then frees the
owning menu. It likewise runs in a separate boot so `LayoutMenusA` and
`LayoutMenuItemsA` never mutate the same native menu during one fixture.

A legacy-program run remains the final behavioral probe. With the local demo
corpus used during this work, PhotoDemo is run with:

```sh
EMU68K_TRACE_CALLS=1 \
CORPUS_BEFORE='Assign Photodemo: MacRW:corpus/gui__PhotoDemo.d' \
EMU68K_MAX_SECONDS=240 CORPUS_TIMEOUT=420 \
./graft/68k-corpus /tmp/aros-68k-candidates/corpus \
  build/photodemo-import.txt
```

The corpus path is machine-local; the important rule is to retain the trace and
compare the next named capability gap, not to treat “the process ran longer” as
certification.

### Agent-assisted review

The versioned skill at
`graft/skills/review-emu68k-bridge-import/SKILL.md` lets another Codex agent carry
out the finite review on the user's behalf. It binds decisions to the manifest
hash, requires exact source/layout citations, separates approved, refused, and
deferred outcomes, applies policy only when authorized, runs the certification
gates, and never converts a lower review count into a certification claim.

Invoke it as `$review-emu68k-bridge-import` after installing or exposing the
repo-owned skill directory to Codex. Its optional `agent-review.json` artifact
sits beside the import packet and records auditable per-review-ID decisions.

### Current GadTools checkpoint

At this checkpoint all 19 public GadTools vectors parse and their reachable C
helpers analyze successfully. Fifteen vectors have active generated policies;
the four IntuiMessage/filter vectors have explicit reviewed fail-closed
refusals. The deterministic review queue is zero and the manifest status is
`ready-for-generation`. `--check-import gadtools` reproduces that packet from
the current source, headers, generated build inputs, compiler configuration,
and policy.

The generated bridge contains all 19 dispatch outcomes. The menu lifecycle,
the `Menu.FirstItem`/`LayoutMenuItemsA` facade crossing, scalar
`GT_GetGadgetAttrsA` copyback, and the
`CreateContext`/`CreateGadgetA`/`FreeGadgets` linked Gadget lifecycle have each
reached their positive boot fixture, including nested record fields and
family-wide stale-token rejection. `GTMN_FrontPen` is now high-confidence `u32`
evidence derived from the public record field type used by the implementation.

This is not yet a certification claim. The complete combined T3GEN boot exposed
a delayed hosted `Macaros` crash after GUI fixtures: trace instrumentation proved
the final 68k call, per-run object cleanup, host-run free, and DOS-base release
all returned, while the macOS reports fault later in native stdio/file flushing.
The non-GUI T3LHA regression passes. The hosted crash must be resolved (or the
same contracts rerun in a demonstrably stable harness) before GadTools is marked
certified and before this iteration retires any remaining equivalent crossing.

The current PhotoDemo image passes `GetVisualInfoA`, `CreateMenusA`, and
`LayoutMenusA`. Source analysis now recognizes both direct zero/nonzero payload
tests and function-like macros such as `MODIFY_FLAG` when every visible
`ti_Data` occurrence is truth-only. Any assignment, cast, dereference, call, or
retention in the same case keeps it unresolved. This classified the Window flag
family in one pass and reduced the Intuition review packet from 264 to 224
findings.

Imported tag candidates also map a pointer payload to an existing object class
when its canonical C pointer type has exactly one match. Thus a proven
`struct Window *` becomes a Window-token conversion while an unmatched Image,
Hook, or private pointer stays refused. The current PhotoDemo image passes both
`WA_NewLookMenus` and the following `WA_HelpGroupWindow` object payload. Its next
named boundary is Intuition LVO 25, `InitRequester`, rather than another
GadTools or Window-tag crossing.

## What cannot be completely automatic

Ordinary AROS C libraries should approach a one-command import. A finite review
step can still be required for behavior the source and types do not uniquely
describe: polymorphic pointers, undocumented retained lifetimes, private
assembly, binary-only third-party libraries, native callbacks with unusual
calling conventions, and direct hardware access. The tool reports these cases;
it never invents a translation to make coverage numbers look complete.
