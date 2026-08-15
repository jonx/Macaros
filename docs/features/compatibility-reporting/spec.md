# Implementation spec — Macaros compatibility reporting

> Status: **slice 1 partially built; reporter UI remains** · Target: hosted AROS on macOS, with the 68k
> execution service as the first producer · Drafted 2026-08-05 · Companion surfaces:
> [host app shell](../host-app-shell/README.md),
> [transparent 68k execution](../68k-transparent-exec/README.md), and
> [crash handling](../crash-handling/README.md).

## Provenance and terminology

This is original project design derived from the repository's existing diagnostics,
Macaros capture UI, and public macOS facilities. No third-party reporting-client source
was consulted. Requirements tagged `[OURS]` bind existing project behavior;
`[PUB]` denotes ordinary documented macOS/POSIX facilities.

- **Report**: one local directory and its exported archive, describing one problem.
- **Producer**: the 68k runtime, AROS diagnostics, or Macaros component that emits
  evidence.
- **Reporter**: the Macaros review/redaction/submission UI.
- **Technical attachment**: logs, traces, build identities, hashes, stacks, and task
  dumps.
- **Sensitive attachment**: screenshot, full log, program binary, guest-memory/core
  snapshot, or recording.
- **Submission**: transfer outside the user's Mac. Local collection is not submission.

## Outcome

A user who encounters a crash, capability gap, hang, or visible misbehavior can open one
Macaros sheet, describe what happened, optionally add a safely redacted screenshot,
inspect the exact contents, and deliberately save, share, or send a self-contained
report. A failure that Macaros detects offers the same sheet without requiring the user
to find a log file.

The system is useful to a developer without assuming source code is available. It
captures enough identity to group duplicate reports, enough execution evidence to locate
the boundary involved, and—when the user elects to include it—enough state to reproduce
the exact 68k execution.

## Scope

### In

- Manual **Help -> Report a Compatibility Problem...** at any time.
- A non-modal report affordance after a capability gap or contained engine fault.
- A single stable local Reports directory with bounded retention.
- Per-68k-run identity, result, gap ledger, structured calls/events, and crash artifacts.
- Bounded host/AROS session logs and non-destructive task dumps for hangs.
- Host framebuffer capture even when the guest program has stopped.
- Local screenshot crop and opaque rectangle redaction.
- Attachment-by-attachment preview, sensitivity labels, and consent.
- A versioned report format independent of its transport.
- Save, macOS Share, and optional HTTPS submission transports.
- Server-side duplicate grouping by non-sensitive failure signature.

### Out

- Silent telemetry or upload-on-crash.
- Raw keyboard history, clipboard contents, network payload capture, or environment dumps.
- Automatic redistribution of third-party binaries.
- Guest-drawn consent UI or guest-owned TLS/account logic.
- A full image editor, OCR service, or cloud-based redaction.
- Treating a screenshot as sufficient proof of a semantic fix.
- Destructive diagnostics: collecting a report must never terminate Macaros or AROS.

## Ownership and architecture

The diagnostic producer and reporter are separate by design:

```text
 68k program
      |
      v
 emu68k/JIT ---------------------> per-run structured evidence
      |                                      |
      | contained gap/fault                  v
      +----------------------------> Reports spool
                                             |
 AROS native diagnostics --------------------+
                                             |
 Macaros framebuffer + product identity -----+
                                             v
                                  review/redaction sheet
                                             |
                           +-----------------+----------------+
                           v                 v                v
                       Save archive      macOS Share     explicit HTTPS Send
```

The runtime owns facts about 68k execution. Macaros owns user interaction, the
framebuffer, filesystem presentation, redaction, and network transfer. This preserves a
working reporting surface when the guest process is gone or AROS input is wedged.

The first implementation uses a filesystem spool rather than callbacks from an AROS or
JIT thread into AppKit. Producers write atomically; Macaros observes completed records
on its own queue and presents UI only on the main thread.

## R-LOCATION — one writable report root `[OURS]`

Launchers set:

```text
AROS_REPORT_DIR=$AROS_RUN_DIR/Reports
```

For a packaged release this resolves to:

```text
~/Library/Application Support/AROS/Reports
```

Development launchers continue to use their configured `$AROS_RUN_DIR`, normally
`run/darwin-aarch64/`. Crash-bundle configuration, structured bridge tracing, the host
session-log sink, screenshots made for a report, and the reporter all derive their paths
from `AROS_REPORT_DIR`; they do not independently choose `/tmp`, the boot directory, or
the Desktop.

Layout:

```text
Reports/
  active/                 incomplete per-run structured records
  ready/                  completed local reports, one directory per report ID
  exported/               archives the user explicitly saved
  session/                bounded Macaros/AROS host logs
```

Directories are mode `0700`, files are `0600`, and report creation uses write-temp plus
atomic rename. A corrupt or partial record is retained as `.incomplete` and never
submitted without being identified as such.

Default retention is the newest 20 ready reports with a 500 MiB total ceiling. Macaros
removes only the oldest unpinned report after a newer report is complete. Reports the
user exported, pinned, or has open in the review sheet are never automatically removed.
Settings exposes retention count/size and **Delete All Local Reports...** with
confirmation.

## R-RUN — every 68k run has a durable identity `[OURS]`

At run creation, the execution service writes `active/<run-id>/run.jsonl`. The ID is a
random UUID, not a hash of user data. The first record is `run.start`; the final record is
exactly one of `clean-exit`, `capability-gap`, `hardware-route`, `killed`, `timeout`, or
`engine-fault`.

Every record carries:

- schema version and monotonically increasing sequence number;
- UTC timestamp and monotonic offset from run start;
- program display name and SHA-256 of the program bytes;
- Macaros, AROS, emu68k/JIT, bridge-policy, and generated-layout build identities;
- execution backend/route and guest arena bounds;
- event name plus event-specific bounded data.

The default trace records library/vector identity, validation decisions, object-token
kinds, event routing, callback boundaries, return/result class, and errors. It does not
record arbitrary pointed-to buffers, text-entry contents, clipboard data, or network
payloads. Argument values whose content is essential are represented by safe scalar
values, type/length, and a hash unless a reviewed event schema explicitly permits more.

Normal execution must not pay for per-instruction tracing. The existing JIT flight
recorder remains a bounded ring used only in a fault snapshot. Structured bridge records
are bounded to 10,000 events or 4 MiB per run; after the limit, one `trace.truncated`
record names the discarded count.

## R-CLASSIFY — one problem taxonomy and signature `[OURS]`

The reporter recognizes:

| Class | Examples | Required signature fields |
|---|---|---|
| `capability-gap` | unknown LVO/tag, absent object policy, unsafe pointer | program SHA, library, vector/LVO, reason code, tag/type when present |
| `engine-fault` | illegal translation, bad guest PC, sandbox violation, host signal in translated code | program SHA, fault kind, 68k PC/opcode, engine build |
| `hang` | event deadlock, task wait cycle, no progress | program SHA when known, waiting-task signature, recent bridge event |
| `visual` | clipping, backfill, stale display, wrong colors | program SHA when known, reporter-selected category; never deduced from pixels |
| `behavior` | wrong value/state with no structural rejection | program SHA, user category, last relevant crossing when known |
| `routing` | hardware access requiring FULL | program SHA, address/access type, routing decision |

The signature is a stable JSON object and SHA-256 used for duplicate grouping. It never
contains a username, host path, screenshot bytes, memory contents, or program bytes.

A report may be opened manually without an automatic classification. The user selects
Visual, Behavior, Hang, Performance, or Other; the reporter associates any currently
running 68k program but never guesses that it caused the problem.

## R-LOCAL — local collection is automatic; submission is not

On a classified failure, producers finish the local technical record without asking the
guest to continue. Macaros shows a non-modal notice such as:

> PhotoDemo stopped because `asl.library/AslRequest` does not yet support
> `ASLFR_Screen`. Review Report... · Dismiss

Dismissal leaves the bounded local report available under **Help -> Show Reports**. It
does not send anything and does not repeatedly prompt for the same run.

For a visible problem that did not fail loudly, the user invokes **Report a
Compatibility Problem...**. Macaros snapshots the current diagnostic rings and opens a
manual report without stopping or sampling the process destructively.

No diagnostic command used by this feature may send SIGKILL, request shutdown, alter
guest state, or use a helper whose default behavior terminates the process. Hang capture
uses the non-destructive task-dump path and a bounded host sample equivalent to
`aros-ctl diag nokill`.

## R-REVIEW — the consent sheet

The report sheet contains:

1. Program and failure summary in plain language.
2. **What were you doing?** free-form reproduction steps.
3. Optional expected and actual behavior fields.
4. An attachment list with checkbox, sensitivity label, byte size, and Preview/Reveal.
5. Screenshot preview and **Redact...** action when screenshot inclusion is enabled.
6. Destination disclosure: Save, Share, or the named report service.
7. Cancel and explicit **Send Report** actions.

Safe technical attachments may start checked, but the entire package is still shown
before Send. Sensitive attachments always start unchecked, and their selection is never
remembered for the next report.

| Attachment | Default | Label |
|---|---:|---|
| sanitized manifest/build IDs | on | Technical |
| bounded structured run trace | on | Technical; may contain AROS filenames |
| sanitized bounded log tail | on | Technical; review available |
| 68k/host stacks or task dump | on when present | Technical; paths sanitized |
| screenshot | off | Sensitive; may show personal information |
| full session log | off | Sensitive; may contain host paths |
| program binary | off | Sensitive; may be copyrighted or proprietary |
| guest-memory/core snapshot | off | Highly sensitive; may contain document data |
| movie recording | off | Highly sensitive |

Selecting the binary displays: "Only include this if you have permission to share the
program." Selecting a core snapshot displays: "Memory may contain open documents,
filenames, or other application data." These warnings appear at selection time and in
the final confirmation.

The sheet works without a network connection. Save and Share remain available; Send
reports a connection failure without losing or silently retrying the package.

## R-SCREENSHOT — capture without accidental disclosure `[OURS]`

Macaros captures its owned framebuffer through the existing offscreen readback, not
ScreenCaptureKit. No screen-recording permission is required and no other Mac window can
appear in the image.

For an automatic failure, Macaros may retain one uncompressed framebuffer copy in memory
so the failure state is not replaced by a blank screen. It must not persist that original
capture. The buffer is replaced on the next failure, zeroed/released when the report is
dismissed or completed, and never included merely because it exists.

For a manual report, capture occurs when the user enables **Include screenshot**. The
checkbox defaults off for every report.

## R-REDACT — simple, irreversible screenshot redaction `[PUB]`

The screenshot editor is a sheet within the reporter, not a general image editor.
Version one provides:

- drag to add any number of axis-aligned redaction rectangles;
- select, resize, move, or delete a rectangle;
- crop with adjustable bounds;
- Undo, Redo, Clear Redactions, Reset Crop, Cancel, and **Use Redacted Screenshot**;
- zoom-to-fit plus a 100% inspection mode;
- a persistent caption: **Only the flattened image shown here will be sent.**

Redaction is a fully opaque solid fill. Blur, translucency, and pixelation are forbidden:
they preserve visual information and can create a false privacy guarantee. The fill
defaults to neutral black and may offer another fully opaque neutral color.

Editing coordinates are stored in source-image pixels, independent of preview scale and
Retina backing scale. Crop is applied first; rectangles are clipped to the crop. On
confirmation Macaros renders a new, opaque bitmap, composites the solid rectangles into
its pixels, encodes a fresh PNG with no metadata or auxiliary chunks, and presents that
exact flattened PNG in the final report preview.

The unredacted bitmap:

- remains in memory only;
- is never placed in `ready/`, an archive, pasteboard, thumbnail cache, Quick Look, or
  upload body;
- is discarded on Cancel, report dismissal, successful flattening, or app termination;
- cannot be recovered through an alpha layer, edit history, sidecar, or embedded
  thumbnail.

Undo history contains geometry only, not image copies. After flattening, the editor may
keep the original in memory only while the same report sheet remains open so the user can
revise redactions; it is destroyed before packaging. Final confirmation shows the
decoded packaged PNG, not an editor-layer preview.

Accessibility: every rectangle appears in a list with pixel bounds and Delete; keyboard
users can add a default centered rectangle and adjust it numerically. The editor exposes
standard accessible names for the canvas, crop, and redaction controls.

## R-PACKAGE — versioned and independently inspectable

The canonical export is:

```text
macaros-report-<UTC>-<short-id>.zip
  manifest.json
  summary.txt
  reproduction.txt
  run.jsonl
  log.txt                         optional
  stacks.txt                      optional
  task-dump.txt                   optional
  screenshot-redacted.png         optional
  crash-bundle.tar.gz             optional
  program.bin                     optional, explicit consent
  core.snapshot                   optional, explicit consent
```

`manifest.json` is UTF-8 JSON with `schema: 1`, report ID, timestamps, failure class and
signature, product/build identities, program display name and SHA-256, route/result,
user-selected category, and an attachment array. Every attachment entry gives its path,
size, SHA-256, sensitivity class, provenance, truncation state, and the consent choice
that admitted it.

`summary.txt` is a concise human-readable rendering of the manifest. `reproduction.txt`
contains only user-entered steps and safe generated instructions. The importer expands
an existing JIT crash bundle in private staging and separates its report/stacks from its
program and core snapshot, so those sensitive members retain independent checkboxes. It
then builds a new consented crash attachment. An opaque legacy bundle is never selected
by default and is labeled Highly sensitive; it cannot bypass the current binary/core
choices.

Archives are built in a private temporary directory, validated against the manifest,
then atomically moved to `exported/` or streamed to the selected transport. Packaging
never follows symlinks and rejects absolute paths, `..`, devices, sockets, and files that
changed after review.

## R-SANITIZE — bounded default evidence

Before review, the safe view:

- replaces the user's home directory with `~`;
- replaces configured host-volume roots with `<host-volume:N>` while retaining the AROS
  volume/path when useful;
- removes environment values, command-line secrets, clipboard contents, and raw input;
- bounds every text attachment by bytes and records truncation;
- records program content by SHA-256, not by including bytes;
- strips macOS extended attributes and image metadata;
- does not invoke remote OCR or any other remote analysis.

The Preview action shows the sanitized artifact that will be packaged. If the user
selects a full unsanitized log, it remains labeled Sensitive and requires a second
confirmation.

## R-TRANSPORT — save, share, or explicit submission `[PUB]`

All transports consume the same validated archive.

- **Save Report...** uses a save panel and makes no network request.
- **Share Report...** uses the macOS sharing service picker after archive creation.
- **Send Report** performs one HTTPS upload to the destination named in the sheet.

Send is disabled until the user checks **I have reviewed the attachments above**. The
button action itself is the submission consent. There is no upload on app launch,
failure, report-sheet open, attachment selection, or screenshot capture.

The upload protocol is versioned multipart HTTP containing the archive plus its
non-sensitive signature. A successful response returns an opaque report ID and optional
status URL, both stored locally. The service may group reports by signature but must not
publish attachments into a public issue automatically. Public issue creation, if
offered, is a separate user action with a preview.

Failed uploads leave the reviewed archive local and offer Retry, Save, or Cancel. A
retry is allowed only for that exact already-consented archive; changing any attachment
invalidates consent and returns to review. No background retry occurs after Macaros
relaunch without another explicit Send.

The endpoint is supplied by signed application configuration, not by report contents or
guest data. Standard platform TLS validation is mandatory. Credentials, if a future
service requires them, live in Keychain and are never placed in a report.

## R-SUFFICIENCY — what a report can prove

The UI and developer documentation must not promise that logs plus a screenshot always
make a problem fixable.

| Failure | Minimum useful report | Strong reproducer |
|---|---|---|
| capability gap | manifest, named gap, bounded crossing record, program SHA | generated fixture or shareable binary |
| visual corruption | steps, redacted screenshot, event/call trace | binary plus deterministic interaction script |
| engine fault | fault report, registers, stacks, flight recorder | binary and reloadable snapshot |
| hang | non-destructive task dump and recent calls/events | binary and exact steps/input script |
| plausible wrong value | arguments/results around the crossing | binary or minimal source reproducer |

A public API and the gap record are often enough to implement a missing crossing, but a
fixture is still required before declaring it fixed. Source code is optional. Symbols
and source improve diagnosis, while a legally shareable binary and deterministic state
are normally more faithful for JIT/bridge reproduction.

## R-UX — menus and settings

Macaros Help becomes:

- **Report a Compatibility Problem...**
- **Show Reports**
- separator
- existing website/help links

The existing generic **Report an Issue** URL may remain as **Open Issue Tracker** but is
not the compatibility-report workflow.

Settings -> Privacy & Reports contains:

- report location and Reveal button;
- retention count and size;
- local-report collection explanation;
- **Delete All Local Reports...**;
- report-service destination and privacy-policy link when submission is configured.

There is deliberately no "send reports automatically" preference. Screenshot, binary,
core, full-log, and movie consent can never be made sticky.

## R-FAILURE — reporter failure must be safe

- If report storage is full/unwritable, keep the newest bounded technical record in an
  in-memory ring and show Save As; do not crash the guest or Macaros.
- If screenshot capture fails, the report remains sendable without it and states why.
- If redaction/encoding fails, screenshot inclusion turns off; the original is not used
  as fallback.
- If manifest validation fails, Send is disabled and the report remains local.
- If Macaros exits during packaging, only `.incomplete` staging remains; it is never
  offered for automatic upload.
- If a producer faults while reporting a fault, the existing re-entry latch prevents
  recursion and emits a minimal record.
- Opening, previewing, sampling, or dismissing a report never resets or terminates the
  running instance.

## Implementation slices

1. **Local contract.** Add `AROS_REPORT_DIR`, stable run IDs, structured terminal
   records, bounded session-log rotation, and atomic `ready/` reports. Route existing
   crash bundles there.
2. **Macaros report browser.** Show Reports, manual Report action, safe manifest preview,
   reproduction fields, and Save archive. No network.
3. **Screenshot privacy.** In-memory capture, crop/opaque rectangles, fresh flattened
   PNG, metadata stripping, and archive-exclusion proofs.
4. **Failure affordance.** Observe completed failure records and show one non-modal
   Review Report notice on the AppKit main thread.
5. **Share and submit.** macOS Share first; add the configured HTTPS transport only after
   the local package and consent tests are green.
6. **Intake loop.** Server signature grouping, private attachment access, report status,
   and a regression-fixture field linking the eventual fix.

## Verification

All tests use a temporary report root and a fake transport. No test sends real data.

### Package and lifecycle

- A clean 68k exit creates a bounded run record but no intrusive failure notice.
- A named capability gap produces one ready report with program/library/LVO/reason and
  no fabricated result.
- A contained engine fault incorporates the crash bundle and leaves Macaros reusable.
- Atomic rename means the reporter never opens a half-written ready report.
- Retention honors count, size, pinning, open sheets, and exports.
- Home and host-volume roots are absent from sanitized fixtures.

### Consent and network

- Opening/dismissing a report produces zero network requests.
- A fake endpoint sees no request until the explicit Send action.
- Screenshot, binary, core, full log, and movie start unchecked every time.
- Changing an attachment after a failed Send invalidates prior consent.
- Relaunch never retries an upload automatically.
- Save and Share work with networking disabled.

### Screenshot/redaction oracle

- Retina and non-Retina previews map drag rectangles to the same source pixels.
- Every pixel under each flattened rectangle equals the selected fully opaque fill.
- Crop output dimensions and rectangle clipping are exact.
- The exported PNG has no alpha disclosure, metadata, editor layers, thumbnails, or
  auxiliary image chunks.
- The archive contains exactly one screenshot member and it byte-matches the final
  decoded preview artifact.
- Cancel, dismiss, encode failure, and successful packaging release the original capture
  and create no original-image file.
- Undo/redo changes geometry only; its history contains no image bytes.

### Live behavior

- A fixture equivalent to the `ASLFR_Screen` gap yields a plain-language Review Report
  notice while AROS stays available.
- A visual-only fixture can be reported manually with steps and a redacted screenshot
  even though no capability gap exists.
- A hung guest task produces a task dump and host sample without signals that stop the
  instance.
- A stopped 68k program can still be reported because Macaros owns the UI and capture.
- Two identical signatures group as duplicates while keeping distinct local report IDs.

## Acceptance criteria

The feature is complete when:

1. Every classified 68k failure leaves a bounded, inspectable local artifact under the
   stable Reports directory.
2. A user can manually report incorrect behavior that did not trigger a failure.
3. Macaros remains able to review/save a report when the guest program has terminated.
4. Nothing leaves the Mac without an attachment list, final confirmation, and explicit
   Send/Share action.
5. Screenshot inclusion is off by default and its flattened redaction is proven to
   contain no recoverable original layers or files.
6. Binary and memory attachments are independent, off by default, and carry clear
   copyright/privacy warnings.
7. Report collection is non-destructive and cannot kill/reset the keeper instance.
8. A developer can connect a received report to a fixture and record the fixing build,
   closing the compatibility feedback loop.
