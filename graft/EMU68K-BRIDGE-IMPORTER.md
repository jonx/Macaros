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

The check regenerates all three artifacts in memory and fails if any file is
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
   target sysroot.
6. The AROS cross-Clang configured for `aarch64-unknown-aros`.

The explicit current sysroot is important: a cross compiler can retain a stale
build path in its defaults. The analyzer therefore reconstructs the real compile
environment instead of invoking a host compiler over isolated source snippets.

## Implemented analysis

For each non-private public vector, the analyzer:

- finds the `AROS_LH*` implementation and records its source location;
- compiles the real function with cross-Clang and consumes its JSON AST;
- records direct calls, indirect calls, parameter field reads, concrete casts,
  pointers stored into fields, allocators, releasers, and callback patterns;
- finds locally defined functions called by that vector and walks their direct
  call graph, currently bounded to 128 helpers and 12 levels;
- maps helper parameters back to public ABI parameters using AST call-argument
  positions; and
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

For example, `CreateGadgetA` does not itself spell out every gadget tag. It calls
helpers such as `makebutton`, `makecycle`, and `makelistview`. The interprocedural
pass includes those helpers and records which observations flow from the public
`taglist` and `ng` arguments.

Every propagated observation retains the source file, line, and shortest known
call path in `via`. Repeated observations are retained when their provenance or
inferred kind differs; later manifest synthesis can choose the strongest
non-conflicting evidence without losing the audit trail.

Tag domains remain per public function and argument. A tag used for input by a
creation/setter API does not automatically describe an output/query API: in the
latter, `ti_Data` can instead be a guest pointer to result storage. Direction
must be proved before a candidate domain is activated.

## Confidence and fail-closed rules

The analyzer produces evidence; it does not silently make uncertain evidence a
bridge policy.

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

## What cannot be completely automatic

Ordinary AROS C libraries should approach a one-command import. A finite review
step can still be required for behavior the source and types do not uniquely
describe: polymorphic pointers, undocumented retained lifetimes, private
assembly, binary-only third-party libraries, native callbacks with unusual
calling conventions, and direct hardware access. The tool reports these cases;
it never invents a translation to make coverage numbers look complete.
