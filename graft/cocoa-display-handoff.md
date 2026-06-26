# Cocoa/Metal display driver — "shell renders, window stays empty" (handoff)

**Status:** AROS boots to a fully usable shell, but the Mac window shows an empty
gray screen. The crash bugs are all fixed (window opens, mouse + keyboard reach
the OS with no crashes, file-I/O fixed, close→quit). This doc is the one
remaining issue: **getting what AROS actually renders to appear in the window.**

You cracked the keyboard fault line fast last time — this is the same flavor of
problem (a contract mismatch between the hosted driver and the AROS core), so I'm
handing it over with the exact evidence rather than more of my guesses.

---

## 1. Setup (one paragraph)

darwin-aarch64 hosted AROS, **threaded "inversion" mode** (`AROS_DARWIN_THREADED=1`):
AROS runs on a dedicated pthread; Cocoa/Metal + CFRunLoop own the main thread;
scheduler signals are masked off every thread except AROS's. The display is a
runtime-built gfx HIDD in `arch/all-darwin/hidd/cocoa/`:

- **CocoaGfx** (subclass of `CLID_Hidd_Gfx`) — `Show()` opens the host window
  lazily and records the front bitmap.
- **CocoaBM** (subclass of `CLID_Hidd_ChunkyBM`) — owns a BGRA8 chunky
  framebuffer; overrides **`UpdateRect()`** to push the dirty rect to the window
  via `cm_upload_rect` + `cm_present`.
- It drives `cocoametal.dylib` over a flat-C `cm_*` ABI (`cm_open` / `cm_upload_rect`
  / `cm_present` / `cm_readback` / …). Every `cm_*` call runs under
  `Forbid()` + `HostLib_Lock()` because it hops to the main thread and blocks the
  AROS thread in a host syscall.

The intended contract: AROS draws into the CocoaBM chunky buffer → the gfx layer
calls `UpdateRect` → we upload + present → it shows in the window.

---

## 2. Symptom

The NSWindow (title bar **"AROS"** — that's the `cm_open` name arg) shows a flat
gray screen whose *rendered* (in-framebuffer) title bar reads **"Workbench
Screen"**. It never changes. No crash. Meanwhile the OS is fully up with a live
shell prompt.

---

## 3. Proof AROS *is* rendering (so this is a display-path bug, not a boot bug)

`C:GrabScreen` dumps `LockPubScreen(NULL)->RastPort.BitMap` via `ReadPixelArray`.
Its output (`graft/cocoa-display-shell-proof.png`) is a **fully rendered shell**:
blue screen, AROS banner, **Kickstart 51.51**, a live `1>` prompt. So AROS
renders the shell into *a* bitmap. The window just isn't showing *that* bitmap.

---

## 4. Hard evidence (instrumented, one boot, startup-sequence = `Version`)

```
[Cocoa] CocoaGfx::Show(0x0000000106a63f38)      <- 1 Show, ever
[Cocoa] cm_open(800,600) -> 0x...               <- 1 window, ever
   Show calls:      1
   cm_open calls:   1
   UpdateRect logs: 0      <-- UpdateRect is NEVER called on the CocoaBM
   first present:   0      <-- so cm_present (via UpdateRect) never fires
```

And a poll-task walk of `IntuitionBase->FirstScreen` (single screen present):

```
[Cocoa:Input] screen 'Workbench Screen' bm=0x0 (shown=0x106a63f38)
```

i.e. `HIDD_BM_OBJ(scr->RastPort.BitMap)` resolved to **NULL** for the listed
screen, while the bitmap intuition `Show()`'d the driver (`xsd.visible =
0x106a63f38`) is a valid, different object. (Caveat: `HIDD_BM_OBJ` is the
`Planes[0]` cast trick from `rom/intuition` + hostgl; the NULL may mean the macro
is wrong for this bitmap rather than the obj truly being NULL — don't over-trust
that one line. The other findings stand on their own.)

---

## 5. Diagnosis — two intertwined problems

**(A) `UpdateRect` is never invoked.** After the single `Show` + initial present,
the gfx layer never calls `CocoaBM::UpdateRect` again — so the present pipeline is
dead and the window is frozen on the first (gray) frame. AROS is clearly drawing
(GrabScreen proves it), but never through the path that would flush the CocoaBM to
the host.

**(B) The Show'd bitmap is not the bitmap the shell renders to.** GrabScreen
(`LockPubScreen(NULL)->RastPort.BitMap`) = the **blue shell**. The cocoa shows
`xsd.visible` (the `Show`'d bitmap) = **gray**. They differ. Most likely there
are **two screens**: an empty front screen (gray, titled "Workbench Screen",
the one `Show`'d to us) sitting in front of the screen the shell actually lives on
(blue, which `LockPubScreen` returns). The user sees the empty front one.

So even if I fix (A) and present `xsd.visible` every frame, I'd be presenting the
*wrong, empty* screen. Confirmed: my periodic-present experiments (below) all
showed gray.

---

## 6. What I already tried (all show gray / miss)

1. **Periodic full-frame present of `xsd.visible`** from the poll task (every 3rd
   tick), bypassing the dead `UpdateRect`. → gray (it's the empty front screen).
2. **Present `LockPubScreen(NULL)`'s bitmap** instead. → gray too (by the time
   the poll task runs, the default-public screen is *also* the empty one; the
   blue shell screen GrabScreen caught early is no longer what `LockPubScreen`
   returns — the default-public assignment changes during boot).
3. **Walk `FirstScreen` and present the screen whose bitmap ≠ `xsd.visible`.** →
   `HIDD_BM_OBJ` returned NULL; one boot even crashed. Reverted.

All three are reverted; tree is back to the last committed state plus the
close→quit fix (`f0047b7`).

---

## 7. The questions to answer (this is where fresh eyes help)

1. **Why is `UpdateRect` never called on the displayable CocoaBM** while AROS is
   actively rendering? Is intuition rendering to a *different* bitmap (a RAM
   screen bitmap, a back buffer, a compositor target) and never blitting to /
   flushing the displayable? (Note: the AROS software compositor does **not**
   appear active — 0 "composit" hits in the boot log, and CocoaGfx advertises no
   composition / `NoFrameBuffer` flags. So either it should be, or the
   direct-render path is being used but bypasses our bitmap.)
2. **Why are there two screens, and why is the shell on the back one?** Is the
   gray front screen the Workbench screen (opened empty because Wanderer isn't
   drawing), with the boot console/shell left behind it? Should the boot leave
   the console in front, or should the shell run as a window on the front screen?
3. **What is the correct contract** for this driver to present "whatever AROS is
   currently displaying"? Rely on `Show` + `UpdateRect` (and figure out why
   UpdateRect is silent), or have the driver follow the frontmost screen's actual
   render bitmap?

A good discriminating experiment: log, at several times post-boot, **every**
screen (`FirstScreen`→`NextScreen`) with its title, `RastPort.BitMap` pointer,
and the displayable HIDD object behind it, alongside `xsd.visible` — to nail down
how many screens exist, which one the shell draws into, and whether that bitmap is
ever the one we were `Show`'d.

---

## 8. Files

| File | Role |
|---|---|
| `arch/all-darwin/hidd/cocoa/cocoagfx_hiddclass.c` | `CocoaGfx::Show` — sets `xsd.visible`, lazy `cm_open`, initial present (~line 161) |
| `arch/all-darwin/hidd/cocoa/cocoagfx_bitmapclass.c` | `CocoaBM::UpdateRect` — the present hook that never fires (~line 27) |
| `arch/all-darwin/hidd/cocoa/cocoa_intern.h` | `struct cocoahidd xsd` — `visible`, `ctx`, `cm` |
| `arch/all-darwin/hidd/cocoa/cocoa_input.c` | poll task (kbd/mouse); where I prototyped the periodic-present experiments |
| `hosted/cocoametal/cocoametal.m` | `cm_open` / `cm_present` (fbTex→offTex + drawable) / `cm_readback` (reads offTex — the screenshot oracle) |
| `workbench/c/GrabScreen.c` | the *working* reference: `LockPubScreen(NULL)` + `ReadPixelArray` reaches the shell |

(`cocoagfx_hiddclass.c` and the cocoametal sources live in the two repos:
AROS core under `~/Source/aros-upstream`, the dylib under
`~/Source/aros-aarch64/hosted/cocoametal`.)

## 9. Repro

```sh
# windowed boot with a control FIFO:
~/Source/aros-aarch64/graft/aros-ctl run
~/Source/aros-aarch64/graft/aros-ctl shot /tmp/x.png   # cm_readback -> gray

# the shell IS there — dump the AROS framebuffer directly:
#   set startup-sequence to "Version\nC:GrabScreen SYS:gs.ppm", boot, convert gs.ppm
#   -> shows blue screen + Kickstart 51.51 + prompt
```
