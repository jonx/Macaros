# Source-driven 68k bridge importer

The importer turns an AROS native library implementation into evidence for the
68k-to-native bridge. Its unit of work is a library, not the next missing LVO
encountered by an application.

The intended stable command is:

```sh
graft/gen-emu68k-bridge --import-library gadtools
```

That command is being built in independently testable stages. The current
source-analysis stage can be inspected with:

```sh
graft/gen-emu68k-bridge --analyze-source gadtools > /tmp/gadtools-analysis.json
```

`--import-library` is not yet a released interface. This document distinguishes
implemented behavior from the remaining pipeline so a prototype is not mistaken
for a certified bridge.

## Inputs and discovery

The library name is resolved through the same AROS tree scan used by bridge code
generation. From there the tool discovers:

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

For example, `CreateGadgetA` does not itself spell out every gadget tag. It calls
helpers such as `makebutton`, `makecycle`, and `makelistview`. The interprocedural
pass includes those helpers and records which observations flow from the public
`taglist` and `ng` arguments.

Every propagated observation retains the source file, line, and shortest known
call path in `via`. Repeated observations are retained when their provenance or
inferred kind differs; later manifest synthesis can choose the strongest
non-conflicting evidence without losing the audit trail.

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

## Planned import and certification pipeline

`--import-library <name>` will consume the analysis and produce deterministic,
source-hashed artifacts:

1. A per-library bridge manifest containing accepted facts and full provenance.
2. A finite review report containing every unresolved or conflicting fact.
3. Generated bridge policy and crossing code for accepted facts.
4. Generated negative tests for unsupported tags and invalid object types.
5. Lifecycle tests for inferred producer/releaser pairs.
6. Callback/re-entry tests when callback contracts are accepted.
7. A certification result proving all public vectors are either generated or
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

