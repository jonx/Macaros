# Debug & bring-up tools

Small diagnostics built during the Wanderer bring-up. They exist to answer one
question that cost us days: **`OpenLibrary()` returned `NULL` — why?** Out of the
box that's a black box; these turn it into a readable, step-by-step picture.

| Tool | Answers | Source | Default |
|------|---------|--------|---------|
| `C:TestLib <lib> [ver]` | "Does `OpenLibrary` *actually* succeed?" | `workbench/c/TestLib.c` | always on (a command) |
| `graft/dep-scan <path>` | "What disk objects does this artifact name before launch?" | `graft/dep-scan` | host-side command |
| `C:LoadMatrix` | "Does the known desktop load chain open?" | `workbench/c/LoadMatrix.c` | always on (a command) |
| `graft/aros-ctl deploy` | "Make the runnable image match current build outputs" | `graft/aros-ctl` | host-side command |
| `graft/deploy-check` | "Am I running the files I just built?" | `graft/deploy-check` | host-side command |
| `graft/desktop-smoke` | "Does the desktop boot and screenshot cleanly?" | `graft/desktop-smoke` | host-side command |
| `graft/desktop-visual` | "Do known GUI desktop tools render visibly?" | `graft/desktop-visual` | host-side command |
| `graft/startup-loop` | "Does repeated launch/stop stay clean?" | `graft/startup-loop` | host-side command |
| `graft/hostvol-smoke` | "Do MacRO/MacRW host volumes behave?" | `graft/hostvol-smoke` | host-side command |
| `make hosted-hostvolume` | "Does the host-volume Unicode/sidecar glue behave?" | `hosted/hostvolume/v_test.c` | host-side make target |
| `graft/loadmatrix-smoke` | "Do runtime libraries/devices/datatypes open?" | `graft/loadmatrix-smoke` | host-side command |
| `graft/clipboard-smoke` | "Does Mac→AROS clipboard sync pause/resume correctly?" | `graft/clipboard-smoke` | host-side command |
| `make hosted-coreaudio` | "Does the CoreAudio ring/offline render path work?" | `hosted/coreaudio/a_test.c` | host-side make target |
| `make coreaudio-abi` | "Does the deployable CoreAudio dylib ABI work?" | `hosted/coreaudio/a_test.c` via `libcoreaudio.dylib` | host-side make target |
| `make audio-smoke` | "Can real AROS play through CoreAudio/AHI?" | `graft/audio-smoke` + `C:AHISmoke` | host-side command |
| `make cocoametal-shell` | "Do the native app menu/actions drive the real dylib ABI?" | `hosted/cocoametal/shell_test.m` | host-side make target |
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

## 2. `dep-scan` — static pre-launch dependency-name scanner

```
./graft/dep-scan SYS:System/Wanderer/Wanderer
./graft/dep-scan LIBS:muimaster.library
./graft/dep-scan --depth 2 SYS:System/Wanderer/Wanderer
```

Runs on the Mac side against the discovered darwin-aarch64 boot image. It does
not execute AROS code. Instead it reads the target file with `strings`, extracts
embedded disk-object names such as `.library`, `.device`, `.datatype`, `.mcc`,
`.hidd`, and `.audio`, resolves them in the boot image, and optionally recurses.

Use it before launching when you want a fast "what files does this thing name?"
view. Example direct Wanderer scan currently resolves `stdc.library`,
`muimaster.library`, `icon.library`, `workbench.library`, `IconImage.mcc`, and the
other desktop libraries from the boot image.

Important limits:

- It is static and string-based, so it can include optional names and miss names
  built dynamically.
- Default depth is `1` to avoid noise from resident core libraries naming legacy
  optional devices. Use `--depth N` when deliberately exploring.
- By default it reports `MISS` but exits successfully; use `--strict` when a CI
  or smoke step should fail on a missing static name.
- It complements `C:TestLib` and `C:LoadMatrix`; it does not prove
  `OpenLibrary()`/`OpenDevice()` initialization succeeds.

---

## 3. `LoadMatrix` — desktop load smoke test

```
LoadMatrix
```

Runs a fixed list of `OpenLibrary()` and `OpenDevice()` calls for the desktop
stack and reports one OK/FAIL table. It is intentionally boring: it does not
discover dependencies, it just answers "is the set we currently need for
Wanderer present and openable?"

The current matrix covers `stdc.library`, `posixc.library`, `png.library`,
`datatypes.library`, `icon.library`, `intuition.library`, `graphics.library`,
`layers.library`, `asl.library`, `cybergraphics.library`, `muimaster.library`,
`muiscreen.library`, `picture.datatype`, `png.datatype`,
`DEVS:AHI/coreaudio.audio`, `timer.device`, `ahi.device`, `input.device`,
`keyboard.device`, and `clipboard.device`.

Run it inside AROS before chasing a Wanderer crash. If `LoadMatrix` fails, fix
deployment/loading first. If it passes and Wanderer still dies, the problem is
past the loader.

Build: `make workbench-c-loadmatrix` → `AROS/C/LoadMatrix`.

Host-side unattended wrapper:

```
./graft/loadmatrix-smoke
```

This boots AROS with an isolated `MacRW:` folder, runs `LoadMatrix` inside AROS,
saves `loadmatrix.txt`, screenshot, and log under
`run/darwin-aarch64/loadmatrix-<timestamp>.*`, and fails if any row starts with
`FAIL`.

The smoke startup also creates/assigns `T:` and registers
`DEVS:AudioModes/COREAUDIO` before running the matrix, so it catches broken
CoreAudio mode registration and the `ahi.device` / `coreaudio.audio` loader path.

---

## 4. `deploy-check` — stale deployment guard

```
./graft/aros-ctl deploy
./graft/deploy-check
```

Runs on the Mac side. `aros-ctl deploy` copies the current host dylibs, settings
schema, and `Storage/Monitors/Cocoa` into the runnable locations without
launching AROS. `deploy-check` then prints the discovered boot tree and
hashes/timestamps for the files that most often go stale:

- `build/cocoametal.dylib` vs `~/lib/cocoametal.dylib`
- `build/libpasteboard.dylib` vs `~/lib/libpasteboard.dylib`
- `build/libcoreaudio.dylib` vs `~/lib/libcoreaudio.dylib`
- `build/libbsdsockhost.dylib` vs `~/lib/libbsdsockhost.dylib`
- `hosted/cocoametal/settings.json` vs `~/lib/settings.json`
- the effective `AROS_HOST_VOLUME` generated from `aros-host.conf` by the same
  helper sourced by `run-window.sh`, `aros-ctl`, and the `.app`
- `build/Daedalos.app` embedded bootstrap, config, host dylibs, settings schema,
  and `aros-host-conf.sh` if the bundle exists
- `Storage/Monitors/Cocoa` vs `Devs/Monitors/Cocoa`
- bootstrap, `emul-handler`, key libraries, `TestLib`, `LoadMatrix`, Wanderer
- presence of the fuller desktop payload: `Prefs/Zune`, `Prefs/Wanderer`,
  `System/About`, `Tools`, `Utilities`

Interpretation:

- `OK` on dylibs/schema/monitor means the launch path is using the current copy.
- `WARN` on `Cocoa deployed` means `Devs/Monitors/Cocoa` is stale relative to
  `Storage/Monitors/Cocoa`. Run `./graft/aros-ctl deploy` before checking if you
  are testing without a fresh launch.
- `MISS` in desktop presence means the fuller desktop set has not been
  built/deployed yet. It is not a loader failure by itself.

For the full deployment map, see [deployment](../deployment/README.md).

### Common startup requester: `Insert volume T:`

If AROS shows a requester asking to insert `T:`, it is not an input-mode or
keyboard message. It means a command needed the temporary volume `T:` and startup
did not assign it. The current launchers and smoke tests create `RAM:T` and run:

```
Assign T: RAM:T
```

Keep that assignment in any custom startup file used by a harness test,
especially before commands that redirect, list, copy, rename, or launch desktop
components.

---

## 5. lddemon loader trace

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

## 6. `desktop-smoke` — visual desktop proof

```
./graft/desktop-smoke [/tmp/aros-desktop.png]
```

Runs the host-side desktop check loop:

1. `aros-ctl deploy`
2. `deploy-check`
3. stop any previous harness instance
4. start `aros-ctl run` with `AROS_CTL_STARTUP_MODE=desktop`
5. wait for the desktop to settle
6. scan the log for traps/alerts
7. capture a screenshot through the control FIFO
8. move the pointer, click `RAM Disk`, and capture an interaction screenshot

It prints `PASS` only if the process is still alive, the screenshot exists, and no
crash markers appeared in the log. Use `AROS_DESKTOP_SMOKE_WAIT=<seconds>` if a
slower build needs more time before capture.

When no screenshot path is supplied, artifacts are written under
`run/darwin-aarch64/desktop-<timestamp>.png`. Override the directory with
`AROS_DESKTOP_SMOKE_RUN_DIR` and the timestamp label with
`AROS_DESKTOP_SMOKE_RUN_STAMP`.

By default, the smoke test fails on traps, alerts, host halts, and null window
opens. Set `AROS_DESKTOP_SMOKE_STRICT_LOG=1` when you also want broad triage
markers from `aros-ctl crash` (including supervisor-mode semaphore warnings) to
fail the run.

Set `AROS_DESKTOP_SMOKE_MENU=1` to also right-click the Wanderer menu bar and
capture a menu screenshot. The click defaults to logical `70,8`; override with
`AROS_DESKTOP_SMOKE_MENU_X`, `AROS_DESKTOP_SMOKE_MENU_Y`, and
`AROS_DESKTOP_SMOKE_MENU_SHOT`. The menu probe uses `aros-ctl menu` / `menuup`,
so it holds RMB while capturing and publishes a tiny held-move for deterministic
menu automation. This is currently an explicit investigation mode, not part of
the default pass criteria, but the current darwin-aarch64 desktop proves the
Wanderer menu opens and paints through both the held-menu helper and the raw RMB
path.

Set `AROS_DESKTOP_SMOKE_MENU_MODE=raw` to test the lower-level path: one pointer
move, one RMB-down, screenshot, RMB-up. Use `AROS_DESKTOP_SMOKE_MENU_NUDGE=1`
only for comparison while debugging.

Set `AROS_DESKTOP_SMOKE_MENU_REPEAT=<n>` to repeat the menu-open screenshot cycle
inside one boot. This is the quickest regression check for the former
`DefaultMenuHandler -> BltTemplateBasedText` crash.

Run this from a normal macOS Terminal/GUI login. A restricted automation sandbox
may be allowed to read the repo but not keep the GUI process alive; in that case
you can still run `deploy-check`, but `desktop-smoke` is expected to fail early
with an empty or short log.

---

## 7. `desktop-visual` — GUI tool proof

```
./graft/desktop-visual
```

Boots desktop mode with a known GUI tool launched from startup. By default it runs
`Clock`, then captures a screenshot and copies the boot log to
`run/darwin-aarch64/desktop-visual-<timestamp>.png/.log`.

Use this after a desktop build when you want visual proof that Wanderer is not
only open, but can launch and render a normal Workbench GUI program.

Environment knobs:

- `AROS_DESKTOP_VISUAL_EXTRA` — startup command to run instead of `Clock`
- `AROS_DESKTOP_VISUAL_WAIT` — settle time before screenshot
- `AROS_DESKTOP_VISUAL_RUN_DIR` / `AROS_DESKTOP_VISUAL_RUN_STAMP` — artifact path

---

## 8. `startup-loop` — brittle launch detector

```
./graft/startup-loop [count]
```

Launches desktop mode repeatedly, screenshots each run, stops the app through
`aros-ctl stop`, and checks that no hosted AROS process is left behind. `stop`
now asks the guest to shut down through `CM_OPT_POWER` before falling back to
TERM/KILL cleanup, so this is a lifecycle test rather than a repeated hard kill.
Artifacts go to `run/darwin-aarch64/startup-loop-<timestamp>-<n>.png/.log`.

The log check intentionally fails on traps, alerts, null window opens, host halts,
and supervisor-mode semaphore warnings. Those warnings are often the first visible
sign of a brittle next launch.

---

## 9. `hostvol-smoke` — MacRO/MacRW filesystem regression test

```
./graft/hostvol-smoke
```

Creates an isolated host directory under `run/darwin-aarch64/`, boots AROS with
that directory mounted as both `MacRO:` and `MacRW:`, and runs an unattended
startup script that checks:

- MacRW create/write/readback
- copy + rename
- delete
- Dir/examine output
- MacRO read-only rejection
- case-insensitive host lookup
- long ASCII filenames
- mounted NFD host-name lookup through an NFC/Latin-1 AROS name
- UTF-8 host names reached by Latin-1 AROS bytes
- AROS Latin-1 filename creation as UTF-8 on the host
- file comments/protection sidecar creation, rename pairing, and delete cleanup

It then verifies the actual host-side files. This caught the darwin/aarch64
variadic `open()` creation-mode bug: files were created as POSIX mode `000`, so
later AROS rename/delete operations failed as write-protected. The emul-handler
now reapplies creation permissions through non-variadic `chmod()`.

Future expansion should add true case-conflict behavior on case-sensitive host
volumes, more sidecar overwrite/default edge cases, and optional live add/remove
behavior. The normal boot/config `MacRO:`/`MacRW:` mount path is already wired.

Run `make hosted-hostvolume` after touching the shared host-volume glue. That
standalone verifier exercises NFC/NFD normalization, Latin-1↔UTF-8 filename
round-trips, unrepresentable-character escaping, sidecar omit/default behavior,
and atomic/concurrent sidecar writes against the real macOS filesystem. It does
not replace `hostvol-smoke`: the smoke proves the AROS-mounted MacRO/MacRW path.

---

## 10. `clipboard-smoke` — pasteboard bridge regression test

```
./graft/clipboard-smoke
```

Deploys current host artifacts, boots the normal console clipboard path, changes
macOS `NSPasteboard` with `pbcopy`, and uses the Cocoa bridge log as the oracle.
It explicitly enables sharing before the first propagation check so a persisted
local Settings choice cannot skew the result. It checks:

- the bridge starts and baselines after `ConClip.rendezvous`
- a Mac text token crosses to AROS as `host->AROS`
- `graft/aros-ctl clipboard off` is consumed by the AROS-side settings path
- a token copied while disabled does **not** cross
- `graft/aros-ctl clipboard on` re-baselines without replaying the disabled token
- a fresh token crosses after re-enable

It writes an isolated log to `/tmp/aros-clipboard-smoke-<timestamp>.log`, saves a
copy under `run/darwin-aarch64/clipboard-smoke-<timestamp>.log`, and restores the
previous textual macOS clipboard content when possible. This is a Mac→AROS and
runtime-toggle regression test; AROS→Mac still needs either a real text-selection
test or a small AROS-side clip-writer helper before it can be fully unattended.

---

## 11. CoreAudio Host Shim Checks

```
make hosted-coreaudio
make coreaudio-abi
make audio-smoke
```

`hosted-coreaudio` proves the Mac-side CoreAudio ring/render code linked directly
into the test binary. `coreaudio-abi` proves the same path through
`build/libcoreaudio.dylib`, the artifact the AROS-side CoreAudio AHI driver
loads through `hostlib.resource`.

Both are headless and silent. The oracle is `run/coreaudio-a.wav` plus numeric
checks: RMS, 440 Hz dominant frequency, frame count, zero underruns, and zero
RT-thread AROS calls. They also render `run/coreaudio-volume50.wav` after
`ca_set_global_volume(50)` and assert the RMS is halved; `cocoametal-abi` then
proves `cm_set_option(CM_OPT_AUDIO_VOLUME, 25)` reaches that CoreAudio global
gain through the deployed dylib boundary.

`audio-smoke` is the end-to-end AROS-side validation. It deploys the host dylib,
boots the real app with a temporary startup file, runs
`AddAudioModes DEVS:AudioModes/COREAUDIO`, then runs `C:AHISmoke`, a direct
`ahi.device` client that plays a generated one-second tone through the CoreAudio
AHI sub-driver. The log oracle is live host output start, first PCM ring push,
and clean stop statistics; the harness also stores a screenshot under
`run/darwin-aarch64/`.

For normal launches, both `graft/aros-ctl` and `graft/run-window.sh` create
`RAM:T` and assign `T:` before running startup commands. This prevents the DOS
"Insert volume T:" requester when a real command needs temporary storage.
Console startup registers the CoreAudio mode synchronously; desktop startup runs
the registration in a quiet background task so Wanderer is not blocked by AHI
database work.

There are two useful AROS-side validators:

- `C:AHISmoke` directly validates the `CoreAudio` AHI mode without depending on
  sound DataTypes.
- `C:Play FILE` plays a DataTypes-readable sound file through the default audio
  output; it is useful later, but the current image cannot open the generated
  WAV as a DataTypes object.
- `PlaySineEverywhere` generates a tone and exercises every registered AHI mode.

---

## 12. Capture / screenshot diagnostics

Three ways to capture what AROS is drawing — useful for unattended PASS/FAIL and for
bug reports:

- **`C:GrabScreen <file>`** — an AROS-side command that reads the front screen's
  bitmap directly and writes a PPM. Always correct (reads AROS memory), no host TCC.
- **`aros-ctl shot [PATH]`** — the harness wrapper: runs `GrabScreen`, converts to
  PNG via `sips`. See [control-harness](../control-harness/README.md).
- **In-app "Take Screenshot" (⇧⌘3)** — the Daedalos menu; writes
  `AROS-screenshot-*.png` under `$AROS_RUN_DIR` when launched by `aros-ctl` /
  `run-window.sh`, or `~/Desktop` when used as a standalone app. It uses the host
  `cm_capture_png` → `cm_readback` path. (Captures the Metal framebuffer as
  **opaque** — AROS leaves the alpha byte at 0, so this path must ignore alpha or
  the desktop saves out transparent/white.)

`make cocoametal-shell` also invokes the real in-app screenshot action against the
production dylib and verifies the PNG lands in the requested run directory.

---

## 13. Native app shell action test

```
make cocoametal-shell
```

Runs `hosted/cocoametal/shell_test.m` against `build/cocoametal.dylib`, not a
mock. It opens the real shell, checks the menu tree, and invokes the actual menu
actions for screenshot, Settings, full screen, scaling/filter, Retina, theme,
clipboard sharing, reset/power, volume-add string relay, and movie capture.

This catches stale or broken Daedalos menu wiring before manual Wanderer testing.

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
2. Run `./graft/aros-ctl deploy && ./graft/deploy-check`. Fix stale
   dylibs/schema/monitor before launching.
3. Run `./graft/desktop-smoke` from a normal Terminal for visual proof, or boot
   manually with `AROS_CTL_STARTUP_MODE=desktop ./graft/run-window.sh` while
   debugging interactively.
   Use `./graft/aros-ctl status` to distinguish a live clean app from a stale
   pid/FIFO/log or a process that already has crash markers.
4. Run `AROS_DESKTOP_SMOKE_RESIZE=1 ./graft/desktop-smoke` after display/input
   changes; it performs a bounded host-window resize sequence and saves a
   post-resize screenshot.
5. Run `AROS_DESKTOP_SMOKE_STRESS=1 ./graft/desktop-smoke` after input/event-pump
   changes; it performs a bounded pointer/click burst and saves a stress
   screenshot.
6. Run `AROS_DESKTOP_SMOKE_MENU=1 AROS_DESKTOP_SMOKE_MENU_MODE=raw
   ./graft/desktop-smoke` after mouse/menu changes. It is still opt-in, but it is
   expected to pass and visually show the Wanderer menu.
7. Run `./graft/desktop-visual` for one normal GUI tool.
8. Run `./graft/startup-loop 3` after launch/shutdown or deployment changes.
9. Run `make hosted-hostvolume` after host-volume charset/normalization/sidecar
   glue changes.
10. Run `./graft/hostvol-smoke` after emul-handler, host-volume, clipboard, CLIPS,
   startup, or shared-folder changes.
11. Run `./graft/loadmatrix-smoke`. Loader failures come before Wanderer debugging.
12. Run `./graft/clipboard-smoke` after clipboard, settings, hostlib, ConClip,
   startup, or host-volume changes.
13. Verify visually with `graft/aros-ctl shot /tmp/aros.png`.
14. If it crashes, use the backtrace module/function first. Only reopen the old
   relbase theory if the trace names base-getter or `x16`/`x17` handoff code.

The host-side smoke scripts share one GUI/control-FIFO owner at a time. They take
`/tmp/aros-harness.lock` by default and fail immediately if another smoke is
running. Override with `AROS_HARNESS_LOCK_DIR` only when you deliberately use a
separate log/control/pid namespace too; otherwise parallel runs will race on
`/tmp/aros-window.log`, `/tmp/aros-cm.ctl`, and the live Daedalos process.
