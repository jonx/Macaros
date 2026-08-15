# Compatibility reporting

**Status: local collection and menu access built; review/export/submission UI remains.**
The first implementation keeps bounded compatibility evidence in one private local
folder and exposes it through the Macaros Help menu. The later stages described below
turn those files into reviewable packages and let the user explicitly choose whether
to save, share, or submit them.

The reporting UI belongs to **Macaros**, not to an AROS application. Macaros can still
capture the framebuffer, open a consent sheet, and save a report when a 68k program has
stopped or the guest is unhealthy. The 68k runtime and AROS remain responsible for
producing the technical evidence: capability gaps, library-call traces, stacks, task
dumps, and crash bundles.

```text
68k runtime / AROS diagnostics
              |
              v
       local Reports folder
              |
              v
   Macaros review + redaction
              |
              v
    Save / Share / explicit Send
```

Nothing is uploaded merely because a problem occurred. Macaros may retain bounded
technical reports locally, but every submission requires a review sheet and an explicit
**Send** action. Screenshots, program binaries, and guest-memory snapshots have separate
consent controls.

## What works today

Packaged and development launchers now set `AROS_REPORT_DIR` and keep structured 68k
diagnostics under its `session/` directory. Each session trace is capped at 4 MiB and at
most 20 generated session traces are retained. JIT crash bundles use the same private
report root. Raw call tracing remains opt-in and has a fixed 10,000-call ceiling.
The opt-in block and task debug streams are also capped and cannot be configured above
100,000 records.

The Macaros **Help** menu provides:

- **Report a Compatibility Problem...**, which explains that reports remain local and
  offers the report folder or the Macaros issue tracker;
- **Show Reports**, which reveals the local folder in Finder; and
- **Open Macaros Issue Tracker**, kept separate from the compatibility workflow.

Nothing is sent automatically. The full review, redaction, export, and submission UI in
the specification is not built yet. Existing development surfaces that remain useful:

- `graft/aros-ctl log` reads the hosted-session log, normally
  `/tmp/aros-window.log`.
- File -> Take Screenshot writes a timestamped PNG beneath `$AROS_RUN_DIR`; development
  launchers normally set that to `run/darwin-aarch64/`.
- The packaged app uses `~/Library/Application Support/AROS` as its writable state
  directory.
- Standalone JIT faults write a `.tar.gz` crash bundle and print its path; see
  [run68k crash bundles](../../../hosted/jit68k/run68k.md#crash-bundles).
- Standalone engine runs can opt into structured bridge traces through
  `EMU68K_BRIDGE_TRACE` and `EMU68K_BRIDGE_TRACE_LEVEL`; Macaros launchers configure
  the bounded local trace automatically.

The packaged folder is `~/Library/Application Support/AROS/Reports`; development
launchers use `run/darwin-aarch64/Reports`.

## What to send

The report review sheet explains the choice, but the practical rule is:

| Problem | Most useful evidence |
|---|---|
| Named capability gap | bounded log, structured crossing record, program hash |
| Incorrect drawing or UI | reproduction steps, redacted screenshot, call/event trace |
| JIT or CPU fault | crash report, 68k/host stacks, flight recorder; binary if shareable |
| Hang | non-destructive task dump, recent calls/events, screenshot |
| Wrong but plausible value | arguments/results trace plus binary or a minimal reproducer |

Source code is helpful but not normally required. The program binary is often the best
reproducer, but it is never attached automatically: it may be copyrighted or private.
Likewise, a guest-memory snapshot may contain document data and is treated as highly
sensitive.

See [spec.md](spec.md) for the normative package, consent, screenshot-redaction,
submission, security, and verification requirements.
