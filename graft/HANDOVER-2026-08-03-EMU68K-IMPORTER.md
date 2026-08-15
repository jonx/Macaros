# AROS 68k bridge importer handover — 2026-08-03

## Goal and non-negotiable completion criteria

Build a reusable, source-driven importer which accepts a library name and, in
one command, discovers its ABI and implementation inputs, analyzes every public
vector and reachable helper, and emits deterministic bridge policy/code/tests:

```sh
graft/gen-emu68k-bridge --import-library gadtools
```

Facts which are proven from ABI, types, layouts, tags, and source behavior may
be accepted automatically. Ambiguous pointers, retention, callbacks, assembly,
or hardware behavior must fail closed in a finite review report. Completion
also requires:

1. all of GadTools imported and certified;
2. equivalent handwritten crossings retired;
3. bridge and LHA regressions passing;
4. PhotoDemo rerun successfully far enough to expose the next genuine gap;
5. a second library imported without adding library-specific importer logic;
6. documentation and a review-agent skill;
7. separate local commits, no pushes, and unrelated changes preserved.

The goal is **not complete**. Do not mark it complete from the zero-item
GadTools review queue alone.

## Repositories and local rules

- Integration/tooling repository:
  `/Users/jkn/Source/Macaros`, branch `main`
- AROS source repository:
  `/Users/jkn/Source/aros-upstream`, branch
  `aarch64-darwin-graft`
- Stable build tree: `/Users/jkn/aros-build`
- Candidate legacy corpus: `/tmp/aros-68k-candidates/corpus`
- Do not push either repository.
- Commit each meaningful iteration separately in the repository it changes.
- Use `apply_patch` for source changes.
- The current environment may require approval to write the upstream source or
  build tree because only the integration repository is a writable root.

## Current committed checkpoint

### Integration/tooling repository

Recent commits, newest first:

```text
35ea42b bridge: complete GadTools scalar query import
de3a3dc bridge: infer scalar and object tag payloads
3573d5c bridge: generate linked Gadget families
042a206 bridge: generate Window refresh lifecycle
f36ec2c bridge: follow delegated public vector semantics
6769952 bridge: recover exact generated library symbols
d61f967 bridge: generate embedded object facades
857f757 bridge: generate source-proven no-op vectors
```

### AROS source repository

Recent commits, newest first:

```text
ed81c65335 emu68k: generate remaining GadTools crossings
6ddfc4de9f emu68k: expand generated Window tag crossings
e7a42e3534 emu68k: generate linked Gadget crossings
bbc02290b4 emu68k: generate Window refresh crossings
1137628c41 emu68k: expose embedded screen RastPort
261a4086d8 emu68k: generate GadTools refresh no-op
00cd51672e emu68k: report reviewed GadTools refusals
78025d5408 emu68k: generate GadTools menu layout
```

No commit has been pushed.

## What the importer currently proves

The authoritative generated packet is:

```text
build/emu68k-import/gadtools/
  analysis.json
  bridge.generated.c
  manifest.json
  policy.generated.json
  review.md
  tests.generated.json
```

The current report says:

```text
Status: ready-for-generation
Public vectors: 19
Generated from ABI: 0
Already covered by policy: 15
Explicitly refused: 4
Crossing semantics requiring review: 0
Review items: 0
```

All 19 public GadTools vectors therefore have a deterministic outcome. Fifteen
have active generated crossings. These four IntuiMessage/filter functions fail
closed by reviewed policy:

- `GT_GetIMsg`
- `GT_ReplyIMsg`
- `GT_FilterIMsg`
- `GT_PostFilterIMsg`

The zero review count means the import packet is finite and resolved. It does
not mean every vector is executable, nor does it establish runtime
certification.

The `Generated from ABI: 0` / `covered by policy: 15` distinction must remain
visible. The importer discovers and analyzes the entire library at once, and
the generator emits the crossings, but GadTools crossing semantics are
currently supplied through the versioned policy accumulated during review.
The second-library proof must show that the reusable inference and policy
vocabulary generalize without adding library-name branches to the importer.

## GadTools crossings already generated and exercised

The generated `LayoutMenusA` crossing is at
`build/emu68k-import/gadtools/bridge.generated.c` around line 521. It converts:

- the guest-readable `Menu` facade to the native Menu object;
- the `VisualInfo` token to its native object;
- the guest tag list through `gadtools.layout_menus`;
- the native Boolean result back to D0.

Seeing `LayoutMenusA` in a trace is expected; it is the call following
`GetVisualInfoA` and `CreateMenusA`. It is not a request to hand-write another
crossing.

Generated coverage includes:

- Menu and `Menu.FirstItem`/MenuItem facades;
- `CreateMenusA`, `LayoutMenusA`, `LayoutMenuItemsA`, and `FreeMenus`;
- `VisualInfo` ownership;
- embedded `Screen.RastPort` facade and `DrawBevelBoxA`;
- Window facade and GadTools refresh lifecycle;
- linked Gadget families created by `CreateContext`/`CreateGadgetA` and consumed
  by `FreeGadgets`;
- `GT_SetGadgetAttrsA` scalar inputs;
- `GT_GetGadgetAttrsA` scalar output-tag copyback;
- nested `NewMenu`, `NewGadget`, and `TextAttr` records;
- source-proven no-op and explicit-refusal outcomes;
- native-to-68k Hook and BOOPSI callback infrastructure in the broader bridge.

## Test evidence and certification status

Current retained artifacts show:

- `build/t3gen-out.txt`, 2026-08-03 06:41:35: the main generated corpus reached
  `=== DONE ===`, including `[T3RECORD] PASS`.
- `build/t3gen-menuitem-out.txt`, 2026-08-03 07:01:50:
  `[T3MENUITEM] PASS` and `=== DONE ===`.
- There is no current `build/t3gen-gadget-out.txt` artifact, so the current
  combined `make hosted-emu68k-t3gen` gate is not proven green even though the
  Gadget fixture passed in an earlier isolated run.
- `make hosted-emu68k-t3lha` passed twice before the current diagnostic work.
- The complete GadTools certification gate has not been rerun successfully
  after the latest generated changes and hosted crash investigation.

Do not replace these scoped facts with a blanket “T3GEN passes” claim. Rerun the
actual make target after resolving the native crash.

## PhotoDemo status

The canonical local command is:

```sh
EMU68K_TRACE_CALLS=1 \
CORPUS_BEFORE='Assign Photodemo: MacRW:corpus/gui__PhotoDemo.d' \
EMU68K_MAX_SECONDS=240 CORPUS_TIMEOUT=420 \
./graft/68k-corpus /tmp/aros-68k-candidates/corpus \
  build/photodemo-import.txt
```

PhotoDemo is **not running successfully yet**. Work during this iteration moved
it through generated `GetVisualInfoA`, `CreateMenusA`, and `LayoutMenusA`.
Source-driven Intuition tag inference also moved it through `WA_NewLookMenus`
and the `WA_HelpGroupWindow` object payload. The importer documentation records
the next named guest boundary as Intuition LVO 25, `InitRequester`.

Older retained files under `build/pg-*.txt` and `build/photogenics-*.txt` are
diagnostic history, not current certification. For example,
`build/pg-visualinfo.txt` predates the generated `CreateMenusA` crossing and
still stops at GadTools LVO 8.

## Current native hosted crash investigation

### Symptom

GUI fixtures can complete all 68k bridge calls and cleanup, then `Macaros`
faults during native graphics work. Temporary bridge tracing previously proved
that the final 68k call returned, per-run bridge state was cleaned, the host run
was freed, the DOS base was closed, and the handler returned. The fault is
therefore not evidence that `LayoutMenusA` is absent.

The crash is reproducible in macOS reports under:

```text
~/Library/Logs/DiagnosticReports/Macaros-2026-08-03-*.ips
```

The useful report is `Macaros-2026-08-03-074848.ips`. Its AROS PCs are stable
relative to the end of the first hosted RAM block:

```text
+0x004174  kernel tlsf_freevec
+0x012a88  kernel nommu_FreeMem
+0x00b084  kernel Exec_35_FreeMem
+0x0e25f8  dos.library Dos_84_RunCommand
+0x00702c  kernel Exec_23_Permit
+0x1c3b64  gfx.hidd bitmap code
```

`C:SymbolDump` was run before the crash and copied to
`~/AROS/Shared/symbols.out`. Correlating the randomized runtime addresses with
the ELF module proved that the last return site is `gfx.hidd` ELF offset
`0xa5f4`, immediately after an indirect `FreeMem` call in the static
`DoBufferedOperation` helper used by
`BM__Hidd_BitMap__PutAlphaImage`.

The failing native operation is effectively:

```c
FreeMem(buf, 40960);
```

The temporary buffer is a 640-by-16, 32-bit pixel chunk. The allocator faults
while freeing it, which strongly indicates that a graphics operation overwrote
the allocation boundary before release.

### Uncommitted diagnostic patch

`/Users/jkn/Source/aros-upstream/rom/hidds/gfx/gfx_bitmapclass.c` currently has
an uncommitted diagnostic modification. It allocates 256-byte prefix/suffix
guards around the shared pixel buffer and checks them after:

1. `GETIMAGE`;
2. the buffered alpha operation;
3. `PUTIMAGE`.

This is diagnostic instrumentation, not a reviewed fix. Do not commit it as-is.
It also has two harmless `%lu` versus `ULONG` warnings which can be changed to
`%u` if the instrumentation is retained.

`kernel-hidd-gfx` compiled successfully at 08:01:20. The subsequent
`kernel-link-unix`/`boot` relink was interrupted once and then restarted with:

```sh
TARGETS='kernel-link-unix boot' ./graft/rebuild-aros.sh
```

The restarted command completed successfully with `2 ok, 0 failed`; both
`kernel-link-unix` and `boot` finished, and `AROS.boot` was restaged. Confirm
the output timestamps when resuming:

```sh
tail -30 /Users/jkn/aros-build/rebuild-logs/kernel-link-unix.log
tail -30 /Users/jkn/aros-build/rebuild-logs/boot.log
stat -f '%Sm %N' -t '%Y-%m-%d %H:%M:%S' \
  /Users/jkn/aros-build/bin/darwin-aarch64/AROS/boot/darwin/kernel
```

After the relink, run `genrecord` by itself or the full main T3GEN folder and
inspect `*.log` for the first line beginning `[gfx.hidd] pixel-buffer`. That
line will identify the corrupting stage. Then replace the diagnostic with the
smallest root-cause fix and rerun without guards.

## Other uncommitted work

The integration repository has one pre-existing uncommitted experiment:

```text
M graft/68k-corpus
```

It adds `CORPUS_DESKTOP=1`, copying the generated startup file into the host
share and running it behind Wanderer. This proved that keeping a public desktop
screen open does not prevent the crash. It is not required by the importer and
should normally be reverted with `apply_patch` after preserving any desired
idea in documentation. Do not accidentally include it in an importer commit.

Expected dirty state at handover:

```text
aros-aarch64:  M graft/68k-corpus
aros-upstream: M rom/hidds/gfx/gfx_bitmapclass.c
```

This handover document itself becomes an additional uncommitted file until it
is committed.

## Recommended continuation sequence

1. Confirm the running kernel relink completed and that the boot kernel mtime
   changed.
2. Run the isolated GUI record fixture with the guarded `gfx.hidd`.
3. Identify whether `GETIMAGE`, the buffered alpha operation, or `PUTIMAGE`
   crosses the guard; inspect that exact implementation and fix the underlying
   size/stride/extent bug.
4. Remove all guard instrumentation and rebuild `kernel-hidd-gfx`,
   `kernel-link-unix`, and `boot`.
5. Run the complete `make hosted-emu68k-t3gen`; require main, Gadget, and
   MenuItem boot artifacts to pass.
6. Run `make hosted-emu68k-t3lha` and the existing bridge regressions.
7. Rerun PhotoDemo with call tracing and address the next genuine bridge gap
   through importer inference/policy, not a one-off handwritten crossing.
8. Update `manifest.json` certification evidence and
   `graft/EMU68K-BRIDGE-IMPORTER.md`; only then call GadTools certified and
   retire any remaining equivalent handwritten crossing.
9. Commit the graphics fix separately in the upstream repository and the
   certification/importer changes separately in the integration repository.
10. Import a second library using the identical command path. Add reusable
    inference vocabulary if needed, but no `if library == ...` logic.
11. Run deterministic regeneration/checks and audit the review-agent skill.

## Final commands and gates

Importer regeneration and deterministic check:

```sh
python3 graft/gen-emu68k-bridge --import-library gadtools
python3 graft/gen-emu68k-bridge --check-import gadtools
```

Generated bridge rebuild and regressions:

```sh
TARGETS='hostlibs-emu68k' ./graft/rebuild-aros.sh
make hosted-emu68k-t3gen
make hosted-emu68k-t3lha
```

Review documentation and agent skill:

```text
graft/EMU68K-BRIDGE-IMPORTER.md
graft/skills/review-emu68k-bridge-import/SKILL.md
graft/skills/review-emu68k-bridge-import/references/decision-schema.md
```

The review skill is already versioned and is designed to bind decisions to the
manifest hash, require source/layout citations, keep approved/refused/deferred
outcomes distinct, apply policy only when authorized, and run certification
gates on behalf of the user.
