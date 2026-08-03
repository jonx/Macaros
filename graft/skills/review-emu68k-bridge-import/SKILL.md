---
name: review-emu68k-bridge-import
description: Review, promote, and certify source-driven AROS 68k-to-native bridge imports from gen-emu68k-bridge artifacts. Use when an agent must inspect a library's build/emu68k-import manifest and review report, resolve tag payloads, structure layouts, pointer translation, object ownership/lifetimes, callbacks, or fail-closed outcomes, update emu68k bridge policy when authorized, and validate a library import without asking a user to perform the ABI review manually.
---

# Review an emu68k bridge import

Act as the ABI safety reviewer for one imported AROS library. Resolve what the
source proves; never optimize for coverage. A deferred or refused crossing is a
correct result when native representation or lifetime is ambiguous.

## Establish the review scope

1. Locate the workspace root with `git rev-parse --show-toplevel`.
2. Read `graft/EMU68K-BRIDGE-IMPORTER.md` completely.
3. Read `references/decision-schema.md` completely.
4. Determine the requested mode:
   - For analysis/review requests, produce decisions without modifying policy.
   - For promotion/certification requests, apply source-proven decisions, generate
     tests, rebuild, and run the required gates.
5. Inspect both worktrees before edits. Preserve unrelated changes and obey the
   repository's commit/push instructions.

Do not treat prior chat conclusions, old traces, or an existing policy entry as
proof. Reconstruct the decision from current source artifacts.

## Refresh the evidence packet

Run the public interface:

```sh
graft/gen-emu68k-bridge --import-library <library>
graft/gen-emu68k-bridge --check-import <library>
```

Use these files together:

- `manifest.json`: authoritative structured evidence and provenance.
- `analysis.json`: complete AST/helper observations.
- `review.md`: finite queue and stable review IDs.
- `policy.generated.json`: proposals, not blanket approval.
- `tests.generated.json`: ready and blocked contracts.
- `bridge.generated.c`: effect of the currently active policy.

If import or stale checking fails, stop promotion and report the tool/build-input
failure. Do not review a packet whose hashes no longer match its inputs.

## Review each item

Process every review ID, grouped by function so interacting arguments and result
lifetimes are considered together.

For each claim:

1. Open every cited source/header at the recorded line. Follow reachable helpers
   and dispatch consumers when the value crosses another API internally.
2. Verify the `.conf` prototype, LVO, register map, public type, and argument
   direction.
3. Verify classic m68k and native layouts through compiler-probed output. Never
   hand-count offsets or infer pointer width from the native declaration.
4. Classify every pointer as one explicit contract: guest bytes/string, rebuilt
   record, typed native object token, guest-readable facade, callback, opaque
   guest cookie, output storage, retained guest memory, or refused.
5. Prove ownership, nullability, identity, destructor, family traversal, and
   retention duration for objects or stored pointers.
6. Prove tag payload representation independently for each public function,
   argument, and direction. Do not copy an input/setter domain into an output/get
   API.
7. Require an executable positive test and a boundary-failure negative test for
   every newly activated semantic class.

Record `approved`, `refused`, or `deferred` using the decision schema. Cite exact
repo-relative paths and lines. A statement such as “AROS normally does this” is
not evidence.

## Mandatory refusal rules

Keep the crossing closed when any of these remain unresolved:

- conflicting high-confidence payload types;
- bare `APTR`, zero defaults, uncast `GetTagData`, or `FindTagItem` receiver types;
- native pointers that would be truncated into a guest register;
- retained guest memory without a lifetime/copy contract;
- linked object destruction without family-token invalidation;
- callback or BOOPSI entry ABI without a tested re-entry adapter;
- assembly, hardware, varargs, or indirect calls whose behavior is not proved;
- output pointers without copy-back semantics;
- record variants whose pointer fields change meaning.

Prefer an explicit `refuse` tag/variant when the outer vector is otherwise safe.
Leave the vector unavailable when its complete function contract is not safe.

## Promote approved decisions

Only perform this section when the request authorizes implementation.

1. Apply minimal deterministic changes to
   `graft/emu68k-bridge-policy.json` and generic generators. Never add a
   per-application workaround. Add per-library logic only as declarative policy;
   reusable representation shapes belong in the generic importer/emitter.
2. Add or update layout types only when a public crossing actually rebuilds or
   exposes that record.
3. Add executable positive, negative, lifecycle, and stale-token tests. A JSON
   test contract alone does not certify runtime behavior.
4. Update `graft/EMU68K-BRIDGE-IMPORTER.md` in the same iteration with the new
   schema, inference, limitation, and commands.
5. Re-import so manifest/review counts reflect the promoted policy.

Do not retire a handwritten crossing until the generated equivalent passes the
same tests and a diff confirms behavior is identical.

## Run certification gates

Run at least:

```sh
python3 -m json.tool graft/emu68k-bridge-policy.json >/dev/null
python3 -m py_compile graft/gen-emu68k-bridge graft/gen-struct-layouts
graft/gen-emu68k-bridge --check-import <library>
make struct-layouts
python3 graft/gen-emu68k-bridge --emit \
  ../aros-upstream/arch/all-darwin/libs/emu68k/
TARGETS="hostlibs-emu68k" ./graft/rebuild-aros.sh
make hosted-emu68k-t3gen
make hosted-emu68k-t3lha
```

Run the relevant unchanged legacy corpus program after these gates. Capture the
next named capability gap and distinguish a bridge refusal from an application
failure or timeout.

Certification requires all of the following:

- every public vector has a generated safe crossing or explicit closed outcome;
- the review queue is empty except documented intentional refusals;
- generated files and import artifacts are current;
- positive and negative runtime tests pass in booted AROS;
- existing regressions pass;
- representative legacy programs reach the expected behavior.

Never label a library certified based only on successful generation or a reduced
review count.

## Report the result

Lead with the outcome. State:

- vectors approved, refused, and deferred;
- policy/code/tests changed, if authorized;
- exact gates run and their results;
- remaining review IDs and why they remain closed;
- the legacy program's new stopping point;
- local commits created and confirmation that nothing was pushed.

Write the machine-readable review decision file described in
`references/decision-schema.md` beside the import packet when the user requests
an auditable or repeatable review.
