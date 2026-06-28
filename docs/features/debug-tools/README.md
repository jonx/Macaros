# Debug & bring-up tools

Small diagnostics built during the Wanderer bring-up. They exist to answer one
question that cost us days: **`OpenLibrary()` returned `NULL` — why?** Out of the
box that's a black box; these turn it into a readable, step-by-step picture.

| Tool | Answers | Source | Default |
|------|---------|--------|---------|
| `C:TestLib <lib> [ver]` | "Does `OpenLibrary` *actually* succeed?" | `workbench/c/TestLib.c` | always on (a command) |
| `C:LoadMatrix` | "Does the known desktop load chain open?" | `workbench/c/LoadMatrix.c` | always on (a command) |
| `graft/deploy-check` | "Am I running the files I just built?" | `graft/deploy-check` | host-side command |
| lddemon loader trace | "*Why* did that disk-library load fail?" | `rom/lddemon/lddemon.c` | **OFF** — flip `__lddemon_trace` + rebuild |
| `C:GrabScreen` | "Dump the live framebuffer to a file" | `workbench/c/GrabScreen.c` | always on (a command) |

---

## 1. `TestLib` — the load-tester

```
TestLib <library> [VER <n>]
```

Runs `OpenLibrary(name, version)` — which actually **loads and initialises** the
library through Exec and the on-demand loader — then reports the outcome:

```
1> TestLib stdc.library
OK: "stdc.library" opened -- version 0.47

1> TestLib muiscreen.library
FAIL: OpenLibrary("muiscreen.library", 0) returned NULL.
  The file may exist (try `Version`) but its init / resident
  registration / a dependency failed. Check the lddemon trace.
```

**Why it exists.** `Version` only *reads the file off disk* — it says nothing about
whether the library will open. The whole stdc saga was "`Version` sees the file, but
`OpenLibrary` still fails." Those are different facts — *file present* vs *library
opens* — and nothing distinguished them. `TestLib` does, in one command.

Returns `RETURN_OK` on success, `RETURN_WARN` on a NULL open, `RETURN_FAIL` on bad
args — so it also works as a pass/fail step in the unattended harness.

Build: `make workbench-c-testlib` → `AROS/C/TestLib`.

---

## 2. `LoadMatrix` — desktop load smoke test

```
LoadMatrix
```

Runs a fixed list of `OpenLibrary()` calls for the desktop stack and reports one
OK/FAIL table. It is intentionally boring: it does not discover dependencies, it
just answers "is the set we currently need for Wanderer present and openable?"

The current matrix covers `stdc.library`, `posixc.library`, `png.library`,
`datatypes.library`, `icon.library`, `intuition.library`, `graphics.library`,
`layers.library`, `asl.library`, `cybergraphics.library`, `muimaster.library`,
`muiscreen.library`, `picture.datatype`, and `png.datatype`.

Run it inside AROS before chasing a Wanderer crash. If `LoadMatrix` fails, fix
deployment/loading first. If it passes and Wanderer still dies, the problem is
past the loader.

Build: `make workbench-c-loadmatrix` → `AROS/C/LoadMatrix`.

---

## 3. `deploy-check` — stale deployment guard

```
./graft/deploy-check
```

Runs on the Mac side. It prints the discovered boot tree and hashes/timestamps
for the files that most often go stale:

- `build/cocoametal.dylib` vs `~/lib/cocoametal.dylib`
- `build/libpasteboard.dylib` vs `~/lib/libpasteboard.dylib`
- `hosted/cocoametal/settings.json` vs `~/lib/settings.json`
- `Storage/Monitors/Cocoa` vs `Devs/Monitors/Cocoa`
- bootstrap, `emul-handler`, key libraries, `TestLib`, `LoadMatrix`, Wanderer
- presence of the fuller desktop payload: `Prefs/Zune`, `Prefs/Wanderer`,
  `System/About`, `Tools`, `Utilities`

Interpretation:

- `OK` on dylibs/schema/monitor means the launch path is using the current copy.
- `WARN` on `Cocoa deployed` means `Devs/Monitors/Cocoa` is stale relative to
  `Storage/Monitors/Cocoa`. Launchers copy it at startup, but sync it manually if
  you are testing without a fresh launch.
- `MISS` in desktop presence means the fuller desktop set has not been
  built/deployed yet. It is not a loader failure by itself.

---

## 4. lddemon loader trace

`lddemon.resource` is AROS's on-demand loader for **disk** libraries and devices
(the ones in `LIBS:` / `Classes/`, not the kickstart-resident ones). When
`OpenLibrary` can't find a library already in memory, it asks lddemon to `LoadSeg`
the file and `InitResident` it. The trace logs every step of that decision.

### Turning it on / off

It is **OFF by default** (no boot noise). To enable it while debugging a library
load, flip the switch and rebuild:

    /* rom/lddemon/lddemon.c */
    volatile int __lddemon_trace = 1;    /* was 0 */

    make kernel-lddemon                  # rebuild lddemon.resource, then reboot

It is a **build-time** switch on purpose. The tempting "read `ENV:LDDEMON_TRACE` at
runtime" approach does DOS-packet I/O *inside the loader*, which runs in arbitrary
task contexts (including `ExecuteStartup`) and trips an "unexpected DOS packet"
alert. A safe runtime toggle (a `C:` command poking a flag in the lddemon resource
base — no packet I/O) is a clean follow-up if you want one.

- It only affects **disk** loads. Resident (ROM/kickstart) libraries don't go
  through lddemon.

The `[LDDiag]` lines go to the AROS debug log — on the hosted darwin port that's the
boot log (e.g. `/tmp/aros-window.log`).

### Reading the output

```
[LDDiag] LDRequestObject lib=muiscreen.library stripped=muiscreen.library version=0 dir=libs
[LDDiag] FindName(muiscreen.library) before load -> 0x00000000     <- not already in memory
[LDDiag] LDLoad caller=WANDERER:Wanderer name=muiscreen.library basedir=libs
[LDDiag] LDLoad result name=muiscreen.library seglist=0x00000000   <- file did not load
[LDDiag] CallLDInit(muiscreen.library) -> 0x00000000               <- so nothing to init
[LDDiag] loader returned raw=0x00000000 for muiscreen.library      <- OpenLibrary => NULL
```

The two values that matter:

- **`LDLoad result ... seglist=0`** → the file was not found / did not `LoadSeg`
  (wrong path, missing from the image, or a load error). *Deployment problem.*
- **`LDInit after InitResident ... node=0`** → the file loaded but its resident did
  **not** register (init returned NULL — a failed dependency or init). *Runtime
  problem.* You'll also see `LDInit resident candidate ...` with the type/version it
  found, and `LDInit no resident found ...` if there's no resident tag at all.

Build after editing: `make kernel-lddemon` in the build root; the trace lives in
`lddemon.resource`, so redeploy by booting that image.

---

## 5. Capture / screenshot diagnostics

Three ways to capture what AROS is drawing — useful for unattended PASS/FAIL and for
bug reports:

- **`C:GrabScreen <file>`** — an AROS-side command that reads the front screen's
  bitmap directly and writes a PPM. Always correct (reads AROS memory), no host TCC.
- **`aros-ctl shot [PATH]`** — the harness wrapper: runs `GrabScreen`, converts to
  PNG via `sips`. See [control-harness](../control-harness/README.md).
- **In-app "Take Screenshot" (⌘3)** — the Daedalos menu; writes
  `~/Desktop/AROS-screenshot.png` via the host `cm_capture_png` → `cm_readback`
  path. (Captures the Metal framebuffer as **opaque** — AROS leaves the alpha byte
  at 0, so this path must ignore alpha or the desktop saves out transparent/white.)

---

## How they pair

`TestLib` **triggers** a load on demand; the lddemon trace shows the loader's
**decision** for that load. Run them together:

1. Set `volatile int __lddemon_trace = 1` in `rom/lddemon/lddemon.c`.
2. Rebuild `lddemon.resource` with `make kernel-lddemon`, then reboot that image.
3. Run `TestLib png.datatype VER 41`.

`TestLib` says OK/FAIL; the `[LDDiag]` lines say whether the file loaded
(`seglist`) and whether its resident registered (`node`) — i.e. *deployment* vs
*runtime* failure. That distinction is the whole point.

## Standard Bring-up Loop

Use this order when resuming desktop work:

1. Build only the touched target.
2. Run `./graft/deploy-check`. Fix stale dylibs/schema/monitor before launching.
3. Boot with `AROS_CTL_STARTUP_MODE=desktop ./graft/run-window.sh`, or use
   `AROS_CTL_STARTUP_MODE=desktop ./graft/aros-ctl run` when you need screenshots
   and input automation.
4. Run `LoadMatrix` inside AROS. Loader failures come before Wanderer debugging.
5. Verify visually with `graft/aros-ctl shot /tmp/aros.png`.
6. If it crashes, use the backtrace module/function first. Only reopen the old
   relbase theory if the trace names base-getter or `x16`/`x17` handoff code.
