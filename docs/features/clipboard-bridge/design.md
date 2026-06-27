# Clipboard bridge — NSPasteboard ↔ AROS clipboard.device

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-24

## What & why

Two-way copy/paste between macOS and the hosted AROS: text first, images later.
Select-copy in a Mac app, paste in an AROS app; and the reverse. Concretely this
means macOS's `NSPasteboard` (the system general pasteboard) and AROS's
`clipboard.device` (unit `PRIMARY_CLIP`) stay in sync.

Why it's the right next feature: it's a *small, end-to-end exercise of the whole
Phase-2 thesis* — "macOS owns the drivers; AROS reaches them via standard exec
I/O." The Mac side owns the real pasteboard; AROS reaches it through a device
LVO call (send an `IOClipReq`, block for the reply). It is the same `IORequest →
device task → real macOS syscall → reply` shape already de-risked by the H11
file-backed device (`hosted/device.c`) and the H10 message ports
(`hosted/msgport.c`) — only the host work changes (NSPasteboard instead of
`pread`/`pwrite`). And it is trivially verifiable in the unattended loop: the
host can `put` a string on the pasteboard and assert AROS reads it back, with no
TCC/Screen-Recording prompt anywhere (clipboard access needs no entitlement).

## Does it already exist?

No. Evidence:

- No host clipboard glue in this repo. `grep -rniE
  'NSPasteboard|pbpaste|pbcopy|generalPasteboard|clipboard'` over
  `/Users/user/Source/aros-aarch64` (excluding `.git`) returns **nothing**. The
  hosted spikes H1–H12 (`hosted/*.c`) cover scheduler, ports, and a file-backed
  device, but no clipboard.
- No NSPasteboard anywhere in upstream either: `grep -rniE
  'NSPasteboard|pbpaste|pbcopy|generalPasteboard'`
  over `/Users/user/Source/aros-upstream` returns **nothing** — AROS has never had
  a macOS host backend at all.
- Upstream *does* ship the pieces we mirror: the device itself
  (`workbench/devs/clipboard/`), and the one existing **host** clipboard bridge —
  the X11 hosted HIDD's `arch/all-hosted/hidd/x11/x11_clipboard.c`, which couples
  the X11 PRIMARY/CLIPBOARD selections to AROS via a public message port named
  `"HOST_CLIPBOARD"`. That is the closest precedent and the shape we adapt to
  Cocoa (see Design). There is also a ready-made test client,
  `developer/debug/test/misc/hostcb.c`, that drives `"HOST_CLIPBOARD"` and wraps
  the text in IFF FTXT — effectively a reference implementation of half this
  feature.

### Prior-art landscape (independently reasoned, *not* in the AROS tree)

No macOS/NSPasteboard clipboard bridge exists for AROS — the in-tree X11 glue is
the only host-clipboard backend AROS has ever had, confirming the gap. There are
two competing architectures for bridging an Amiga-like clipboard to a host OS (the
*emulator-core virtual device* vs. the *in-guest host-helper*); both are directly
relevant to our scheme-A/B decision. We reason about them from first principles
only — no third-party implementation source was read, searched, or consulted:

- **Emulator-core virtual clipboard device (scheme-A shape).** A full-chipset
  Amiga emulator can mount a *virtual* Amiga clipboard device that parses/generates
  **IFF FORM FTXT / CHRS** in the emulator and copies the text to/from the host
  clipboard. We independently determined that such a virtual device needs
  IFF-truncation hardening (validate truncated IFF parsing to prevent crashes) and
  a startup size cap. This is **exactly our scheme A** (host-backed virtual device
  that does FTXT↔host text) — including the truncation/size-cap edge cases our C4
  robustness spike targets. *Catch:* the cross-platform/macOS side of such an
  emulator is not necessarily a clean NSPasteboard path (SDL/Cocoa text only), so
  we still write the pasteboard glue ourselves.

- **In-guest host-helper (guest-side helper shape).** The *opposite* architecture:
  an AmigaOS command running **inside** the guest that shells out to the host to
  read/write the real clipboard, with **macOS supported** (backends: macOS
  pasteboard, Linux `wl-clipboard`/`xclip`/`xsel`, Windows PowerShell). Most
  relevant detail: such a helper must transcode **Amiga ISO-8859-1 ↔ host encoding
  via `iconv` on macOS**, and it need not go through `clipboard.device`/IFF at all —
  it talks the host clipboard directly. This is the closest "Amiga-like ↔ macOS
  clipboard" shape, and it argues our "pass UTF-8 through unchanged" shortcut is
  wrong for non-ASCII: a serious bridge transcodes (Latin-1↔UTF-8), matching our
  Text-encoding risk. *Catch:* this pattern relies on a guest-side host-command
  escape hatch and is a guest-side helper, not a device/daemon — not a structure we
  can lift, only an encoding lesson.

- **Guest-agent + host-channel (scheme-B shape).** The general VM pattern: a
  **guest agent** plus a host channel carry the clipboard across the boundary. The
  industry norm is an in-guest agent cooperating with a host endpoint — our scheme B
  (sync daemon + stock device) is the AROS-shaped version of this. *Catch:*
  heavyweight (a serial transport, a whole agent protocol) and not Amiga-aware;
  useful only as the architectural analogy, nothing to reuse.

Net: the design space splits cleanly along our own A-vs-B axis (virtual device = A,
host-helper = guest helper, agent = B-style), and we independently determined that
the IFF-FTXT hardening/size-cap edge cases and the Latin-1↔UTF-8 conversion are
real requirements, not deferrable niceties.

## Background: the AROS clipboard.device contract (grounded)

The device lives at `workbench/devs/clipboard/` — `clipboard.c` (logic),
`clipboard_intern.h` (private structs), `clipboard.conf` (config), with the
public API in `compiler/include/devices/clipboard.h`. It is a normal exec device:
`clipboard.conf` declares `libbasetype struct ClipboardBase`, `beginio_func
beginio`, `abortio_func abortio`, so callers reach it the standard way —
`OpenDevice("clipboard.device", unit, ioreq, 0)` then `DoIO`/`SendIO` of an
`IOClipReq`.

### Commands

`SupportedCommands[]` (`clipboard.c:51`), dispatched in `beginio`
(`clipboard.c:334`):

- `CMD_READ` (2) — read clip bytes; `clipboard.c:400`, `readCb` at `:610`.
- `CMD_WRITE` (3) — write clip bytes; `clipboard.c:393`, `writeCb` at `:725`.
- `CMD_UPDATE` (4) — commit/close the current write; `:494`, `updateCb` at `:870`.
- `CBD_POST` (`CMD_NONSTD+0`) — announce "I will write soon", lazily; `:501`.
- `CBD_CURRENTREADID` / `CBD_CURRENTWRITEID` (`CMD_NONSTD+1/2`) — query IDs; `:560/:568`.
- `CBD_CHANGEHOOK` (`CMD_NONSTD+3`) — add/remove a change-notification hook; `:367`.
- `NSCMD_DEVICEQUERY` — new-style command query; `:345`.

(`CBD_*` numbers from `compiler/include/devices/clipboard.h:33-36`.)

### The IOClipReq and data model

`struct IOClipReq` (`devices/clipboard.h:42`) is an `IOStdReq`-shaped request plus
`io_Offset` and `io_ClipID`. The device is **a raw byte stream backed by a real
file** — it does *not* understand IFF. `writeCb`/`readCb` (`clipboard.c:725/610`)
just `Open`/`Seek`/`Read`/`Write` a host AROS file. The file path is computed in
`SetupDevice` (`clipboard.c:232`): `"%s%lu"` of `cb_ClipDir` + unit number, where
`cb_ClipDir` is `"CLIPS:"` if assigned else falls back to `"ram:clipboards/"`
(`clipboard.c:149-190`). So a clip is literally a file like `CLIPS:0`.

`io_ClipID` is the framing/transaction token:
- `CMD_WRITE` with `io_ClipID==0` starts a new clip; the device bumps
  `cu_WriteID` and returns it as the new `io_ClipID` (`writeCb`, `clipboard.c:735`).
  Subsequent writes pass that same ID to append; `CMD_UPDATE` with it commits and
  closes (`updateCb`, `:870`). The clip is **a sequence of WRITEs terminated by an
  UPDATE**.
- `CMD_READ` with `io_ClipID==0` starts a read: device bumps `cu_ReadID`, opens
  the file, returns the new ID (`clipboard.c:467-470`). Reads continue with that
  ID until `io_Actual==0` (EOF), at which point `io_ClipID` is set to `-1`
  (`readCb`, `:610`). A `CMD_READ` with `io_Data==NULL` advances the offset
  without copying — used to size or skip a clip (`readCb`, `:636`).

### IFF FTXT framing (a *caller* convention, not the device)

Text clips are an IFF `FORM FTXT` whose `CHRS` chunks hold the bytes — but the
**device never sees IFF**; the structure is imposed by `iffparse.library` writing
*through* the device. The chain (all grounded):

1. `OpenClipboard(unit)` (`workbench/libs/iffparse/openclipboard.c`) opens
   `clipboard.device` and returns a `ClipboardHandle` wrapping an `IOClipReq`.
2. The app sets `iff->iff_Stream = (IPTR)ClipboardHandle` and calls
   `InitIFFasClip(iff)`, which installs `ClipStreamHandler`
   (`workbench/libs/iffparse/clipboardfuncs.c`).
3. `ClipStreamHandler` maps IFF stream ops to device I/O: `IFFCMD_READ →
   CMD_READ`, `IFFCMD_WRITE → CMD_WRITE`, `IFFCMD_INIT` resets `io_ClipID=0` /
   `io_Offset=0`, `IFFCMD_CLEANUP` on write issues `CMD_UPDATE`
   (`clipboardfuncs.c:81-150`). So `PushChunk(iff, ID_FTXT, ID_FORM)` +
   `PushChunk(iff, 0, ID_CHRS)` + `WriteChunkBytes` end up as `CMD_WRITE`s, and
   `CloseIFF` triggers the `CMD_UPDATE` that commits.
4. Chunk IDs: `ID_FORM`, `ID_FTXT = MAKE_ID('F','T','X','T')`, `ID_CHRS =
   MAKE_ID('C','H','R','S')` (e.g.
   `workbench/classes/zune/texteditor/mcc/ClipboardServer.c:71`). The CHRS bytes
   are plain text (Latin-1/locale on classic Amiga; see the UTF-8 risk below).

The reference round-trip is `developer/debug/test/misc/hostcb.c`: to push host
text into AROS it does `OpenClipboard → InitIFFasClip → OpenIFF(IFFF_WRITE) →
PushChunk(ID_FTXT,ID_FORM) → PushChunk(0,ID_CHRS) → WriteChunkBytes(text) →
CloseIFF` (`hostcb.c:155-180`); to pull AROS text out it does `OpenIFF(IFFF_READ)
→ StopChunk(ID_FTXT,ID_CHRS) → ParseIFF` loop accumulating CHRS bytes
(`hostcb.c:225-282`). **We copy this verbatim** — only the source/sink of the
text changes from `"HOST_CLIPBOARD"` (X11) to NSPasteboard.

### Change notification

Two mechanisms, both grounded:
- **Hooks** (`CBD_CHANGEHOOK`): an app `ADDHEAD`s a `struct Hook` onto
  `cb_HookList` (`clipboard.c:381`). On every commit the device calls each hook
  with a `struct ClipHookMsg { chm_Type; chm_ChangeCmd; chm_ClipID; }`
  (`devices/clipboard.h:60`); `chm_ChangeCmd` is `CMD_UPDATE` after a normal write
  (`updateCb`, `clipboard.c:895`) or `CBD_POST` after a post (`:522`). This is the
  push signal "the AROS clipboard changed" we need to drive AROS→host sync.
- **POST/SatisfyMsg** (`CBD_POST` + `cu_PostPort`): lazy-write protocol where the
  owner declares it *will* provide data and is only made to `CMD_WRITE` when a
  reader actually appears (`clipboard.c:501`, `cu_PostPort`/`SatisfyMsg` in
  `clipboard_intern.h`). Useful later; not needed for the first text spikes.

### Relation to H10/H11

`OpenClipboard`→`DoIO(IOClipReq)` is exactly the H11 path: a client builds an
IORequest and `DoIO`s it; `BeginIO → PutMsg → device task → ReplyMsg → WaitIO`
(`hosted/device.c` header). `IOClipReq` is `IOStdReq` + two longs, so the H11
device dispatcher generalizes with a wider command switch. The change-hook signal
to a waiting host poller is the H10 `PutMsg`/`Signal` mechanism
(`hosted/msgport.c`). Nothing new in the exec plumbing — only the host backend.

## Design

The host owns the real pasteboard; AROS reaches it through `clipboard.device`.
Two viable couplings (decide at C0):

- **(A) Host-backed device unit.** A hosted `clipboard.device` whose `CMD_READ`/
  `CMD_WRITE`/`CMD_UPDATE` are serviced by a device task that, on `CMD_UPDATE`
  for a write, ships the accumulated bytes to NSPasteboard, and on `CMD_READ`
  start pulls the current pasteboard string into the clip buffer. Cleanest
  long-term: every AROS app that uses the clipboard hits the host with no extra
  daemon. This is the natural evolution of `hosted/device.c`.
- **(B) Sync daemon + stock device.** Run the unmodified file-backed
  `clipboard.device` and add a host-side AROS task that watches both ends and
  copies between `CLIPS:0` (via the device) and NSPasteboard — the exact role
  `x11_clipboard.c` + the `"HOST_CLIPBOARD"` port play under X11.

Start with **(B)** for the spikes (it reuses the stock device and the proven
`hostcb.c` IFF logic, so the AROS side is *known-good* and we only write the host
half), then migrate to **(A)** once text + change-detection are solid.

### Host side (NSPasteboard)

A small Objective-C (or C-with-objc-runtime) host module, `hosted/pasteboard.m`
(new), exposing C entry points callable from AROS via the H3 host-call shim
(`hosted/abishim.S`):

- `host_pb_get_text(char **out, size_t *len)` →
  `[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString]`,
  returned as malloc'd UTF-8. NULL if no string type present.
- `host_pb_set_text(const char *utf8, size_t len)` →
  `clearContents` then `setString:forType:NSPasteboardTypeString`.
- `host_pb_change_count(void)` → `[NSPasteboard generalPasteboard].changeCount`
  (monotonic `NSInteger`; bumps on *any* external write). This is the
  poll-for-change primitive — **UNVERIFIED** exact return-type marshalling
  through the shim; treat as `long`.
- (later) `host_pb_get_png` / `host_pb_set_png` for
  `NSPasteboardTypePNG`/`NSPasteboardTypeTIFF`.

**Non-blocking rule:** the host pasteboard calls happen on the *device/daemon
task's* switched stack (H1 property) under preemption — they must not block the
AROS scheduler. `stringForType:`/`setString:` are synchronous and fast, so a
short host call is fine; we do **not** install Cocoa notifications or run an
`NSRunLoop` (that would entangle AROS's signal-driven scheduler). Change
detection is *polled* via `changeCount`, not event-driven. **UNVERIFIED:** whether
`NSPasteboard` may be touched off the main thread — Apple's docs are ambiguous;
pasteboard reads/writes are generally thread-safe, but the spike must confirm
empirically (C1) and, if not, route pasteboard calls to the macOS main thread via
the existing host boot-anchor task (`hosted/exec.c` models the main thread as the
low-pri "boot" task).

### AROS side (clipboard.device, or a host-backed unit/handler)

For the spikes (scheme B) the AROS side is **pure stock AROS code**, lifted from
`hostcb.c`:

- *Host→AROS*: take the UTF-8 from `host_pb_get_text`, wrap it as `FORM FTXT /
  CHRS` and write it through `OpenClipboard(PRIMARY_CLIP)` + `InitIFFasClip` +
  `PushChunk/WriteChunkBytes/CloseIFF` (the `hostcb.c:155-180` sequence).
- *AROS→Host*: read the current clip with `OpenClipboard` + `StopChunk(ID_FTXT,
  ID_CHRS)` + `ParseIFF` loop (the `hostcb.c:225-282` sequence), concatenate the
  CHRS bytes, hand to `host_pb_set_text`.

For scheme A later, the device task's command handlers call the same host entry
points directly; the IFF wrap/unwrap still happens in the *caller* (iffparse), so
the device just sees CHRS-payload bytes — meaning a host-backed unit can ship the
clip file bytes to NSPasteboard *as-is* only if both sides agree to strip/add the
IFF envelope. Cleanest: keep the device IFF-agnostic and do FTXT↔UTF-8 in the
bridge layer, exactly as B does.

### The bridge (two-way sync + change detection; IFF FTXT ↔ UTF-8)

A single hosted AROS "clipboard-sync" task owns the policy:

1. **AROS→host:** register a `CBD_CHANGEHOOK` so a `CMD_UPDATE`/`CBD_POST` on
   `PRIMARY_CLIP` signals the task (hook just `Signal`s it — same pattern as
   `x11_clipboard.c`'s message port). On signal: read the FTXT clip → UTF-8 →
   `host_pb_set_text`. Record the resulting `host_pb_change_count` as "ours".
2. **host→AROS:** poll `host_pb_change_count` on a timer (reuse the H6 SIGALRM
   tick / a `timer.device`-style wait — **UNVERIFIED** which timer source the
   hosted port exposes yet). When it differs from "ours", read `host_pb_get_text`
   → wrap FTXT → write the AROS clip. Record the new count.
3. **Loop-break:** the recorded change-count and the AROS clip's `cu_WriteID`
   (`CBD_CURRENTWRITEID`) are the anti-ping-pong guards — never echo a value we
   just wrote. (The X11 bridge notes the same ping-pong hazard;
   `x11_clipboard.c` even disables an aggressive POST path to avoid it.)

**FTXT ↔ UTF-8:** CHRS bytes are "just text" to `clipboard.device`, but the bridge
may not treat them as opaque once the bytes cross into macOS. macOS pasteboard
strings are Unicode/UTF-8; AROS-side FTXT text for this bridge is interpreted as
ISO-8859-1 unless a later, explicit charset marker says otherwise. Therefore the
bridge layer does the same conversion the executable spec requires:
AROS/FTXT CHRS Latin-1 bytes → UTF-8 for `NSPasteboardTypeString`, and UTF-8 →
Latin-1 on the way back. Host characters outside Latin-1 must follow the spec's
lossy/escape policy instead of being silently copied as mojibake. ASCII remains the
trivial exact case, but non-ASCII is part of the required bridge, not a deferred
nicety.

## Plan — spikes in the loop

Each is one PASS/FAIL the harness greps for, no manual step.

- **[C0] Wiring + decision.** Build `hosted/pasteboard.m` against the objc
  runtime on aarch64-darwin; from a host-side test, set then get a string,
  print `[C0] PB rt=<string>`. PASS = round-trips host→host. Confirms NSPasteboard
  is reachable and picks scheme A vs B for the rest.
- **[C1] host→AROS text.** Host `host_pb_set_text("AROS<C1>")`; the sync task reads
  the host pasteboard, transcodes the text, writes a `FORM FTXT` clip to
  `PRIMARY_CLIP`, and an independent AROS reader prints `[C1] read=<...>`. PASS =
  AROS clip equals the host string after the documented charset policy.
- **[C2] AROS→host text.** AROS writes a FORM FTXT clip ("HELLO<C2>") via
  `OpenClipboard`/iffparse; host reads `host_pb_get_text` (≡ `pbpaste`); print
  `[C2] host=<...>`. PASS = pasteboard equals the AROS clip.
- **[C3] change detection both ways.** Wire the `CBD_CHANGEHOOK` + `changeCount`
  poller. Mutate one side, assert the other converges within N ticks without an
  explicit kick; print `[C3] a2h=ok h2a=ok`. PASS = both directions auto-sync,
  no ping-pong (assert exactly one propagation per change).
- **[C4] robustness.** Empty clip, a large clip (> `WRITEBUFSIZE` 4096, multiple
  CHRS / multi-segment writes), and a non-FTXT clip (assert graceful no-op).
  `[C4] big=ok empty=ok nonftxt=skipped`.
- **[C5] images (later).** PNG via `NSPasteboardTypePNG` ↔ an AROS image clip
  (datatypes / ILBM — **UNVERIFIED** target AROS image-clip format). Deferred.

## How we verify it unattended

The whole feature is observable through the host with **no TCC / Screen-Recording
prompt** — pasteboard access needs no entitlement, so the agent never hits an
approval dialog. The loop, in the established H1–H12 style:

- **Markers:** each spike prints a unique `[C#] …` line to the serial/stdout log;
  the harness greps for it and the expected payload (e.g. `read=AROS<C1>`) →
  PASS/FAIL block. Same marker discipline as `[H1]…[H12]`.
- **Read-back through the host, not the screen:** for AROS→host, the test asserts
  by calling `host_pb_get_text` (or shelling `pbpaste`) from the host side and
  string-comparing — the data is verified *as bytes*, not by looking at a window.
  For host→AROS, the host `put`s a known string and AROS echoes what it read.
  This is the clipboard-as-oracle technique: ground-truth text crosses the
  boundary and is asserted equal on the far side.
- **Determinism:** known fixed strings with embedded marker tags (`<C1>` etc.) so
  a partial/garbled transfer fails the exact-match. Change-count values are
  printed so a ping-pong shows up as an extra propagation in the log.
- **Clean exit / watchdog:** PASS exits fast via the existing semihosting/`SYS_EXIT`
  path; a hung sync loop is reaped by the harness watchdog. No spike waits on a
  human.

## Risks & open questions

- **Off-main-thread NSPasteboard.** Whether pasteboard calls are safe from the
  device/daemon task's stack vs. requiring the macOS main thread. C1 must prove
  it; fallback is to marshal to the boot-anchor (main) task. **UNVERIFIED.**
- **Text encoding.** FTXT CHRS codepage vs. macOS UTF-8. ASCII is fine; non-ASCII
  needs transcoding (where? — bridge layer). Risk of mojibake on round-trip. We
  independently determined that a serious Amiga-like↔macOS clipboard bridge must
  **not** pass bytes through — it must transcode Amiga ISO-8859-1 ↔ host via
  `iconv` on macOS. So Latin-1↔UTF-8 is a real requirement, not a deferrable
  nicety; the bridge layer should `iconv`-style transcode rather than assume opaque
  bytes.
- **changeCount races.** `changeCount` is monotonic but our "ours vs theirs"
  bookkeeping can race if both sides write between polls; need the write-ID +
  count double-guard, and accept last-writer-wins. Polling latency is a tunable.
- **Ping-pong.** AROS write → host set → changeCount bumps → looks like a host
  change → AROS write … The X11 bridge hit exactly this; we break the loop by
  tagging the value we just wrote (count + `cu_WriteID`).
- **Large clips & multi-segment.** Clips span multiple `CMD_WRITE`s and CHRS
  chunks (> `WRITEBUFSIZE`=4096, `clipboard.c:39`); the bridge must accumulate,
  as `hostcb.c`'s read loop does. Very large pasteboard items (images) need a
  size cap / streaming.
- **Ownership / flushing.** NSPasteboard ownership and lazy providers; AROS's
  `CBD_POST` lazy-write. For text we use eager copies; lazy providers on either
  side are a later optimization, with the matching ping-pong/ownership care.
- **Image formats.** `NSPasteboardTypePNG`/`TIFF` ↔ AROS image-clip representation
  (ILBM? a datatype?) is unresolved — **UNVERIFIED** what AROS apps expect as an
  image clip; C5 must establish a concrete target before promising images.
- **Timer source.** The host→AROS poller needs a periodic wake; which timer the
  hosted port exposes (H6 SIGALRM tick vs. a `timer.device`) is **UNVERIFIED** at
  this milestone.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- `workbench/devs/clipboard/clipboard.c` — device logic: command dispatch
  (`beginio`, `:334`), `readCb`/`writeCb`/`updateCb` (`:610/:725/:870`),
  clip-dir/file (`:149-190`, `:232`), hooks (`:367`, `:895`), POST (`:501`).
- `workbench/devs/clipboard/clipboard_intern.h` — `ClipboardBase`,
  `ClipboardUnit` (`cu_ReadID/cu_WriteID/cu_PostID/cu_PostPort`), `PostRequest`.
- `workbench/devs/clipboard/clipboard.conf` — device config (beginio/abortio).
- `compiler/include/devices/clipboard.h` — `IOClipReq`, `ClipHookMsg`,
  `SatisfyMsg`, `PRIMARY_CLIP`, `CBD_*`.
- `workbench/libs/iffparse/openclipboard.c` — `OpenClipboard` → `ClipboardHandle`.
- `workbench/libs/iffparse/clipboardfuncs.c` — `ClipStreamHandler`
  (IFF stream cmd → `CMD_READ/WRITE/UPDATE`).
- `developer/debug/test/misc/hostcb.c` — reference host-clipboard tool: FTXT
  wrap/unwrap + `"HOST_CLIPBOARD"` `'R'`/`'W'` protocol. **Adapt to NSPasteboard.**
- `arch/all-hosted/hidd/x11/x11_clipboard.c` — the existing host-clipboard bridge
  (X11 selections ↔ `"HOST_CLIPBOARD"` port); ping-pong/async-state precedent.
- `workbench/classes/zune/texteditor/mcc/ClipboardServer.c` (`:71-103`),
  `.../private.h:735` — `ID_FTXT`/`ID_CHRS`/`ID_FORM` definitions + a real FTXT
  writer.

This project (`/Users/user/Source/aros-aarch64`):
- `hosted/device.c` — H11 file-backed device (`IORequest → device task → real
  syscall → reply`); the dispatcher we widen for `IOClipReq`.
- `hosted/msgport.c` — H10 message ports (`PutMsg`/`GetMsg`/`ReplyMsg` +
  `Signal`); the hook/daemon signalling path.
- `hosted/abishim.S`, `hosted/abishim.c` — H3 host-call shim (Apple arm64 stack
  varargs); how AROS calls `host_pb_*`.
- `hosted/exec.c` — scheduler + macOS main thread modelled as the boot-anchor
  task (the off-main-thread fallback).
- `NOTES.md` — thesis, H1–H12 log, marker/unattended-loop discipline.

New (to write): `hosted/pasteboard.m` — `host_pb_{get,set}_text`,
`host_pb_change_count`, later `host_pb_{get,set}_png`.

Prior-art shapes (architectural analogies only; *not* in the AROS tree, and no
third-party implementation source was read, searched, or consulted):
- Emulator-core virtual clipboard device + IFF FTXT host sync (scheme-A analog;
  IFF-truncation hardening + size cap).
- In-guest host-helper — macOS clipboard backend, ISO-8859-1↔host transcode via
  `iconv` (encoding precedent, *not* via clipboard.device/IFF).
- Guest-agent + host-channel clipboard pattern (scheme-B analog).
