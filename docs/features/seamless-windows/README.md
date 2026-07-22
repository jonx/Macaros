# Seamless windows: AROS programs as first-class Mac citizens

**Status: feasibility study (2026-07-14). No code yet.** Grounded against both trees
(this repo + `../aros-upstream`); every contract cited points at a real file.

The goal, in one sentence: each AROS application window becomes its **own native
macOS window** (movable, minimizable, in the Window menu, mixing freely with Mac
apps), while all of them remain windows of the **one running AROS**: same memory,
same exec, same libraries. And the AROS filesystem shows up as a **drive in
Finder**. AROS programs get to be first-class citizens on the Mac while keeping
their own identity. The classic name for the display half is a **rootless** mode
(X11.app on the Mac, "seamless"/"coherence" in VM products).

Verdict up front: **feasible, and cheaper than it sounds**, because three of the
four hard pieces already exist in some form. The genuinely new work is one graft
seam in Intuition/layers and a per-window generalization of the host display shim.
The filesystem half is mostly already true today and needs presentation plus one
bridge for `RAM:`.

## 1. Ground truth (what the trees actually say)

**Host display side** ([hosted/cocoametal/](../../../hosted/cocoametal/)): the shim
is hard-wired to ONE window. A single `CMContext` owns the NSWindow, the
`CAMetalLayer` and the framebuffer texture ring (`cocoametal.m:144`). The `cm_*`
ABI ([cocoametal.h](../../../hosted/cocoametal/cocoametal.h)) is append-only, so we
can add a surface family without breaking anything. Two head-starts are already
in place:

- Input is drained centrally on the main thread and **already routes by
  NSWindow**: events over the Settings panel are filtered out by comparing
  `ev.window` (`cocoametal_window.m:745`). Per-window routing is a
  generalization, not a rewrite.
- The threading rule (AppKit/Metal on the main thread only, AROS calls hop via
  `cm__sync_main` with signals blocked, `cocoametal.m:74`) **scales unchanged to
  N windows**: more NSWindows is just more objects on the same one thread.

**AROS display side** (`../aros-upstream/arch/all-darwin/hidd/cocoa/`): the gfx
HIDD keeps a single global context (`cocoa_intern.h`, "one Cocoa display in the
system") and shows one BGRA8 framebuffer. Every hosted AROS driver in the tree
(X11, SDL, GDI, Cocoa) is **one host window per AROS *screen***; nothing is
per-Intuition-window. There is no rootless prior art in the tree.

**How Intuition windows render** (`../aros-upstream`): a window is **not a
surface**. It is a `struct Layer` (`compiler/include/graphics/clip.h:32`), a
clipped region drawing straight into the **screen's shared bitmap**
(`rom/intuition/openwindow.c:1565`: `w->RPort = w->WLayer->rp`). The only
per-window pixels that exist are SMART_REFRESH's obscured-cliprect backups and
SUPER_BITMAP windows. The in-tree Compositor
(`workbench/devs/monitors/Compositor/`) composites **screens**, not windows.
Consequence: a per-window host surface must be *derived* (a slice of the screen
bitmap) or *created* (promote windows to backing bitmaps). See §2.

**The interception seam is small.** Every window create/destroy/move/size/depth
change funnels into five hyperlayers entry points:
`CreateUpfrontLayer*` (from `rom/intuition/openwindow.c:1373`), `DeleteLayer`,
`MoveSizeLayer` (single choke point `DoMoveSizeWindow`,
`rom/intuition/inputhandler_actions.c:406`), and
`UpfrontLayer`/`BehindLayer`/`MoveLayerInFrontOf` (from
`windowtofront.c`/`windowtoback.c`/`movewindowinfrontof.c`). Each carries
`layer->Window` back to the Intuition window. Intuition even documents a per-port
driver customization point (`openwindow.c:647`, `intui_OpenWindow`). Hooking
these five gives the host a complete, low-noise window lifecycle feed.

**Input**: host events already flow per-window-capable. The AROS poll task pulls
neutral `CMEvent`s (`cocoa_input.c`) and injects RAWKEY/RAWMOUSE; Intuition's
input handler hit-tests in **global screen coordinates** and picks the target
window itself (`rom/intuition/inputhandler.c`). So per-window input just means
translating each host window's local coordinates by that window's AROS position
before injection. The position map is the load-bearing piece; there is no
"inject into window X" API to bypass hit-testing.

**Filesystem**, the punchline: **most of the AROS filesystem is already host
storage.** `SYS:` is the boot directory on the Mac disk served by `emul-handler`
(no disk image, no partition; `arch/all-hosted/filesys/emul_handler/`), and
`MacRO:`/`MacRW:` map a host folder, default `~/AROS/Shared`
([host-volume](../host-volume/README.md), `graft/run-window.sh:339`).
`emul-handler` translates each DOS operation into host libc calls on that real
directory, so the two-way traffic already works: a file dropped into the folder
from Finder appears in AROS on the next directory scan, and a file saved from
AROS is immediately a normal Mac file. What Finder lacks is *volume identity*
(these are plain folders, not drives; `.amimeta` metadata sidecars are opaque
clutter; Latin-1/NFC name translation applies). The only host-invisible
storage is **`RAM:`** (in-memory `rom/filesys/ram/`) and what's assigned onto it
(`T:`, `ENV:`, `RAM:Clipboards`). No FUSE/FSKit/NFS/SMB serving code exists in
either tree; that half starts from a clean slate.

**Alignment with the project direction**: this is exactly the
[libaros](../../../hosted/libaros/IDEAS.md) framing, **one engine per process,
many presentation surfaces** (`libaros.h:261`). Seamless mode is "many surfaces"
taken to its logical end (one surface per Intuition window), not a new
architecture. The user-visible constraint holds by construction: every window is
the same OS instance, shared memory and all.

## 2. Recommended architecture: composite-slice rootless

Keep the single logical AROS screen, its shared bitmap, and the whole existing
render path **unchanged**. Add, on top:

1. **A window lifecycle feed.** Hook the five hyperlayers seams (§1) in a small
   graft (guarded, darwin-only) that publishes `{open, close, move/size, depth}`
   events with `layer->Window` identity to the Cocoa HIDD.
2. **N host surfaces.** New append-only shim ABI: `cm_win_open(id, x, y, w, h,
   flags)`, `cm_win_upload(id, rect, pixels, stride)`, `cm_win_present(id)`,
   `cm_win_set(id, geometry/z/title)`, `cm_win_close(id)`, plus a window-id-aware
   event pump (`CMEvent` has no id field and the ABI is append-only, so this is a
   new event struct + new pump entry, not a field change). The AROS side drops
   the single-`ctx` assumption for a window→surface map.
3. **Slice uploads.** On damage, intersect the damaged screen-bitmap rect with
   each mapped window's bounds and `cm_win_upload` only the affected surfaces
   (stride = screen bitmap pitch, offset = window origin). Total uploaded bytes
   stay ≈ screen area, same as today. The existing full-frame texture-ring
   assumption (`cocoametal.m:188`) becomes per-surface.
4. **Per-window input.** Each NSWindow feeds the shared event ring tagged with
   its id; the AROS poll task adds that window's AROS origin and injects as
   today. Host focus changes inject `ActivateWindow`; the host close button
   becomes `IDCMP_CLOSEWINDOW` for that window (same contract as the current
   `CM_EV_CLOSE`).

**Why slices are correct (mirror mode).** The first milestone keeps host window
positions and z-order in **lockstep** with the AROS screen (AROS screen sized to
the Mac desktop; host drag feeds back `MoveWindow`, host raise feeds
`WindowToFront`). Then any pixels a window loses to an overlapping AROS window
are exactly hidden behind that same window's host NSWindow: opaque rectangles +
identical stacking = visually correct, with only transient artifacts during a
drag. No per-window backing store needed.

**The arena variant (free placement, later).** Lockstep breaks the moment the
user wants Spaces, multiple monitors, or host-only placement. The fix is to
decouple the two coordinate spaces: lay windows out **non-overlapping** on an
oversized AROS screen (a layout arena), so every window's framebuffer slice is
always fully visible, while host positions are free. Costs: arena bitmap memory
(8192×8192 BGRA = 256 MB; 4096×4096 = 64 MB with slot reuse; needs a sizing
policy), and apps that read their own window coordinates see arena coordinates.
Mirror mode first, arena as milestone 3.

**Identity (decoration).** Two options, both cheap once surfaces exist: (a)
borderless NSWindows carrying the AROS decoration pixels (the window slice
includes borders; drag/close/depth gadgets already work since input round-trips),
maximum identity; (b) host titlebar + hidden AROS border, maximum nativeness.
Ship (a) as default with (b) as a setting; the slice rect just includes or
excludes the border area.

**What stays out of seamless mode.** The Wanderer backdrop window is suppressed
(no host window for backdrop layers); desktop icons are lost in seamless mode
initially (open question: a host desktop overlay later). Non-Workbench screens
(games, custom modes) keep today's single-window display; seamless applies per
screen, and the front-screen switch toggles between the two presentations.

**Menus are the hardest UX piece.** Intuition menus draw onto the *screen*
bitmap, which no longer has a host window. Two routes: (a) detect menu mode in
the input handler and show the menu strip region in a transient borderless
overlay NSWindow (keeps all behavior, ugly but correct, a good milestone); (b)
map the window's `struct Menu` tree (readable after `SetMenuStrip`) onto the
real macOS menu bar and synthesize `IDCMP_MENUPICK` on selection, the true
first-class answer and a separate design of its own. Do (a) first.

### Rejected alternative: per-window backing bitmaps

Promote every window to its own HIDD bitmap (SUPER_BITMAP-like,
`rom/hyperlayers/dohookcliprects.c:171`) and extend the Compositor's
`StackBitMapNode` stack (`compositor_intern.h:16`) from screens to windows.
Strictly more capable (true per-window damage, no arena), but it rewires
`layers` + `graphics` + Compositor, fights apps that choose their own refresh
mode, and diverges hard from upstream. The slice design gets the same
user-visible result for a fraction of the risk; revisit only if slice
performance disappoints.

Portability framing: neither design is darwin-only, but they differ in blast
radius. The slice design's OS-side change is five host-agnostic notification
hooks (shapeable as a generic window-notification driver API any hosted port
could consume), with all platform code in the darwin HIDD + shim; rendering
semantics and app compatibility are untouched. Per-window backing changes
shared core code that every port (hosted and native) executes, and changes
app-visible semantics (e.g. SIMPLE_REFRESH windows gaining a backing store),
so every port inherits the regression risk.

## 3. The filesystem half: AROS volumes in Finder

Three tiers, independently shippable:

- **FS-A · Present what already exists.** `SYS:` and `MacRW:` are real host
  directories today. Give them Finder presence: volume icon + sidebar/Location
  for the boot dir and the shared folder, surfaced from the Macaros shell (the
  File menu already has "Open Folder as Volume"; this is its mirror image). A
  day of work, ninety percent of the perceived feature. Caveat to document:
  AROS-side metadata (protection bits, comments) lives in `.amimeta` sidecars
  (`emul_meta.c`) and filenames are NFC-normalized Latin-1↔UTF-8
  (`emul_norm.c`, `emul_charset.c`), so Finder sees sidecar files.
- **FS-B · A generic AROS-volume bridge.** A small AROS daemon that is a plain
  DOS client (`Lock`/`Examine`/`ExNext`/`Open`/`Read`/`Write`, which works
  against *any* handler, `RAM:` included) serving a simple file protocol over a
  second FIFO, exactly the [control-harness](../control-harness/README.md)
  transport shape, with a host CLI (`aros-fs ls RAM:`) as the unattended-loop
  proof.
- **FS-C · A real mount.** Present FS-B through a macOS **FSKit** file-system
  extension (macOS 15+; API details UNVERIFIED, needs a spike) so `RAM:`, or a
  virtual root showing every AROS volume, mounts as an actual drive. Fallback
  if FSKit disappoints: a localhost NFSv3 or WebDAV server over the same bridge
  (no kext; macFUSE is ruled out: kext/system-extension pain). FS-C is the only
  genuinely uncertain piece in this study.

## 4. First-class extras (natural follow-ups, not in scope)

- **Finder launch**: associate AROS executables/projects with Macaros; an
  open-document event → control FIFO → `SystemTags`/WBRun launch in the running
  AROS. Cheap and high-payoff once FS-A exists.
- **Dock**: all seamless windows belong to the Macaros app (correct Window menu,
  cycling, Mission Control for free). True per-program Dock icons would need
  lightweight proxy apps owning their windows; open question, far later.
- **Drag & drop** host↔AROS files onto windows: its own feature, needs FS-A.

## 5. Risks and open questions

- **Coordinate authority drift** (mirror mode): host drag vs AROS `MoveWindow`
  racing. Mitigation: AROS stays authoritative; host drags are *requests*
  (exactly how host resize already works, `CM_EV_RESIZE`).
- **Damage granularity**: the deployed driver currently pushes whole frames even
  though `UpdateRect` is rect-shaped. Per-window slices make real dirty-rect
  routing matter; needs the intersect step in §2.3, else N windows × full frame.
- **Menus, pointer, screen-depth gadgets**: menu plan in §2; whether the AROS
  pointer is host-cursor or drawn into the framebuffer needs checking in
  `cocoa_input.c`/the HIDD before SW1.
- **Apps reading coordinates** see arena coordinates in arena mode (SW3 only).
- **Upstream divergence**: the hyperlayers hooks are a graft; keep them in the
  darwin arch dir behind the documented `intui_OpenWindow` seam where possible.
- **Harness compatibility**: the offscreen readback oracle reads the *screen*
  bitmap, which still exists and stays authoritative, so `aros-ctl` capture
  keeps working unchanged through all milestones. Per-window readback
  (`cm_win_readback`) is worth adding for verification anyway.

## 6. Spike plan (greppable markers, unattended-loop verifiable)

| Marker | Prove | PASS looks like |
|---|---|---|
| `[SW0]` | Shim only: N `CMContext`s behind `cm_win_*`; two host windows showing two slices of one test buffer | per-surface readback matches expected pixels, no window server needed |
| `[SW1]` | The graft: hyperlayers feed → host windows mirror Wanderer's windows in lockstep (pos/size/z); input round-trips | `aros-ctl` opens a shell window, types into it via the *per-window* path, screen oracle shows the text |
| `[SW2]` | Identity: borderless + AROS decor, close/focus/minimize sync, backdrop suppressed | close button on the host window closes the AROS window (oracle diff) |
| `[SW3]` | Arena mode: free host placement, second monitor | window dragged host-side keeps rendering; AROS-side coords stable |
| `[SW4]` | Menus route (a): overlay window during menu mode | menu pick via injected input still lands (`IDCMP_MENUPICK` observed) |
| `[FS0]` | Finder presence for boot dir + shared folder (icons, sidebar) | manual check only (Finder is not scriptable in the loop; document it) |
| `[FS1]` | DOS-client file daemon over FIFO + `aros-fs` CLI | `aros-fs ls RAM:` lists a file created via `aros-ctl type` |
| `[FS2]` | FSKit (or NFS fallback) mount of the FS1 bridge | `ls /Volumes/AROS-RAM/` from plain zsh shows the same file |

Rough effort: SW0 days · SW1 the real graft work (the hard one) · SW2 days ·
SW3/SW4 each a design-plus-week · FS0 a day · FS1 days · FS2 uncertain (spike
first). SW0+SW1+SW2+FS0 alone already deliver the "wow": AROS windows living on
the Mac desktop next to Safari, backed by one shared AROS.
