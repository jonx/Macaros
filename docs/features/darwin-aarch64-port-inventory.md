# Darwin AArch64 Port Inventory

Last checked: 2026-06-28

This is a working inventory of the source-tree areas that need Darwin/aarch64
adaptation for the hosted Apple Silicon port. It separates "already adapted",
"adapted but needs hardening", and "still missing / needs creation".

## Current Baseline

Verified in the current build tree:

- The shell/window path works.
- Wanderer opens visually through both `graft/aros-ctl run` and
  `graft/run-window.sh` when started in desktop mode.
- `C:TestLib` opens the main Wanderer dependency chain: `stdc.library`,
  `posixc.library`, `png.library`, `muimaster.library`, `picture.datatype`, and
  `png.datatype`.
- The build image contains the expected desktop pieces:
  `C/Decoration`, `C/TestLib`, `C/GrabScreen`, `Libs/stdc.library`,
  `Libs/posixc.library`, `Libs/muimaster.library`, `Libs/icon.library`,
  `Classes/DataTypes/picture.datatype`, and `Classes/DataTypes/png.datatype`.

Important caveat: feature READMEs are the current status docs. The design/spec
files are design history unless explicitly updated, so do not treat planning
language there as a live blocker by itself.

## 1. CPU / ABI / Toolchain

Status: mostly adapted, still needs cleanup and broader tests.

Source areas:

- `arch/aarch64-all/`
- `arch/all-darwin/kernel/cpu_aarch64.h`
- `compiler/crt/stdc/`
- `compiler/crt/posixc/`
- `compiler/startup/`
- `tools/genmodule/`
- `tools/collect-aros/`

Already present / recently adapted:

- aarch64 Exec stack/context machinery exists in `arch/aarch64-all/exec/`.
- aarch64 `setjmp` / `longjmp` exists in `arch/aarch64-all/stdc/`.
- posixc aarch64 `sigsetjmp`, `siglongjmp`, `vfork`, and `vfork_longjmp` were
  added in `compiler/crt/posixc/`.
- `collect-aros` has support for clang `@response-file` expansion.
- genmodule/aarch64 calling stubs have active fixes in flight.

Still needed:

- Turn the aarch64 genmodule/startup fixes into a clearly documented ABI rule:
  which register carries the library base, which registers are scratch in
  generated stubs, and how program startup receives its base.
- Add small executable/library ABI tests for:
  `OpenLibrary`, dynamic program startup, BOOPSI class init/expunge, detach,
  and varargs/math calls.
- Audit broad `#error` fallbacks. Some are harmless generic fallbacks because
  arch-specific code exists, but these are real remaining blockers:
  `workbench/classes/datatypes/heic/heicclass.c` needs an aarch64 stack-pointer
  implementation, and AROSTCP has a 32-bit fd-mask/longword assumption.

## 2. Hosted Darwin Runtime

Status: functional enough for shell/desktop, needs hardening.

Source areas:

- `arch/all-hosted/`
- `arch/all-unix/`
- `arch/all-darwin/`
- `hosted/`
- `graft/`

Already present:

- hosted bootstrap, hostlib, unixio, and darwin Cocoa host shims exist.
- the control harness exists and can boot, inject input, and capture screenshots.
- deployment scripts now print hashes/timestamps for more runtime artifacts,
  reducing stale-build confusion.

Still needed:

- Clean shutdown audit: no orphaned bootstrap/window processes, no stale locks,
  no half-closed host resources.
- Host thread to AROS task signaling needs a reusable contract used by
  clipboard, audio, sockets, and future app shell services.
- Harden host/AROS lock boundaries. Past freezes were often not "the app
  crashed", but the host event path entering AROS while a critical lock/scheduler
  state was held.
- W^X / `MAP_JIT` executable-memory policy still matters for future 68k JIT and
  any native dynamic code path.

## 3. Cocoa Display, Input, Clipboard, And Harness

Status: working, but this is still a hot area.

Source areas:

- `arch/all-darwin/hidd/cocoa/`
- `hosted/cocoametal/`
- `hosted/clipboard/`
- `graft/aros-ctl`
- `graft/run-window.sh`

Already present:

- Cocoa/Metal display opens and renders the shell and Wanderer.
- keyboard/mouse input works through the hosted input path.
- clipboard bridge exists and has been exercised.
- `C:GrabScreen`, `aros-ctl shot`, and host screenshot support exist.

Still needed:

- Resize interaction needs hardening. A previous freeze during resize while the
  clipboard heartbeat continued suggests the system was alive but the display or
  input/UI path wedged.
- Input should have a bounded host-side queue or coalescing policy for mouse
  motion/resize/key-repeat bursts. Full queuing may be overkill, but an explicit
  drop/coalesce rule is not.
- Clipboard status should be read from the feature README / current build
  evidence. The design/spec files are planning artifacts and may still describe
  the original intended implementation.
- The bridge should avoid startup races by waiting for the actual AROS services
  it needs instead of using delay-based readiness.

## 4. Desktop, Datatypes, Zune, And Userland

Status: Wanderer baseline works; broad userland still needs expansion.

Source areas:

- `workbench/c/`
- `workbench/system/Wanderer/`
- `workbench/libs/`
- `workbench/classes/`
- `rom/lddemon/`

Already present:

- `stdc.library` and `posixc.library` build and open in the current image.
- `lddemon` tracing and `C:TestLib` make library-load failures observable.
- `png.library`, `picture.datatype`, and `png.datatype` build/open.
- `C/Decoration` is present in the current build image.

Still needed:

- Reconcile stale build comments around `workbench/c/mmakefile.src`. It still
  describes Decoration as blocked by `__ieee754_sin`, while the current image
  contains `C/Decoration`. The wrapper Makefile did not expose
  `workbench-c-decoration`, so the build graph/status needs cleanup.
- Add a dependency walker for commands/tools. `TestLib` proves one library opens;
  a walker should report the whole dependency ladder before runtime.
- Add a library/device load matrix run at boot or under `aros-ctl`: important
  libraries, classes, datatypes, and devices should be opened once and reported
  as PASS/FAIL.
- Expand datatype coverage beyond the currently verified picture/png path.
  `heic.datatype` has an explicit aarch64 stack-pointer compile blocker.

## 5. Host Volume / Emul Handler

Status: usable, but still one of the riskier subsystems.

Source areas:

- `arch/all-hosted/filesys/emul_handler/`
- `arch/all-unix/filesys/emul_handler/`
- `hosted/hostvolume/`
- `docs/features/host-volume/`

Already present:

- host folder sharing is wired into `run-window.sh` as `MacRO:` and `MacRW:`.
- unicode/path normalization work is in the tree.
- the desktop path exercises host-backed volumes.

Still needed:

- Reconcile any status-sensitive notes into the README. The design/spec files
  can keep their planning context, but the README should be the quick current
  answer for what has landed.
- Continue hardening `NameToHost` / examine paths. Previous startup crashes were
  in normalization/examine, so this area needs regression tests.
- Add tests for ASCII, NFC/NFD Unicode, long names, case conflicts, metadata
  sidecars, delete/rename, and read-only vs read-write mounts.
- Dynamic mount/unmount and Finder-friendly app-shell controls are not done.

## 6. Networking

Status: mostly missing for Darwin hosted.

Source areas:

- `docs/features/bsdsocket-net/`
- `workbench/network/`
- `arch/all-unix/devs/networks/`
- `arch/all-mingw32/bsdsocket/` as a possible host-passthrough reference shape.

What exists:

- network headers are present in the image.
- Unix `eth`/`tap` SANA-II style drivers exist under `arch/all-unix/devs/networks`.
- AROSTCP source exists under `workbench/network/stacks/AROSTCP`.

Still needed:

- There is no `arch/all-darwin/bsdsocket` host-passthrough implementation.
- AROSTCP has an explicit 64-bit blocker:
  `amiga_api.c` says it depends on 32-bit `fd_mask`/longword sizing.
- Decide between two paths:
  implement the planned Darwin host-passthrough `bsdsocket.library`, or make
  AROSTCP + SANA-II viable on darwin-aarch64. The host-passthrough path is
  probably the smaller first win.

## 7. Audio

Status: missing Darwin host audio.

Source areas:

- `docs/features/coreaudio-audio/`
- `workbench/devs/AHI/`
- future `arch/all-darwin` or hosted CoreAudio bridge code.

What exists:

- AHI core and many drivers exist in `workbench/devs/AHI/`.
- Linux/PulseAudio/ALSA-style hosted bridge patterns exist.

Still needed:

- No CoreAudio/AUHAL-backed AHI sub-driver exists for the Darwin hosted port.
- Need a host callback to AROS signaling contract shared with other host-backed
  services.
- Need audio-mode registration and simple playback verification through
  `C:Play` or an AHI test.

## 8. Native Mac App Shell

Status: substantial host shell exists; wiring and hardening remain.

Source areas:

- `hosted/cocoametal/`
- `docs/features/host-app-shell/`
- `graft/`

Already present:

- `graft/run-window.sh` already performs the practical launch/deploy work:
  discovers the boot tree, deploys `cocoametal.dylib`, deploys
  `settings.json`, switches the Cocoa monitor into `Devs/Monitors`, writes the
  console/desktop startup sequence, maps host volumes, signs the launched
  `Daedalos` binary, and prints artifact hashes.
- `hosted/cocoametal/cocoametal_shell.m` installs the host menu bar, About
  panel, app icon, screenshot command, copy/paste commands, display controls,
  machine/power menu, and host-folder open action.
- `hosted/cocoametal/cocoametal_settings_schema.m` plus
  `hosted/cocoametal/settings.json` provide the schema-driven settings system.
  The panel persists to `NSUserDefaults` and `aros-host.conf`, and can apply
  live host options or enqueue AROS-facing option requests.
- `graft/make-aros-app.sh` builds a structural `Daedalos.app` bundle with the
  bootstrap, dylib, settings schema, launcher, and `aros-host-conf.sh`.
- `graft/aros-host-conf.sh` translates the Settings UI's plain shared-folder
  path into the launch-time `MacRO:`/`MacRW:` host-volume pair, while preserving
  explicit `Name:path[;WRITE]` specs.

Still needed:

- Keep launcher/config parity aligned as new settings are added. The current
  `run-window.sh`, `.app`, and `aros-ctl run` paths all source the same
  `aros-host.conf` helper for saved memory/shared-folder settings.
- Runtime volume mounting/unmounting UI and AROS-side handling. Launch-time
  shared-folder config is wired; live mount/unmount still needs the AROS-side
  consumer.
- Validate menu/action states with the control harness: screenshot, settings,
  full screen, scaling/filter, clipboard, folder sharing, reset/power.
- Better crash/exit presentation: AROS task halt should be visible and useful,
  but should not leave the Mac app in a confusing half-alive state.

## 9. 68k JIT

Status: planned, not integrated.

Source areas:

- `docs/features/68k-jit/`
- future CPU/JIT integration points.

Still needed:

- MAP_JIT/W^X allocation layer.
- instruction cache flush and host/guest code ownership rules.
- clear integration plan for m68k binaries under the hosted aarch64 system.

## 10. Tooling We Should Add Next

Highest-value additions:

- Dependency walker: given an AROS command/library/datatype, report its disk
  libraries/classes/devices before launch.
- Load matrix: drive `OpenLibrary`/`OpenDevice`/datatype-class opens for the
  known desktop stack and produce a single PASS/FAIL report.
- Deployment manifest: every boot should print and optionally verify hash,
  timestamp, and source build path for key files copied into `~/lib` and the
  boot image. This addresses stale-build confusion directly.
- Startup/shutdown test: launch/quit in a loop, assert no crash, no orphaned
  process, and no stale resource.
- Input stress test: bounded repeat/mouse/resize bursts with visual heartbeat
  and screenshot verification.

## Prioritized Missing Pieces

1. Reconcile stale docs/comments and build graph status, especially Decoration
   and clipboard/host-volume docs.
2. Add the dependency walker + load matrix so library/load failures are caught
   before manual Wanderer testing.
3. Harden host-volume/emul-handler with regression tests; it remains central to
   CLIPS:, MacRW:, desktop files, and startup reliability.
4. Harden Cocoa display/input resize and burst behavior.
5. Create Darwin `bsdsocket.library` host passthrough.
6. Create CoreAudio-backed AHI driver.
7. Finish wiring and hardening the existing app shell.
8. Resume 68k JIT only after executable-memory policy and the desktop baseline
   are stable.
