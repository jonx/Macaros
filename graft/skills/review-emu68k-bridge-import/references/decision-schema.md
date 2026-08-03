# Review decision contract

Use this schema for `build/emu68k-import/<library>/agent-review.json`. It makes an
agent review auditable and prevents prose approval from being mistaken for an
active bridge contract.

```json
{
  "schema": 1,
  "library": "gadtools.library",
  "manifest_sha256": "<sha256 of manifest.json>",
  "reviewed_at_commit": {
    "bridge_repo": "<commit>",
    "aros_source": "<commit>"
  },
  "summary": {
    "approved": 0,
    "refused": 0,
    "deferred": 1
  },
  "decisions": [
    {
      "review_id": "R0001",
      "function": "CreateContext",
      "status": "deferred",
      "claim": "object result and linked-family lifetime",
      "policy_delta": null,
      "evidence": [
        {
          "path": "workbench/libs/gadtools/createcontext.c",
          "line": 1,
          "fact": "Describe only what this exact source proves"
        }
      ],
      "tests": [],
      "reason": "Family token invalidation is not generated yet"
    }
  ]
}
```

## Field rules

- `manifest_sha256` binds decisions to the exact packet. Re-review after it
  changes.
- `status` is exactly `approved`, `refused`, or `deferred`.
- `approved` requires a complete `policy_delta`, direct source evidence, and
  named positive/negative tests.
- `refused` means an explicit closed outcome is the intended supported policy;
  explain which ambiguity or unsupported representation makes refusal correct.
- `deferred` means more evidence or bridge machinery is required. Never emit a
  speculative `policy_delta` for it.
- `policy_delta` contains the minimal JSON object intended for the active policy,
  not a copy of the whole generated candidate file.
- `evidence.path` is repo-relative. `line` must identify current source, header,
  `.conf`, or generated layout evidence.
- `tests` names concrete executable contracts and expected outcomes, not merely
  “add tests.”

## Approval checklist

An approval must answer all applicable questions:

1. What does the 68k register contain: scalar, guest address, BPTR, or token?
2. What is the native type and width?
3. Is the argument input, output, or in/out?
4. Is pointed data copied, rebuilt, rebased, retained, or represented by a token?
5. What are nullability and bounds?
6. Who owns the native result and what destroys it?
7. Can destruction traverse related objects whose tokens must also be invalidated?
8. Does the guest read object fields, requiring a facade?
9. Can native code call back into 68k code, and is that path tested?
10. Do tags change meaning by API direction or record variant?
11. What exact positive test proves the successful crossing?
12. What exact negative test proves unsafe input fails before native code?

If any applicable answer is missing, use `deferred` or `refused`.

## Review summary

Optionally write `agent-review.md` beside the JSON for humans. Keep it derived
from the JSON and include the manifest hash, counts, remaining review IDs,
certification gate results, and the next legacy-program capability gap.
