# Darwin AArch64 Port Inventory

Last checked: 2026-06-28

This is the single current inventory and work order for the hosted Darwin/aarch64
Apple Silicon port. It separates:

- **done baseline**: already adapted and verified enough to build on
- **active port completion**: work that moves the port toward feature-complete
  hosted AROS
- **deferred hardening**: useful tests/polish that should not distract from
  completing the missing port surfaces

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

## Active Port Completion Roadmap

These are the current priorities. Items marked **done** stay here only long
enough to preserve context for the next person picking up the tree.

1. **Deployment/load diagnostics, first desktop visual/input proof, bounded
   resize proof, clipboard pause/resume proof, startup loop, and first
   MacRO/MacRW smoke.** Done.
   Evidence and tools:
   `graft/desktop-smoke`, `graft/desktop-visual`, `graft/startup-loop`,
   `graft/hostvol-smoke`, `graft/clipboard-smoke`, `graft/deploy-check`,
   `C:TestLib`, `C:LoadMatrix`.
2. **Reconcile stale build/docs and make diagnostics first-class.** Active.
   Current focus: remove stale Decoration/link comments and keep this inventory
   as the single Darwin roadmap. `LoadMatrix` now tests the current desktop and
   audio load chain, including `ahi.device` and `DEVS:AHI/coreaudio.audio`.
   `graft/aros-ctl deploy` is the shared deploy path used by `run` and smokes,
   so `graft/deploy-check` can be made clean before a launch. `graft/dep-scan`
   now provides a static pre-launch disk-object name scan for a command/library.
3. **Complete non-network hosted runtime surfaces.** Active.
   Host-thread-to-AROS signaling, resize/input queueing, app lifecycle/quit
   handling, and clipboard runtime controls all belong here. One major host lock
   boundary is now fixed: Darwin hosted `hostlib.resource` uses `Forbid()` /
   `Permit()` instead of a regular semaphore, avoiding intermittent
   `ReleaseSemaphore called in supervisor mode` alerts from filesystem handler
   host calls.
4. **Host-volume boot/config path.** Integrated, with the main MacRO/MacRW smoke
   now passing repeatedly.
   `run-window.sh`, `.app`/`aros-ctl`, and `aros-host-conf.sh` all mount the
   configured Mac folder at boot as `MacRO:` read-only and `MacRW:` read/write.
   Basic ASCII read/write/copy/rename/delete, read-only behavior, long names,
   case-insensitive lookup, mounted NFC/NFD and Latin-1/UTF-8 filename paths, and
   sidecar comment/protection round-trips pass. Still needed for completion: true
   case-conflict behavior on case-sensitive host volumes, remaining sidecar
   overwrite/default edge cases, and the optional live add/remove event path.
5. **CoreAudio-backed AHI audio.** Done at the low-level playback layer.
   `build/libcoreaudio.dylib` builds, passes its dylib ABI proof, deploys to
   `~/lib`, and bundles into `Macaros.app`. The AROS-side `CoreAudio` AHI
   sub-driver, `DEVS:AudioModes/COREAUDIO`, and `C:AHISmoke` are present.
   `make audio-smoke` boots the app, registers the mode, plays through
   `ahi.device`, captures a screenshot, and was audibly verified. Normal
   launcher startup now also assigns `T:` and registers the CoreAudio mode
   (synchronously for console, backgrounded for desktop). The host ABI now also
   proves global CoreAudio gain, and the app Settings volume control applies it.
6. **Native Mac app shell wiring.** Active, with the main action surface now
   covered by the real-dylib shell test.
   The shell exists; completion means launcher/config parity stays aligned,
   settings that enqueue AROS-side changes have consumers, and reset/quit/crash
   presentation is deliberate. Audio volume is host-applied through
   `libcoreaudio.dylib` and mirrored to AROS as a setting event. `make
   cocoametal-shell` verifies the real menu/actions for screenshot, Settings,
   full screen, scaling/filter, Retina, theme, clipboard, power/reset, string
   volume-add relay, and movie capture.
7. **Networking / `bsdsocket.library`.** Done at the core TCP/IP layer.
   `build/libbsdsockhost.dylib` builds, passes host pump / ABI / errno proofs,
   deploys to `~/lib`, and bundles into `Macaros.app`. The AROS-side
   `bsdsocket.library` loads through hostlib, uses non-blocking host sockets with
   a kqueue readiness pump plus timer-polled AROS handoff, and has live proof for
   localhost TCP round-trip, `WaitSelect`, outbound HTTP, and DNS. Remaining LVOs
   such as `gethostbyaddr`, service/protocol lookup, `inet_*`,
   socket ownership/duplication, `sendmsg`/`recvmsg`, and socket events are safe
   secondary stubs to fill as real applications need them.
8. **68k JIT integration and user-facing snapshot/resume.** Deferred until the
   hosted desktop/runtime surfaces above are boring. The existing 68k snapshot
   machinery remains a crash/debug feature, not an app-session save state.

## Deferred Hardening / Later TODO

Keep these visible, but do not let them supersede port completion:

- Broader Wanderer interaction coverage: Tools enablement, settings panels,
  About, window depth/front/back, additional tools.
- Larger host-volume regression suite beyond the first smoke: sidecar overwrite/
  default edge cases, true case conflicts, and optional live add/remove. Unicode
  and mounted Latin-1/UTF-8 paths are already covered by current smokes.
- Broader input/resize stress harness with longer bursts and visual heartbeat.
  First bounded resize and pointer/click stress layers exist via
  `AROS_DESKTOP_SMOKE_RESIZE=1` and `AROS_DESKTOP_SMOKE_STRESS=1`
  `./graft/desktop-smoke`.
- More exhaustive startup/shutdown audits: stale locks, half-closed host
  resources, app-bundle reset/power flows.
- Whole-system save-state/resume. This needs its own checkpoint contract across
  Exec tasks, devices, files, timers, host shims, RAM, CPU state, and versioned
  metadata.
- Host USB ([usb-iokit](usb-iokit/design.md)). Lowest priority / poor loop fit —
  only enumeration (`[UB1]`–`[UB3]`) is unattended; defer behind audio & sockets.
  Prior art to adapt: AROS already ships a **libusb vHCI** at `rom/usb/vusbhc/`
  (the Poseidon HCD seam), so the host bridge forwards transfers to host **libusb**
  and enumerates via **IOKit** rather than writing an HCD from scratch.

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
- `graft/startup-loop` repeatedly launches/stops desktop mode, saves evidence in
  `run/darwin-aarch64/`, asks the guest to shut down through `CM_OPT_POWER`
  before signal fallback, and checks for stale hosted processes.

Still needed:

- Keep expanding the clean shutdown audit beyond the first `startup-loop` layer:
  stale locks, half-closed host resources, and app-bundle quit/reset behavior.
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
- clipboard bridge exists and has been exercised. It defaults to sharing enabled,
  avoids early AROS-facing settings events during `cm_open`, and has a smoke test
  that explicitly enables sharing before checking propagation/pause/resume.
- `C:GrabScreen`, `aros-ctl shot`, and host screenshot support exist.
- Pointer motion and left-click interaction are now part of the default
  `desktop-smoke` proof; the harness moves the pointer, clicks a desktop volume
  icon, and saves a second screenshot.
- Host-window resize is now scriptable via `graft/aros-ctl resize W H`; the
  optional `AROS_DESKTOP_SMOKE_RESIZE=1 ./graft/desktop-smoke` pass runs a
  bounded resize sequence and saves a post-resize screenshot.
- A bounded pointer/click stress pass is available through
  `AROS_DESKTOP_SMOKE_STRESS=1 ./graft/desktop-smoke`; it saves a stress
  screenshot after repeated UI traffic.
- RMB/menu input has harness support (`aros-ctl menu` / `menuup`) and an AROS-side
  deferred held-move pulse. `AROS_DESKTOP_SMOKE_MENU=1 ./graft/desktop-smoke`
  now visually proves Wanderer's backdrop menu opens and paints.
- Cocoa mouse motion is delivered as classic absolute `IECLASS_RAWMOUSE`, matching
  the other hosted mouse drivers. Do not switch it to `NEWPOINTERPOS/NEWTABLET`
  for the normal pointer path; that regression made the desktop visible but the
  Wanderer pointer appear stuck.
- The AROS-side Cocoa input task coalesces consecutive mouse-motion and resize
  bursts inside one host-pump batch, preserving key/button ordering while keeping
  resize/motion floods from overwhelming `input.device`/Intuition.

Still needed:

- Continue resize interaction verification with a real drag/manual stress pass.
  A previous freeze during resize while the clipboard heartbeat continued
  suggests the system was alive but the display or input/UI path wedged; the
  first coalescing layer is now in place but needs more abuse.
- Clipboard status should be read from the feature README / current build
  evidence. The bridge waits for `ConClip.rendezvous` before opening
  `clipboard.device`, and the `CM_OPT_CLIPBOARD_SHARE` setting now controls the
  runtime bridge state. `graft/clipboard-smoke` proves Mac→AROS propagation,
  pause, re-baseline, and resume. Remaining work is unattended AROS→Mac coverage
  and an explicit shutdown path.

## 4. Desktop, Datatypes, Zune, And Userland

Status: Wanderer baseline works; initial useful desktop payload now builds.

Source areas:

- `workbench/c/`
- `workbench/system/Wanderer/`
- `workbench/libs/`
- `workbench/classes/`
- `rom/lddemon/`

Already present:

- `stdc.library` and `posixc.library` build and open in the current image.
- `lddemon` tracing and `C:TestLib` make library-load failures observable.
- `C:LoadMatrix` opens the current desktop libraries, datatypes, and devices in
  one PASS/FAIL table, including `ahi.device` and
  `DEVS:AHI/coreaudio.audio`; `graft/loadmatrix-smoke` registers the CoreAudio
  mode, runs it unattended, and stores evidence in `run/darwin-aarch64/`.
- `png.library`, `picture.datatype`, and `png.datatype` build/open.
- `C/Decoration` is present in the current build image.
- `Prefs/Zune`, `Prefs/Wanderer`, `System/About`, `Tools/Editor`,
  `Tools/ScreenGrabber`, `Utilities/MultiView`, and `Utilities/Clock` build in
  the current darwin-aarch64 image.

Still needed:

- Keep improving dependency discovery. `graft/dep-scan` now reports static
  disk-object names from a command/library before launch, while `TestLib` and
  `LoadMatrix` prove actual runtime open/init behavior.
- Keep expanding `LoadMatrix` as new port surfaces land.
- Expand datatype/tool coverage beyond this first payload. The broad meta-targets
  still pull much larger dependency sets; build those deliberately, one group at
  a time. `heic.datatype` still has an explicit aarch64 stack-pointer compile
  blocker.

## 5. Host Volume / Emul Handler

Status: usable, but still one of the riskier subsystems.

Source areas:

- `arch/all-hosted/filesys/emul_handler/`
- `arch/all-unix/filesys/emul_handler/`
- `hosted/hostvolume/`
- `docs/features/host-volume/`

Already present:

- host folder sharing is wired into `run-window.sh`, `aros-ctl`, and the app
  config helper as `MacRO:` and `MacRW:`. Settings writes the shared-folder path
  into `aros-host.conf`; launchers consume it on the next boot.
- unicode/path normalization work is in the tree.
- the desktop path exercises host-backed volumes.
- `make hosted-hostvolume` proves the shared host-volume glue against the real
  macOS filesystem: NFC/NFD normalization, Latin-1↔UTF-8 filename round-trips,
  unrepresentable-character escaping, sidecar omit/default behavior, and
  atomic/concurrent sidecar writes.
- `graft/hostvol-smoke` now checks isolated MacRO/MacRW create/read/copy/rename/
  delete/read-only behavior, case-insensitive lookup, long ASCII names, and
  metadata sidecar creation/rename/delete cleanup. It also covers mounted Unicode
  paths: NFD host filename lookup through an NFC/Latin-1 AROS name, UTF-8 host
  names reached by Latin-1 AROS bytes, and AROS Latin-1 filename creation as
  UTF-8 on the host. It stores screenshot/log evidence under
  `run/darwin-aarch64/`.
- The darwin/aarch64 hosted file-creation mode bug exposed by that smoke is fixed
  by applying the intended mode through non-variadic `chmod()` after variadic
  host `open(..., O_CREAT, mode)` calls.
- The `FileInfoBlock.fib_Comment` sidecar path now returns handler-format
  BSTR comments, so DOS converts them correctly and `List` shows the full
  comment instead of dropping the first character.
- Darwin hosted `hostlib.resource` now uses `Forbid()` / `Permit()` for the
  global host-call gate. This removes the intermittent supervisor-mode
  semaphore alert seen in `emul-handler` operations such as `MakeDir`.

Still needed:

- Continue hardening `NameToHost` / examine paths. Previous startup crashes were
  in normalization/examine, so this area needs regression tests.
- Add true case-conflict behavior tests on case-sensitive host volumes. The
  standalone glue and mounted NFC/NFD/Latin-1 paths are covered by
  `make hosted-hostvolume` and `graft/hostvol-smoke`.
- Add optional live add/remove tests once the app-shell `CM_OPT_VOLUME_ADD/REMOVE`
  string event has an AROS-side consumer. This is separate from the already
  integrated boot-time `MacRO:`/`MacRW:` mount path.
- Add any remaining sidecar overwrite/default edge cases not covered by
  `make hosted-hostvolume` or `hostvol-smoke`. ASCII create/copy/rename/delete,
  read-only vs read-write mounts, case-insensitive lookup, long ASCII names, and
  sidecar creation/rename/delete cleanup are
  covered by `hostvol-smoke`.
- Dynamic mount/unmount and Finder-friendly app-shell controls are not done.

## 6. Networking

Status: implemented and proven at the core TCP/IP sockets layer; secondary
compatibility surface remains.

Source areas:

- `docs/features/bsdsocket-net/`
- `hosted/bsdsocket/` in this repository
- `/Users/user/Source/aros-upstream/arch/all-unix/bsdsocket/`
- `/Users/user/Source/aros-upstream/workbench/network/`
- `arch/all-unix/devs/networks/` for the separate SANA-II / native-stack path.

What exists:

- A host-passthrough `bsdsocket.library` now exists under
  `arch/all-unix/bsdsocket/`. It is the current darwin-aarch64 networking path:
  AROS socket LVOs forward to macOS `libSystem` BSD sockets through
  `hostlib.resource`.
- `build/libbsdsockhost.dylib` provides the host kqueue readiness pump, async DNS
  resolver, non-blocking socket helpers, and Darwin-to-AmiTCP errno table.
- `graft/aros-ctl deploy`, `graft/run-window.sh`, and `graft/make-aros-app.sh`
  deploy/bundle `libbsdsockhost.dylib`; `graft/deploy-check` verifies it, plus
  `LIBS:bsdsocket.library`, `C:socktest`, and `C:nettest`.
- Host proofs pass: `make hosted-bsdsocket`, `make bsdsock-abi`, and
  `make bsdsock-errno`.
- Live AROS proofs pass through `graft/bsdsock-livetest.sh`: localhost TCP
  round-trip, `WaitSelect`, outbound HTTP fetch, and `gethostbyname` DNS fetch.
  Current screenshot evidence is in `run/darwin-aarch64/` and
  `docs/features/bsdsocket-net/`.
- The implementation deliberately uses kqueue for efficient host readiness and
  timer-poll `Delay()` handoff on the AROS side. It does not call `Signal()` from
  the host pump thread.
- Network headers, AROSTCP source, and Unix `eth`/`tap` SANA-II drivers still
  exist, but they are not the path used to provide current Darwin hosted
  internet access.

Still needed:

- Fill secondary LVOs as real applications need them:
  `gethostbyaddr`, `get{net,serv,proto}by*`, `inet_*`,
  `ObtainSocket` / `ReleaseSocket`, `Dup2Socket`, `SocketBaseTagList`,
  `sendmsg` / `recvmsg`, and `GetSocketEvents`.
- Add broader app-level socket-client coverage beyond the current `socktest` /
  `nettest` proof, especially once a browser, FTP/telnet/IRC client, or package
  tool is available in the image.
- Decide later whether AROSTCP + SANA-II should also be made viable on
  darwin-aarch64. That is now an optional native-stack project, not the first
  path to internet access. AROSTCP still has the documented 32-bit
  `fd_mask`/longword assumption.
- Add longer repeated WaitSelect/DNS/connect stress coverage to catch lifecycle
  leaks in the host pump and per-task `SocketBase` cleanup.

## 7. Audio

Status: implemented and audibly proven through AHI; build/UX polish remains.

Source areas:

- `docs/features/coreaudio-audio/`
- `workbench/devs/AHI/`
- `hosted/coreaudio/`
- AROS-side CoreAudio AHI sub-driver.

What exists:

- AHI core and many drivers exist in `workbench/devs/AHI/`.
- Linux/PulseAudio/ALSA-style hosted bridge patterns exist.
- The Mac-side CoreAudio shim exists in `hosted/coreaudio/`.
- The AROS-side `CoreAudio` AHI sub-driver exists under
  `/Users/user/Source/aros-upstream/workbench/devs/AHI/Drivers/CoreAudio/`.
- `DEVS:AudioModes/COREAUDIO` registers the `coreaudio` mode.
- `C:AHISmoke` opens `ahi.device`, allocates mode `0x00450002`, and plays a
  generated tone through CoreAudio.
- `C:LoadMatrix` covers `ahi.device` and `DEVS:AHI/coreaudio.audio` v6, while
  `graft/loadmatrix-smoke` registers `DEVS:AudioModes/COREAUDIO` before running
  the matrix.
- `make hosted-coreaudio` proves the SPSC ring and offline CoreAudio render path
  directly.
- `make coreaudio-abi` builds and proves `build/libcoreaudio.dylib`, the dylib a
  AROS-side driver loads through `hostlib.resource`.
- `make audio-smoke` proves the full hosted app path: deploy, boot, register
  the AHI mode, play `C:AHISmoke`, assert host CoreAudio ring activity, and save
  a screenshot under `run/darwin-aarch64/`.
- `graft/aros-ctl` and `graft/run-window.sh` create/assign `T:` during normal
  console and desktop startup, preventing DOS "Insert volume T:" requesters when
  startup commands need temporary files.
- Normal console startup registers `DEVS:AudioModes/COREAUDIO` before ConClip.
  Normal desktop startup starts `AddAudioModes` as a quiet background task before
  Wanderer; a synchronous registration there previously made the compact desktop
  startup hit a hosted stack/supervisor failure.
- `graft/aros-ctl deploy`, `graft/aros-ctl run`, and `graft/run-window.sh`
  deploy `build/libcoreaudio.dylib` to `~/lib/libcoreaudio.dylib` when it
  exists, and `graft/deploy-check` verifies source/destination hashes.
- `cm_set_option(CM_OPT_AUDIO_VOLUME, percent)` applies host CoreAudio gain
  immediately through `ca_set_global_volume()` and mirrors the setting event to
  the AROS Cocoa HIDD.
- `graft/make-aros-app.sh` bundles and verifies `libcoreaudio.dylib` in
  `Macaros.app/Contents/Frameworks/` when the artifact exists.

Still needed:

- Make the `ahi.device` localization/generated-file workaround first-class in
  the source build.
- Wire default AHI preferences / app settings so the CoreAudio mode is selected
  by normal desktop configuration, not only registered by launcher startup.
- Add mute and, if needed later, AHI-native mixer preference integration.
- Extend `audio-smoke` to repeated start/stop and longer-playback stress.

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
  `Macaros` binary, and prints artifact hashes.
- `hosted/cocoametal/cocoametal_shell.m` installs the host menu bar, About
  panel, app icon, screenshot command, copy/paste commands, display controls,
  machine/power menu, and host-folder open action.
- `hosted/cocoametal/cocoametal_settings_schema.m` plus
  `hosted/cocoametal/settings.json` provide the schema-driven settings system.
  The panel persists to `NSUserDefaults` and `aros-host.conf`, and can apply
  live host options or enqueue AROS-facing option requests.
- `graft/make-aros-app.sh` builds a structural `Macaros.app` bundle with the
  bootstrap, display dylib, optional host shims (`libpasteboard.dylib`,
  `libcoreaudio.dylib`, `libbsdsockhost.dylib`), settings schema, launcher, and
  `aros-host-conf.sh`.
- `graft/aros-host-conf.sh` translates the Settings UI's plain shared-folder
  path into the launch-time `MacRO:`/`MacRW:` host-volume pair, while preserving
  explicit `Name:path[;WRITE]` specs.

Still needed:

- Keep launcher/config parity aligned as new settings are added. The current
  `run-window.sh`, `.app`, and `aros-ctl run` paths all source the same
  `aros-host.conf` helper for saved memory/shared-folder settings.
- Optional runtime volume add/remove UI and AROS-side handling. Launch-time
  shared-folder config is wired through `aros-host.conf`; live add/remove still
  needs the AROS-side string-option consumer.
- Scalar AROS-facing settings now have a first AROS-side consumer: clipboard
  sharing toggles the bridge; power requests call the hosted shutdown/reset path;
  audio, display-mode, and volume requests log clearly until those backend
  surfaces land. Keyboard layout is boot-wired through `aros-host.conf`
  (`keymap pc105_f` for French) and desktop startup runs `C:SetKeyboard` when
  `AROS_CTL_KEYMAP` is set.
- `DEVS:Keymaps` is part of the desktop deployment surface now. The launchers
  guard `Assign KEYMAPS:` so a missing payload does not raise a requester, and
  `graft/deploy-check` reports the directory explicitly.
- Classic bitmap fonts (`SYS:Fonts`) and the fallback image path
  (`SYS:System/Images`) are staged into the boot image, so guarded desktop
  assigns resolve cleanly even outside the active theme path.
- `DEVS:Printers` remains a deferred TODO tied to the printing/CUPS bridge; it
  should stay visible in `graft/deploy-check` but should not be treated as a
  current desktop boot blocker.
- `graft/aros-ctl stop` uses that power path before falling back to process
  signals; this fixed the repeated desktop startup loop that could surface
  supervisor-mode semaphore alerts on the next launch after a hard kill.
- Keep the real-dylib shell action test green (`make cocoametal-shell` now
  covers screenshot, Settings, full screen, scaling/filter, Retina, theme,
  clipboard, reset/power, volume-add string relay, and movie capture).
  Optional live runtime volume add/remove still needs the AROS-side consumer.
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

- Dependency walker: first layer exists as `graft/dep-scan`, which reports
  static disk-object names from AROS commands/libraries/datatypes before launch.
  Keep improving it where static names are too noisy or incomplete.
- Load matrix: drive `OpenLibrary`/`OpenDevice`/datatype-class opens for the
  known desktop stack and produce a single PASS/FAIL report. First library/device
  layer exists as `C:LoadMatrix`; keep expanding it as new port surfaces land.
- Deployment manifest: every boot should print and optionally verify hash,
  timestamp, and source build path for key files copied into `~/lib` and the
  boot image. This addresses stale-build confusion directly.
- Startup/shutdown test: launch/quit in a loop, assert no crash, no orphaned
  process, and no stale resource. First layer exists as `graft/startup-loop`.
- Input stress test: bounded repeat/mouse bursts plus longer resize abuse with
  visual heartbeat and screenshot verification. The first resize and pointer/
  click stress layers are implemented; keep extending them rather than treating
  manual interaction reports as enough.

## Prioritized Missing Pieces

1. Reconcile stale docs/comments and build graph status, especially Decoration,
   clipboard, and host-volume docs.
2. Expand `graft/dep-scan` + `LoadMatrix` so library/device/datatype failures are
   caught before manual Wanderer testing.
3. Expand host-volume/emul-handler regression tests; the first MacRO/MacRW smoke
   exists and already caught one darwin/aarch64 host-libc boundary bug.
4. Harden Cocoa display/input resize and burst behavior. First host-window
   resize and pointer/click stress smokes are implemented; continue with longer
   burst/manual drag coverage.
5. Finish wiring and hardening the existing app shell. The current harness covers
   the main menu/action ABI; remaining work is the AROS-side consumers and
   deliberate halt/quit/crash presentation.
6. Add CoreAudio follow-up polish: build-rule cleanup, default AHI prefs,
   volume/mute, and longer audio stress.
7. Expand networking beyond the proven core: secondary `bsdsocket.library` LVOs,
   real socket-client app coverage, and longer WaitSelect/DNS/connect stress.
8. Resume 68k JIT only after executable-memory policy and the desktop baseline
   are stable.
