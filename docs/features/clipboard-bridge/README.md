# Clipboard bridge — share copy/paste between macOS and AROS

> Status: **working on darwin-aarch64 (2026-06-27)** — host→AROS verified
> byte-exact end-to-end; AROS→host implemented (symmetric path).
> This is the practical "what & how". For the design and the
> implementation spec, see [design.md](design.md) and [spec.md](spec.md).

## What it does

Bridges the macOS system clipboard (`NSPasteboard`) and the AROS clipboard
(`clipboard.device`, unit `PRIMARY_CLIP`) so plain **text** copied on one side can
be pasted on the other:

- **Copy on the Mac → paste in AROS.** Select text in any Mac app and ⌘C, then in
  the AROS console paste with **Right-Amiga + V** (ConClip) — your Mac text appears.
- **Copy in AROS → paste on the Mac.** Mark text in the AROS console and
  **Right-Amiga + C**, then ⌘V in any Mac app — the AROS text appears.

No files to shuffle: by default the sync runs continuously in the background while
AROS is up. The Settings panel / Machine menu can pause it at runtime. Text crosses
the charset boundary correctly (see below), and a clip you just sent never echoes
back as a phantom second copy.

## Quick start

```sh
make pasteboard-dylib          # build the host shim (build/libpasteboard.dylib)
graft/run-window.sh            # boot AROS in a window — the bridge starts itself
```

`run-window.sh` (and the `aros-ctl` test harness) now deploy `libpasteboard.dylib`
to `~/lib` automatically alongside `cocoametal.dylib`, and the AROS driver starts
the sync at boot. You'll see it in the boot log:

```
[Cocoa] clipboard bridge up: NSPasteboard <-> PRIMARY_CLIP
```

Then just copy on one side and paste on the other. That's the whole feature.

## How it works (a polling bridge, no cross-thread signals)

There is **no shared memory and no emulated hardware.** Two independent clipboards
exist — `NSPasteboard` on the Mac, `clipboard.device` in AROS — and a small AROS
**process** keeps them in step by *polling both change counters* and copying the
delta across, in whichever direction changed:

```
   macOS app            AROS app / ConClip
      │ ⌘C / ⌘V             │ R-Amiga C/V
      ▼                     ▼
  NSPasteboard         clipboard.device (PRIMARY_CLIP)
      ▲                     ▲   │ file-backs to
      │ libpasteboard.dylib │   ▼
      │ (hostlib.resource)  │  CLIPS:  (a DOS dir)
      └─────────┬───────────┘
                ▼
     cocoa.hidd clipboard  ── a Process in the Cocoa driver
       every ~0.2s:  hostPB.changeCount?  vs  CBD_CURRENTWRITEID?
         host changed → get text → UTF-8→Latin-1 → write FTXT to PRIMARY_CLIP
         AROS changed → read FTXT  → Latin-1→UTF-8 → set NSPasteboard
```

Why polling rather than a signal: under the threaded host scheduler, a cross-thread
`Signal()` from AppKit into an AROS task is fragile. Each side already exposes a
cheap monotonic "did anything change" counter — `[NSPasteboard changeCount]` on the
Mac and `CBD_CURRENTWRITEID` in `clipboard.device` — so the bridge just compares
them on a timer and never blocks.

**No phantom echoes.** When the bridge writes a side, it records the resulting
change id (`ourHostWrite` / `ourArosWrite`) and ignores that one bump, so a clip
that crossed over does not immediately bounce back as a new copy and ping-pong.

**The payload is real Amiga IFF.** A clip written to AROS is a proper
`FORM … FTXT … CHRS` chunk — exactly what every Amiga clipboard-aware app expects —
built/parsed with `iffparse.library`. (Verified: a Mac clip of `DAEDALOS_CLIP_4242`
lands in AROS as `464f524d…4654585443485253…` = `FORM…FTXT…CHRS` + the 18 bytes.)

## Text — accents and the charset boundary

AmigaOS text is **ISO-8859-1 (Latin-1)**; macOS is **UTF-8**. The host shim
transcodes both ways so accented text round-trips:

- A Mac `café` / `grün` arrives in AROS as the correct Latin-1 bytes.
- AROS Latin-1 text becomes UTF-8 on the Mac clipboard.
- Characters Latin-1 cannot represent (e.g. `€`, `œ`) are dropped/approximated on
  the way *into* AROS — Latin-1 simply has no code point for them. Plain text and
  the full Latin-1 range are lossless.

(Images and other flavors are out of scope for now — text only. The design doc
sketches how `ILBM`/PNG would layer on later.)

## Where the code lives

**Host side (this repo — `aros-aarch64`):**

- [hosted/clipboard/pasteboard.m](../../../hosted/clipboard/pasteboard.m) +
  `pasteboard.h` — the `NSPasteboard` shim: `host_pb_get_text` / `host_pb_set_text`
  / `host_pb_change_count`, the self-write token, and the Latin-1↔UTF-8 transcode
  (`host_latin1_to_utf8` / `host_utf8_to_latin1`). Built into
  **`build/libpasteboard.dylib`** via `make pasteboard-dylib` (exports in
  `hosted/clipboard/pasteboard.exports`); proven standalone with `make pasteboard-abi`
  (`[PBABI] PASS`) and `make hosted-clipboard` (`[C] PASS`).
- Deployed to `~/lib` by [graft/run-window.sh](../../../graft/run-window.sh) and
  [graft/aros-ctl](../../../graft/aros-ctl), beside `cocoametal.dylib`.

**AROS side (`aros-upstream`, branch `aarch64-darwin-graft`):**

- `arch/all-darwin/hidd/cocoa/cocoa_clipboard.c` — the sync **process**
  (`CreateNewProcTags`, because it touches `clipboard.device`, which file-backs
  `CLIPS:`, a DOS path that needs a Process context). Polls both sides, transcodes
  via the shim, suppresses its own echoes, frames/parses IFF FTXT.
- `arch/all-darwin/hidd/cocoa/cocoa_intern.h` — `struct PBInterface` (the shim's
  function table) + the clipboard fields on `struct cocoahidd`.
- `arch/all-darwin/hidd/cocoa/startup.c` — starts the bridge after input init
  (non-fatal: no `libpasteboard.dylib` → the bridge just no-ops).
- `arch/all-darwin/hidd/cocoa/mmakefile.src` — builds `cocoa_clipboard` into `Cocoa`.

The shim loads into AROS exactly like the display shim: by bare name through
`hostlib.resource` (`HostLib_Open("libpasteboard.dylib")` → `GetInterface`), which
is why it must sit in `~/lib` with `DYLD_FALLBACK_LIBRARY_PATH` pointing there.

## How it's wired into the boot

`run-window.sh` / `aros-ctl` set up the AROS side so console copy/paste works:

```
Assign CLIPS: SYS:clips           # clipboard.device backing dir (host-visible)
Run >SYS:conclip.log ConClip      # console <-> clipboard.device (R-Amiga C/V)
```

`clipboard.device` + `iffparse.library` + `con-handler` + `ConClip` are added to
the kickstart module set. With `CLIPS:` assigned to a host folder, a clip is also
a readable file at `<AROS>/clips/0` — handy for verification.

## Hardening Notes

The bridge is intentionally conservative:

- It waits for the `ConClip.rendezvous` port before opening `clipboard.device`.
  That is the readiness gate; do not replace it with an arbitrary delay.
- It is a `Process`, not a bare task, because `clipboard.device` file-backs to
  `CLIPS:` and needs DOS process context.
- Host calls remain wrapped in the existing `Forbid()` + `HostLib_Lock()`
  discipline. The host calls are short; do not hold this lock while doing AROS
  file/IFF work.
- Text payloads are capped at 1 MiB in both directions. Oversized Mac text and
  oversized AROS FTXT clips are skipped with a log line instead of allocating
  unbounded memory inside the poll task.
- A failed `NSPasteboard` write no longer advances the self-write token or host
  baseline. Failed writes remain visible as failures instead of being mistaken
  for a successful bridge event.
- IFF `CHRS` chunks are concatenated with overflow checks; invalid/empty chunks
  are ignored, and non-FTXT clips are skipped.
- The Settings / Machine menu "Share Clipboard" toggle defaults to enabled, matching
  the bridge's historical behavior. AROS-facing settings are intentionally not
  injected during `cm_open`; doing so can fire while the guest is still in early
  display/load bring-up. Runtime toggles remain the safe path until a guest-ready
  persisted-settings handoff exists.
- Runtime `CM_OPT_CLIPBOARD_SHARE` changes are consumed by the AROS-side Cocoa input
  task; they flip a clipboard-enable flag observed by the bridge. Disabling pauses
  both directions; re-enabling re-baselines both change counters so stale clips are
  not replayed.
- The control harness can exercise this path without opening the Settings panel:
  `graft/aros-ctl clipboard off` / `graft/aros-ctl clipboard on`.
- `graft/clipboard-smoke` now verifies the Mac→AROS path and the runtime toggle:
  it deploys current artifacts, explicitly enables sharing so local user defaults
  cannot skew the test, proves a token crosses while enabled, a token does not
  cross while disabled, re-enable re-baselines without replaying the disabled
  token, and a fresh token crosses after resume.

Still to wire:

- There is still no explicit bridge shutdown path; host process exit tears it
  down. Proper app lifecycle should add a stop signal before closing host shims.

## Verifying it works — layer by layer

The bridge is a chain of links; copy/paste "not working" almost always means one
specific link is down. The sync **process logs every event** (DEBUG build), so you
can watch exactly where a clip stops. Tail the log filtered to the bridge:

```sh
tail -f /tmp/aros-window.log | grep 'clip:'      # or: graft/aros-ctl log 40
```

For the repeatable host-side regression test:

```sh
./graft/clipboard-smoke
```

This uses `pbcopy` as the Mac-side source and the bridge log as the oracle. It
saves `run/darwin-aarch64/clipboard-smoke-<timestamp>.log` and restores the
previous textual macOS clipboard content when possible. It proves Mac→AROS and
runtime pause/resume; AROS→Mac still needs a real selection test or an AROS-side
clip-writer helper before it can be fully unattended.

| # | Link | How to check | Healthy sign |
|---|------|--------------|--------------|
| 0 | Host shim deployed | `ls -l ~/lib/libpasteboard.dylib` | the file exists (else `make pasteboard-dylib`) |
| 1 | Bridge running | look at the boot log | `clipboard bridge up` + `baseline macOS cc=… AROS wid=…`, then a `heartbeat` every ~30s |
| 2 | **Mac → AROS** reaches the AROS clipboard | ⌘C text in a Mac app | `macOS pasteboard changed (cc X->Y)` → `host->AROS N bytes "…" -> PRIMARY_CLIP` |
| 3 | AROS pastes it | Right-Amiga+V in the AROS console | your Mac text appears at the prompt (this is ConClip) |
| 4 | **AROS → Mac** reaches NSPasteboard | mark console text + Right-Amiga+C | `AROS clipboard changed (wid X->Y)` → `AROS->host N bytes "…" -> NSPasteboard` |
| 5 | Mac pastes it | ⌘V in a Mac app | your AROS text appears |

Reading the log lines:

- `host->AROS …` / `AROS->host …` — a clip **crossed**; the direction works. The
  quoted preview and byte count are the actual text.
- `…it's our own … write, ignored` — echo suppression firing. **Not** a bug; it's
  the bridge refusing to bounce a clip it just delivered back the other way.
- `macOS pasteboard changed` with **no** following `host->AROS` — the Mac clip has
  no plain-text flavour (it's an image/file), or transcoding failed; the line says which.
- **No** `AROS clipboard changed` after a console copy — the copy never wrote the
  AROS clipboard (selection/ConClip), so there is nothing for the bridge to forward.
  That's upstream of the bridge, not the bridge itself.

This is the fast triage: if you see step 2/4's `->` line, the bridge did its job and
the problem (if any) is in the console copy/paste; if you don't, the problem is the
shim, the bridge, or the clipboard write.

## Status

| Direction | State |
|-----------|-------|
| **Mac → AROS** (⌘C on Mac, **Right-Amiga+V** in the shell) | **verified end-to-end** — pasted text appears at the prompt |
| **AROS → Mac** (select console text + **Right-Amiga+C**, ⌘V on Mac) | **verified chain** — a real console copy reaches the Mac clipboard (needs a real mouse text-selection) |
| Echo suppression | in place (self-write tokens both sides) |
| Latin-1 ↔ UTF-8 | in place and unit-tested in the host shim |
| Images / non-text flavors | not implemented (text only) |

> **The Amiga key:** on a Mac keyboard **either ⌘** (left or right) maps to
> Right-Amiga, so ⌘C/⌘V *inside the AROS window* act as the Amiga clipboard keys
> Right-Amiga+C/V — not macOS copy/paste. The Daedalos **Edit menu** Copy/Paste
> items synthesize those same Right-Amiga+C/V chords into AROS.

### Requires up-to-date console binaries

Shell copy/paste is handled by **`console.device`** + **`con-handler`** (not ConClip —
ConClip's edit hook only covers string gadgets). The Right-Amiga+C/V detection is a
recent fix in `rom/devs/console/consoletask.c` + `rom/filesys/console_handler/con_handler.c`;
**if those binaries are stale, paste silently does nothing.** Rebuild them:

```sh
make kernel-console-quick kernel-fs-con-quick   # in the AROS build tree
```

`run-window.sh` / `aros-ctl` then pull the rebuilt `console.device` + `con-handler` into
the kickstart automatically.

The bridge defaults to sharing enabled. The Settings / Machine menu "Share
Clipboard" toggle, and `graft/aros-ctl clipboard on|off`, pause/resume the running
bridge through `CM_OPT_CLIPBOARD_SHARE`.
