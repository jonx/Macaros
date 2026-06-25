# Implementation spec — NSPasteboard ↔ AROS clipboard.device bridge

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Clean-room banner

**Role B (implementer): do NOT read WinUAE, FS-UAE, Amiberry / `host-tools`,
E-UAE/Janus-UAE, vAmiga, QEMU/SPICE `vdagent`, or any GPL emulator/agent source.**
Implement only from this spec + the approved sources cited by tag: `[PUB]` Apple
framework docs / published standards (the IFF-85 / FTXT spec, NSPasteboard, POSIX
`iconv`), `[AROS]` in-tree AROS headers and drivers (paths given), `[OURS]` this
project's spikes (the H-series). `[REF-CONFIRM]` items were sanity-checked by Role A
against the GPL clipboard bridges named above — WinUAE/FS-UAE's virtual
`clipboard.cpp` (IFF FTXT host sync with truncation hardening + a size cap) and
Amiberry's `host-clip` (ISO-8859-1↔host transcode via `iconv`) — but each is
**restated here with an independent `[PUB]`/`[AROS]`/`[OURS]` justification**.
Implement from that justification, not from any reference. The one in-tree shape we
**do** adapt directly is non-GPL: `developer/debug/test/misc/hostcb.c` `[AROS]`
(APL/LGPL), swapping its X11 `"HOST_CLIPBOARD"` source/sink for NSPasteboard.

## Scope

**In.** A two-way copy/paste bridge for `aarch64-darwin` hosted AROS that keeps the
macOS system pasteboard (`NSPasteboard generalPasteboard`) and AROS
`clipboard.device` unit `PRIMARY_CLIP` in sync: text first, images later. It (1)
wraps/unwraps IFF `FORM FTXT` around `clipboard.device` exactly as `hostcb.c` does;
(2) transcodes the CHRS text payload between the Amiga byte encoding (ISO-8859-1)
and macOS UTF-8 in **both** directions — a requirement, not a nicety; (3) detects
changes on **both** ends without polling the AROS device — a host thread polls
NSPasteboard `changeCount`, the AROS device pushes via `CBD_CHANGEHOOK`; and (4) is
verifiable unattended through the host with **no TCC / Screen-Recording prompt**
(pasteboard access needs no entitlement).

**Decision (confirmed in [design.md](design.md)).** Ship **scheme B** first — the
unmodified file-backed `clipboard.device` plus a hosted AROS "clipboard-sync" task
that bridges `CLIPS:0` (through the device, via iffparse) to NSPasteboard, the exact
role `x11_clipboard.c` + the `"HOST_CLIPBOARD"` port play under X11. The AROS half is
then **pure stock AROS code lifted from `hostcb.c`**; only the host half is new.
Migrate to **scheme A** (a host-backed `clipboard.device` unit whose command handlers
call the host directly) once text + change-detection + transcode are solid. This spec
covers B end-to-end and specifies the A interface so the migration is a re-wiring, not
a redesign.

**Out (non-goals, this spec).** Lazy/deferred pasteboard providers (`CBD_POST` +
`SatisfyMsg` and NSPasteboard lazy `NSPasteboardWriting` — eager copies only at
first); multiple pasteboard items / multiple `NSPasteboardType`s per copy (one text
item, later one image item); rich text / RTF / HTML flavours (plain string only);
drag-and-drop pasteboards (`NSDragPboard` etc. — general pasteboard only);
pasteboard ownership change notifications via Cocoa (`changeCount` polling instead).

## Architecture

Two layers joined by a **flat hand-written C ABI** (the ABI header is ours, ASCII, no
GPL lineage). Unlike the display HIDD, **no Cocoa object outlives a call** and there is
**no run loop** — every host entry is a short synchronous pasteboard touch.

```
AROS side (stock AROS code + sync task)          Host side (Apple toolchain)
┌────────────────────────────────────┐           ┌──────────────────────────────┐
│ clipboard.device (STOCK, scheme B) │           │ libpasteboard.dylib          │
│  · CLIPS:0 file-backed (H11 shape) │           │  · NSPasteboard generalPb    │
│                                    │           │  · stringForType: / setString│
│ clipboard-sync task  [OURS+AROS]   │  hostlib  │  · changeCount (NSInteger)   │
│  · iffparse FTXT wrap/unwrap       │  + H3     │  · (later) PNG/TIFF data     │
│    (hostcb.c shape)                │ ────────► │                              │
│  · ISO-8859-1 <-> UTF-8 transcode  │   C ABI   │  poller thread:              │
│  · CBD_CHANGEHOOK  (AROS->host)    │ ◄──────── │   watches changeCount,       │
│  · changeCount poll (host->AROS)   │  Signal   │   raises an AROS Signal       │
└────────────────────────────────────┘           └──────────────────────────────┘
        PRIMARY_CLIP  ==  CLIPS:0  ◄── FTXT bytes ──►  CHRS text  ◄─transcode─►  UTF-8 on NSPasteboard
```

- **Host shim** `[OURS]` — Objective-C (`.m`) + C, `hosted/pasteboard.m` (new), built
  with the **host** clang (NOT AROS crosstools), the peer of `hosted/display.c` (H7).
  It owns the NSPasteboard access and the `changeCount`-poller thread, and exposes the
  C ABI below. It pulls **no** AROS headers; the only AROS-aware thing it holds is an
  opaque `(task, sigbit)` pair handed in by the AROS side, used solely to fire a
  `Signal` (see "Change-detection model").
- **AROS sync task** `[OURS]` + `[AROS]` — a single hosted AROS task that owns the
  bridge policy (the `x11_clipboard.c` role). It reaches the shim through
  `hostlib.resource` (`dlopen` of the dylib, `HostLib_GetPointer` for each ABI symbol)
  across the H3 host-call boundary, and reaches `clipboard.device` through stock
  iffparse exactly as `hostcb.c` does.
- Spike-phase paths: shim in `hosted/pasteboard.m`; the sync logic as a hosted spike
  alongside `hosted/device.c`. At graft, the AROS side lands in the proposed
  `arch/all-darwin/` hosted tree; scheme A would land a unit in a hosted
  `clipboard.device` variant.

## The C ABI (`pasteboard.h`)

Hand-authored, neutral. Types are ours; the *behaviour* of each call is specified
below. `[PUB]` Apple objects under the hood, `[AROS]` shapes driven by the bridge's
needs. **All strings crossing this ABI are UTF-8** (macOS-native); the ISO-8859-1
side lives entirely on the AROS side of the wall.

```c
#include <stddef.h>

/* Opaque AROS handle the host poller signals on a host->AROS change. The host
   treats these as two integers it passes back to one host->AROS callback symbol;
   it never dereferences a task pointer itself (it cannot — it has no AROS ABI).
   See "Change-detection model" for why this is a callback, not a raw Signal. */
typedef struct { void *signal_task; int signal_bit; } PBSignalTarget;

/* --- text (spikes C0..C4) --- */

/* Current pasteboard string as freshly malloc'd UTF-8 (NUL-terminated; *len is
   the byte length excluding the NUL). Returns 0 and *out=NULL if the pasteboard
   holds no string-typed item. Caller frees with host_pb_free. */
int    host_pb_get_text(char **out, size_t *len);

/* Replace the pasteboard with one UTF-8 string item. clearContents first, then
   set the string. Returns the changeCount value the write produced (see below),
   or -1 on failure. len excludes any NUL. */
long   host_pb_set_text(const char *utf8, size_t len);

/* Monotonic NSInteger snapshot; bumps on ANY external pasteboard write. The
   poll-for-change primitive. Marshalled as host long. */
long   host_pb_change_count(void);

void   host_pb_free(void *p);   /* free a buffer host_pb_get_text returned */

/* --- change poller (spike C3) --- */

/* Start a host background thread that polls host_pb_change_count and, when it
   differs from the value last seen, fires the host->AROS change callback (below)
   with `target`. interval_ms is the poll cadence. Idempotent / one poller per
   process. Returns 0 on success. */
int    host_pb_poller_start(PBSignalTarget target, int interval_ms);
void   host_pb_poller_stop(void);

/* The poller does NOT call exec.Signal itself (it is a host pthread with no AROS
   context). It calls back THROUGH a single function pointer the AROS side
   installs at init, whose body runs the actual exec Signal(task, 1<<bit). The
   AROS side registers it via host_pb_set_signal_cb so the host stays AROS-blind. */
typedef void (*PBSignalFn)(void *signal_task, int signal_bit);
void   host_pb_set_signal_cb(PBSignalFn fn);

/* --- images (spike C5, later; interface fixed now) --- */
int    host_pb_get_png(void **out, size_t *len);          /* NSPasteboardTypePNG */
long   host_pb_set_png(const void *png, size_t len);
```

`host_pb_set_signal_cb` + `PBSignalFn` are the discipline that keeps the wall clean:
the host never links exec and never learns the AROS `Signal` ABI; it only calls a
function pointer the AROS side gave it. The body of that function (on the AROS side)
performs `Signal((struct Task*)task, 1L << bit)`. **UNVERIFIED** whether a host
pthread may call into AROS `Signal` safely under the H6 single-thread scheduler — see
"Change-detection model" for the constraint and the fallback (a self-pipe / pollable
flag the AROS sync task checks on its own tick instead of a cross-thread `Signal`).

## Change-detection model (the load-bearing constraint) — `[REF-CONFIRM]`, restated

This is the hard part. Two ends mutate independently; each must learn of the other's
change **without** busy-reading the far side, and the system must not ping-pong. The
GPL bridges confirmed two-way change-sync is the crux and that a naive echo loops
forever; the requirements below are restated from independent footing.

**R-DETECT-A (AROS→host): `CBD_CHANGEHOOK`.** `[AROS]` The sync task registers a
`struct Hook` on `PRIMARY_CLIP` via `CBD_CHANGEHOOK`. On every commit the device
calls the hook with `struct ClipHookMsg { chm_Type; chm_ChangeCmd; chm_ClipID; }`
(`compiler/include/devices/clipboard.h:63`), `chm_ChangeCmd == CMD_UPDATE` after a
normal write or `CBD_POST` after a post (`workbench/devs/clipboard/clipboard.c:895`
`[AROS]`). The hook body does the minimum — it `Signal`s the sync task (the proven
pattern: `x11_clipboard.c`'s `reply_async_request` ends by
`Signal(mp->mp_SigTask, 1L<<mp->mp_SigBit)`, `arch/all-hosted/hidd/x11/x11_clipboard.c:65`
`[AROS]`). The task, on that signal, reads the FTXT clip, transcodes, and calls
`host_pb_set_text`. **No polling of the AROS device** — the device pushes.

**R-DETECT-H (host→AROS): `changeCount` poll.** `[PUB]` NSPasteboard has no
KVO/notification for "someone else wrote"; the **only** public detection primitive is
`changeCount`, a monotonic `NSInteger` that bumps on any external write (Apple
AppKit docs). So the host MUST poll it on a thread (R-POLLER) and, on a delta, fire
the host→AROS callback to wake the sync task, which then reads `host_pb_get_text`,
transcodes, and writes the AROS clip. Cadence is a tunable (default 200 ms); latency,
not correctness, scales with it.

**R-POLLER (the poller thread).** `[OURS]` + `[PUB]` `host_pb_poller_start` spawns one
host `pthread` that loops: read `changeCount`; if it differs from the last value it
saw AND differs from "ours" (R-LOOPBREAK), invoke `PBSignalFn(target.signal_task,
target.signal_bit)`; sleep `interval_ms`. The thread touches **only** NSPasteboard
read APIs (`changeCount`, and — see R-PBTHREAD — possibly `stringForType:`) and the
installed callback; it never touches AROS memory directly.

**R-PBTHREAD (thread-safety of NSPasteboard off the main thread).** **UNVERIFIED**,
`[PUB]`-ambiguous. Apple does not document `NSPasteboard` as main-thread-only the way
it does `NSWindow`/`NSView`; pasteboard reads/writes are widely used off-main but the
contract is not guaranteed. The spike (C1) MUST confirm empirically that
`changeCount` + `stringForType:` + `setString:` work from a non-main host thread. If
they do not, the fallback is: the poller thread reads **only** `changeCount` (cheap,
believed safe) and the actual `stringForType:`/`setString:` are marshalled to the
macOS main thread (modelled as the H4 low-pri boot/anchor task, `hosted/exec.c`)
exactly as the display HIDD confines Cocoa to one task. This is the same
single-owner-task discipline as `cocoa-metal-display/spec.md` R-THREAD `[OURS]`; here
it is conditional on the C1 result rather than mandatory.

**R-LOOPBREAK (anti-ping-pong — the requirement the bridges proved you cannot skip).**
`[REF-CONFIRM]`, restated `[OURS]`+`[AROS]`. Every cross-write itself bumps the other
end's change indicator, so a naive bridge loops: AROS write → `CBD_CHANGEHOOK` fires →
host set → `changeCount` bumps → looks like a host change → AROS write → … . The X11
bridge hit exactly this hazard (its async read-state machine and a deliberately
disabled aggressive path exist to avoid re-entrant propagation,
`x11_clipboard.c` `[AROS]`). Restated as an independent invariant the implementer can
verify: **the bridge holds two "self-written" tokens and never re-propagates a value
it just wrote.**

- *Host token:* `host_pb_set_text` returns the `changeCount` value its own write
  produced (`[PUB]`: read `changeCount` immediately after `setString:`). The sync task
  records that as `ours_host`. The poller suppresses any delta equal to `ours_host`.
- *AROS token:* after writing the AROS clip the task records the unit's write id via
  `CBD_CURRENTWRITEID` (`compiler/include/devices/clipboard.h:35` `[AROS]`). When the
  `CBD_CHANGEHOOK` fires, the task compares `chm_ClipID` (and/or the current write id)
  against `ours_aros`; if equal, it is the echo of its own write and is dropped.
- *Direction lock:* while propagating one direction the task sets a `busy` flag so a
  hook/poller event that arrives mid-propagation is coalesced, not acted on
  re-entrantly. Last-writer-wins if both ends change between ticks (accepted).

  Acceptance (C3): mutate one side once; assert the other converges within N ticks AND
  the logs show **exactly one** propagation (no echo). A second propagation in the log
  is a ping-pong failure.

### Spike status vs production contract — `[OURS]`

The two R-LOOPBREAK tokens live on **opposite sides of the wall**, and the host spike
(`hosted/clipboard/`) implements only the host one:

- **Host token (host→AROS direction) — implemented in the spike.** `host_pb_set_text`
  records the `changeCount` its own write produced as a `_Atomic` "last self-write
  token" inside the shim. The poller suppresses the delta equal to that token, so the
  bridge never raises a spurious host→AROS change for its **own** host write. This is
  fully covered by the standalone proof's **[C-4]** sub-check (self-write suppressed,
  an external NSPasteboard write still fires) and is what the host shim owns.
- **AROS token (AROS→host direction) — production, AROS-side, not in the host spike.**
  The `CBD_CURRENTWRITEID` token (and the `chm_ClipID` compare in the `CBD_CHANGEHOOK`
  body) still guards the **other** direction: it drops the echo of the sync task's own
  AROS clip write. That token is AROS-side and lands with the sync task at graft; the
  host spike cannot exercise it (it has no `clipboard.device`).
- **Thread-safety follows the shared `host-wake` contract.** The cross-thread poller
  state (run flag, signal target, callback pointer, self-write token, last-seen count)
  is C11 `_Atomic` with acquire/release ordering — the same "a foreign host thread
  wakes an AROS task" pattern the hosted port is factoring into a shared primitive
  (referenced here as **`host-wake`** / the shared wake contract). The host poller
  invokes `PBSignalFn` (never `exec.Signal` directly) under that contract; whether the
  AROS-side body of that callback may run `Signal` straight from the host pthread, or
  must defer to a pollable flag/self-pipe, remains the **Cross-thread Signal**
  UNVERIFIED item below.

**R-SIGNAL (the wake mechanism).** `[OURS]` (H9/H10). Both directions converge on
"wake the one sync task." `CBD_CHANGEHOOK`'s hook and the host poller's callback both
end in `Signal(syncTask, 1L<<sigbit)`; the task `Wait()`s on that bit (plus, in
scheme B's host→AROS path, a timer bit if the poller is replaced by an in-AROS tick).
This is precisely the H9 Wait/Signal + H10 message-port plumbing already de-risked
(`hosted/signal.c`, `hosted/msgport.c`); nothing new in the exec layer — only the host
backend and the transcode are new (the design.md thesis).

## IFF FORM FTXT framing (a *caller* convention) — `[PUB]` + `[AROS]`

`clipboard.device` is a raw byte stream and **never sees IFF**; the FTXT structure is
imposed caller-side by `iffparse.library` writing *through* the device
(`workbench/libs/iffparse/clipboardfuncs.c` maps IFF stream ops →
`CMD_READ`/`CMD_WRITE`/`CMD_UPDATE` `[AROS]`). The framing is the public IFF-85 / FTXT
standard `[PUB]`: a `FORM` whose type is `FTXT`, containing one or more `CHRS` chunks
whose bytes are the text. The bridge produces/consumes it with the **exact stock
sequences** from `hostcb.c` `[AROS]` — adapt, do not re-derive:

- **Write (host→AROS), from `hostcb.c:150–185`:** `AllocIFF` →
  `iff->iff_Stream = (IPTR)OpenClipboard(PRIMARY_CLIP)` → `InitIFFasClip` →
  `OpenIFF(IFFF_WRITE)` → `PushChunk(ID_FTXT, ID_FORM, IFFSIZE_UNKNOWN)` →
  `PushChunk(0, ID_CHRS, IFFSIZE_UNKNOWN)` → `WriteChunkBytes(text, len)` →
  `PopChunk` ×2 → `CloseIFF` (which triggers the device `CMD_UPDATE` that commits) →
  `CloseClipboard`. `text` here is the **transcoded ISO-8859-1 bytes**, not UTF-8
  (see Transcode).
- **Read (AROS→host), from `hostcb.c:216–293`:** `AllocIFF` →
  `OpenClipboard(PRIMARY_CLIP)` → `InitIFFasClip` → `OpenIFF(IFFF_READ)` →
  `StopChunk(ID_FTXT, ID_CHRS)` → `ParseIFF(IFFPARSE_SCAN)` loop: on each
  `CurrentChunk` with `cn_Type==ID_FTXT && cn_ID==ID_CHRS`, grow the accumulator and
  `ReadChunkBytes(buf, cn_Size)`, concatenating until EOF. This loop already handles
  **multi-segment** clips (a clip split across many CHRS chunks / many device
  `CMD_WRITE`s), which is the natural large-clip case (> `WRITEBUFSIZE`=4096,
  `clipboard.c:39` `[AROS]`). The accumulated bytes are ISO-8859-1, transcoded to UTF-8
  before `host_pb_set_text`.
- **IDs:** `ID_FORM`, `ID_FTXT = MAKE_ID('F','T','X','T')`, `ID_CHRS =
  MAKE_ID('C','H','R','S')` — `[PUB]` IFF spec ids; in-tree real-writer cross-check at
  `workbench/classes/zune/texteditor/mcc/ClipboardServer.c:71` `[AROS]`.

**R-IFFHARDEN (truncation hardening + size cap).** `[REF-CONFIRM]`, restated
`[PUB]`+`[AROS]`. The GPL bridges learned the hard way that a truncated/garbled IFF
stream crashes a naive parser and that an unbounded initial copy is a hazard (hence
their explicit "validate truncated IFF" fix and a startup size cap). Restated from
independent footing: (1) the parser MUST tolerate a malformed/short clip — the
`hostcb.c` read loop already breaks on any `ParseIFF` error other than `IFFERR_EOC`
and on a failed `ReadChunkBytes`, yielding "no text" rather than a crash; the bridge
treats a non-FTXT or unparseable clip as a **skip/no-op** (C4 asserts this), per the
IFF spec's own framing rules `[PUB]`. (2) The bridge MUST impose a configurable
**size cap** (`PB_MAX_CLIP`, default e.g. 1 MiB for text) on both directions and
refuse/clip oversize transfers rather than allocate unboundedly — justified by ordinary
defensive bounds on attacker-influenced input `[OURS]`, not by any reference's value.

## Transcode: ISO-8859-1 ↔ UTF-8 (a REQUIREMENT) — `[REF-CONFIRM]`, restated

`[REF-CONFIRM]`+`[PUB]`. The CHRS payload of a classic FTXT clip is **single-byte
codepage** text — on the Amiga, ISO-8859-1 (Latin-1) is the de-facto encoding.
`NSPasteboardTypeString` is Unicode and the host ABI carries **UTF-8**. Passing bytes
through unchanged corrupts every non-ASCII character (e.g. `0xE9` "é" in Latin-1 is an
illegal/standalone byte in UTF-8; "€" has no Latin-1 byte at all). The only shipping
Amiga-like↔macOS clipboard tool transcodes for exactly this reason — independent
confirmation that this is **required behaviour, not a deferrable nicety**. Restated
requirement on independent footing:

- **Host→AROS:** UTF-8 from `host_pb_get_text` → **ISO-8859-1** before the FTXT write.
  Use POSIX `iconv` `[PUB]` `"UTF-8" → "ISO-8859-1"` (or `"ISO-8859-1//TRANSLIT"` to
  best-effort map characters with no Latin-1 equivalent, e.g. "€"→"EUR" or "?").
- **AROS→host:** the accumulated CHRS bytes (ISO-8859-1) → **UTF-8** before
  `host_pb_set_text`. `iconv` `[PUB]` `"ISO-8859-1" → "UTF-8"` (this direction is
  always lossless — every Latin-1 byte has a UTF-8 codepoint).
- **Where:** in the **bridge/sync layer** on the AROS side (it already holds both the
  CHRS bytes and the UTF-8 ABI strings), keeping `clipboard.device` and the host shim
  both encoding-agnostic. **UNVERIFIED** whether AROS's `iconv`-equivalent (its
  codesets / `locale.library` or a hosted libc `iconv`) is available in the hosted
  build; if not, a small **static** Latin-1↔UTF-8 table is sufficient and trivial to
  author from the published code-point mapping `[PUB]` (Latin-1 maps 1:1 to U+0000–
  U+00FF), and must be written from scratch, not lifted.
- **R-TRANSCODE acceptance (C3-nonascii):** round-trip a string containing `é ü ß ©`
  (host → AROS clip → host) and assert the final UTF-8 equals the original; and the
  reverse (AROS Latin-1 clip → host UTF-8 → AROS) byte-exact. A pass proves the
  transcode is wired both ways; a mojibake byte fails the exact-match.

## AROS-side binding — `[AROS]`, grounded in [design.md](design.md) + `hostcb.c`

For scheme B the AROS side is **stock AROS code**; the only new AROS code is the
sync-task policy (hook registration, signal handling, transcode call-out, host
call-out). Contracts (all cite `workbench/devs/clipboard/clipboard.c` +
`compiler/include/devices/clipboard.h`):

- **Device access.** `OpenClipboard(PRIMARY_CLIP)` (iffparse) returns a
  `ClipboardHandle` wrapping an `IOClipReq`; the bridge never builds the `IOClipReq`
  by hand — it drives the handle through iffparse exactly as `hostcb.c` does. The clip
  transaction model (`io_ClipID` token; a write = a sequence of `CMD_WRITE`s
  terminated by `CMD_UPDATE`; a read continues until EOF sets `io_ClipID = -1`) is the
  iffparse `ClipStreamHandler`'s job, not the bridge's (design.md "IFF FTXT framing").
- **Change hook.** Register/remove with `CBD_CHANGEHOOK` (`clipboard.c:367`). The hook
  struct's `h_Entry` is the AROS hook-calling convention `[AROS]`; its body reads
  `chm_ChangeCmd`/`chm_ClipID` from the `ClipHookMsg`, applies the R-LOOPBREAK AROS
  token check, and `Signal`s the sync task. Author the hook from the `struct Hook` /
  `ClipHookMsg` headers, not from any reference.
- **Write-id query.** `CBD_CURRENTWRITEID` (`clipboard.c`, id `CMD_NONSTD+2`) gives the
  AROS self-written token for R-LOOPBREAK. `CBD_CURRENTREADID` is available
  symmetrically if needed.
- **Scheme-A note (later).** A host-backed unit services `CMD_READ`/`CMD_WRITE`/
  `CMD_UPDATE` from a device task (the `hosted/device.c` H11 dispatcher widened to the
  `IOClipReq` command set), shipping accumulated bytes to `host_pb_set_text` on the
  `CMD_UPDATE` that commits and pulling `host_pb_get_text` into the clip buffer on a
  `CMD_READ` start. The IFF wrap/unwrap **still happens caller-side** in iffparse, so
  the unit sees only CHRS-payload bytes; the transcode still lives in the bridge layer.
  This is a re-wiring of the same primitives, deferred.

## Host side (`hosted/pasteboard.m`) — `[PUB]` Apple

- `host_pb_get_text` → `[[NSPasteboard generalPasteboard] stringForType:
  NSPasteboardTypeString]`; if non-nil, copy as malloc'd UTF-8 (`UTF8String` / explicit
  `NSUTF8StringEncoding`), set `*len`. Returns 0/`*out=NULL` when no string item.
- `host_pb_set_text` → `[pb clearContents]` then `[pb setString:s
  forType:NSPasteboardTypeString]` (or `writeObjects:@[nsstr]`); return the post-write
  `pb.changeCount` (the host self-written token).
- `host_pb_change_count` → `pb.changeCount` (NSInteger → host `long`). **UNVERIFIED**
  exact width marshalling through H3; treat as 64-bit `long` (design.md).
- `host_pb_poller_start/stop` → one detached `pthread` running the R-POLLER loop;
  `host_pb_set_signal_cb` stores the `PBSignalFn`. The shim links `Foundation`,
  `AppKit` (for `NSPasteboard`) and `objc`; built as `libpasteboard.dylib`, loaded via
  `hostlib.resource`. These are plain (non-variadic) C functions, reached with
  `HostLib_GetPointer` and called under `HostLib_Lock`/`Unlock` — they do **not** need
  the H3 variadic marshaller (`hosted/abishim.S`), which exists only for variadic host
  calls like `snprintf` (`hosted/abishim.c` `[OURS]`).
- (later) `host_pb_get_png`/`set_png` → `NSPasteboardTypePNG` (fall back to
  `NSPasteboardTypeTIFF` + ImageIO transcode via the H7 path if PNG absent).

**Non-blocking rule** `[OURS]`. Each text host call is one synchronous, fast
pasteboard touch — no Cocoa notifications, no `NSRunLoop`, nothing that would entangle
AROS's signal-driven scheduler (design.md). The only long-lived host object is the
poller pthread, which sleeps between cheap `changeCount` reads.

## Verification (unattended — `[OURS]` H1–H12 marker discipline)

No TCC anywhere: pasteboard access needs no entitlement, so the agent never hits an
approval dialog. The pasteboard itself is the oracle — ground-truth text crosses the
boundary and is asserted byte-equal on the far side, read back **through the host**
(`host_pb_get_text` / `pbpaste`), never off a screen. Each spike prints one unique
`[C#] …` line the harness greps for, with the expected payload (the `[H1]…[H12]`
discipline, NOTES.md).

- **[C0] Wiring + scheme decision.** Host-only: `host_pb_set_text("rt<C0>")` then
  `host_pb_get_text`; print `[C0] PB rt=<...> cc=<changeCount>`. **PASS** = round-trips
  host→host and `changeCount` advanced. Confirms NSPasteboard is reachable via
  `hostlib.resource` and fixes scheme B.
- **[C1] host→AROS text + R-PBTHREAD.** Host `host_pb_set_text("AROS<C1>")`; an AROS
  task does the `hostcb`-style FTXT **write** of `PRIMARY_CLIP` from the (transcoded)
  host string, then an independent reader prints `[C1] read=<...>`. Run the host calls
  **from a non-main host thread** to settle R-PBTHREAD; if that faults, re-run via the
  main-thread marshal and note the result. **PASS** = AROS clip equals `AROS<C1>`.
- **[C2] AROS→host text.** AROS writes a `FORM FTXT` clip (`"HELLO<C2>"`) via iffparse;
  host reads `host_pb_get_text` (≡ `pbpaste`); print `[C2] host=<...>`. **PASS** =
  pasteboard equals the AROS clip.
- **[C3] change-detection both ways + R-LOOPBREAK.** Start the poller and register the
  `CBD_CHANGEHOOK`. Mutate one side once; assert the other converges within N ticks
  **without an explicit kick**, and that the log shows **exactly one** propagation per
  change (no echo). Print `[C3] a2h=ok h2a=ok prop=1`. **PASS** = both directions
  auto-sync, ping-pong-free.
- **[C3-nonascii] R-TRANSCODE.** Round-trip `"café €<C3>"` host→AROS→host and the
  reverse; print `[C3x] utf8_rt=ok latin1_rt=ok`. **PASS** = both round-trips
  byte-exact after transcode (Latin-1 lossy chars handled per `//TRANSLIT`).
- **[C4] robustness / R-IFFHARDEN.** Empty clip; a large clip (> 4096, multi-CHRS /
  multi-`CMD_WRITE`); a non-FTXT / truncated clip (assert graceful **skip**, no crash);
  an oversize clip (> `PB_MAX_CLIP`, assert refused/capped). Print
  `[C4] big=ok empty=ok nonftxt=skipped cap=ok`.
- **[C5] images (later).** PNG via `NSPasteboardTypePNG` ↔ an AROS image clip
  (datatypes/ILBM — **UNVERIFIED** target format). Deferred; interface fixed above.

Clean exit / watchdog: PASS exits fast via the existing semihosting `SYS_EXIT` path; a
hung sync loop is reaped by the harness watchdog (NOTES.md). No spike waits on a human.

## Build / integration

- Shim `hosted/pasteboard.m` links `Foundation, AppKit, objc`; built as
  `libpasteboard.dylib`, codesigned ad-hoc (confirm vs. the existing `run.sh` signing
  path, **UNVERIFIED**), loaded via `hostlib.resource`.
- The C ABI header (`pasteboard.h`) is shared source, hand-written, no GPL provenance.
- The shim must not link or include AROS headers; the AROS side must not include
  Cocoa/Foundation headers. The C ABI is the only contact surface, and the AROS→host
  `Signal` crosses it only as the installed `PBSignalFn` pointer.
- The AROS sync logic builds against stock `iffparse.library` + `exec` + the
  `clipboard.device` headers; for the spikes it can live as a hosted spike beside
  `hosted/device.c` and reuse the H10 message-port / H9 Signal plumbing.

## Open questions / UNVERIFIED

- **R-PBTHREAD:** whether `NSPasteboard` `changeCount`/`stringForType:`/`setString:`
  are safe off the macOS main thread; if not, marshal the read/write (not the cheap
  `changeCount` poll) to the H4 boot-anchor task. **C1 settles this.**
- **Cross-thread Signal:** whether a host pthread (the poller) may invoke an AROS
  `Signal` through `PBSignalFn` under the H6 single-thread scheduler, or whether the
  poller must instead set a pollable flag / write a self-pipe the AROS sync task reads
  on its own tick. Fallback specified; **UNVERIFIED** which is needed.
- **Timer source (scheme-B host→AROS, if the poller is moved in-AROS):** which periodic
  wake the hosted port exposes (H6 SIGALRM tick vs. a `timer.device`) is **UNVERIFIED**.
- **`iconv` availability** in the hosted AROS build vs. the static Latin-1↔UTF-8 table
  fallback; and the `//TRANSLIT` policy for characters with no Latin-1 form.
- **`changeCount` width** marshalling through H3 (treated as 64-bit `long`).
- **AROS UTF-8 consumers:** whether any AROS text consumer expects UTF-8 (not Latin-1)
  in CHRS — if AROS apps standardise on UTF-8, the transcode target may become a config
  choice rather than a fixed ISO-8859-1. **UNVERIFIED.**
- **Image-clip format** (C5): `NSPasteboardTypePNG`/`TIFF` ↔ AROS image-clip
  representation (ILBM? a datatype?) is unresolved.

## Provenance summary

`[PUB]` IFF-85 / FORM FTXT / CHRS framing; Apple `NSPasteboard` general-pasteboard +
`changeCount` + `NSPasteboardTypeString`/`PNG`; POSIX `iconv`. ·
`[AROS]` `compiler/include/devices/clipboard.h` (`IOClipReq`, `ClipHookMsg`,
`CBD_CHANGEHOOK`/`CBD_CURRENTWRITEID`, `PRIMARY_CLIP`),
`workbench/devs/clipboard/clipboard.c` (commands, hooks, `WRITEBUFSIZE`),
`workbench/libs/iffparse/{openclipboard,clipboardfuncs}.c`,
`developer/debug/test/misc/hostcb.c` (FTXT wrap/unwrap shape — adapted, X11→NSPasteboard),
`arch/all-hosted/hidd/x11/x11_clipboard.c` (Signal-on-change + ping-pong precedent),
`workbench/classes/zune/texteditor/mcc/ClipboardServer.c` (FTXT ids). ·
`[OURS]` `hosted/device.c` (H11 device dispatcher, scheme-A base), `hosted/msgport.c`
(H10 ports), `hosted/signal.c` (H9 Wait/Signal), `hosted/abishim.{S,c}` (H3 host-call
boundary), `hosted/exec.c` (H4 boot-anchor/main-thread model), `hosted/display.c` (H7
host-shim peer + ImageIO), NOTES.md (marker discipline). ·
`[REF-CONFIRM]` WinUAE/FS-UAE virtual `clipboard.cpp` confirmed IFF-FTXT host sync +
the truncation-hardening / size-cap edge cases (R-IFFHARDEN) — restated from the IFF
spec + defensive-bounds reasoning; Amiberry `host-clip` confirmed the
ISO-8859-1↔host `iconv` transcode is a real requirement (R-TRANSCODE) — restated from
the encoding facts + POSIX `iconv`; both bridges confirmed two-way change-sync
ping-pongs without a self-written guard (R-LOOPBREAK) — restated as an independent
token invariant grounded in `changeCount` `[PUB]` + `CBD_CURRENTWRITEID` `[AROS]`.
Implement every `[REF-CONFIRM]` item from its independent justification, never from a
reference.
