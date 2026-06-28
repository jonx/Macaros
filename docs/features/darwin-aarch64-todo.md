# Darwin AArch64 TODO

Last updated: 2026-06-28

This is the active order of work for the hosted Darwin/aarch64 port. The goal is
to keep the desktop baseline moving without losing days to opaque loader,
deployment, or input failures.

## Current Rule Of Thumb

- The feature READMEs are current status.
- Design/spec files are planning context unless their status header says they
  were updated after implementation.
- The old broad "relbase is broken" theory is closed for now. Reopen it only if
  a new failure shows `__aros_getoffsettable()`, `__aros_getbase_*`, or an
  `x16`/`x17` library-base handoff in the backtrace. Current failures are in the
  input/menu/font path, not relbase.

## First Six

1. **Fix RMB/menu input delivery.**
   - Symptom: right-click only opens/updates the menu after moving the mouse.
   - Likely cause: button events are not preceded by/paired with an AROS pointer
     position update in the way Intuition expects.
   - Success: right-button down opens the Wanderer menu without requiring mouse
     movement.

2. **Fix the menu text crash.**
   - Symptom: opening Wanderer menus can trap in
     `graphics.library BltTemplateBasedText`, reached from
     `intuition.library DefaultMenuHandler`.
   - Success: Wanderer menus can open repeatedly without host halt.

3. **Add the two bring-up tools that prevent wasted debugging.**
   - `DeployCheck`: verify key host dylibs and AROS image artifacts by hash,
     timestamp, and path so stale deployments are obvious.
   - `LoadMatrix`: run the known desktop library/class/datatype opens and print
     one PASS/FAIL table.
   - Success: one command answers "am I running what I built?" and one command
     answers "does the desktop load chain open?"

4. **Build/deploy the fuller desktop set.**
   - Wanderer itself only installs the desktop, classes, icons, env defaults, and
     `WANDERER:Tools`.
   - Add the surrounding useful desktop targets: `workbench-prefs`,
     `workbench-tools`, `workbench-utilities`, and `workbench-system-aboutaros`
     where they build on darwin-aarch64.
   - Success: `SYS:Prefs/Zune`, `SYS:Prefs/Wanderer`, `SYS:System/About`,
     `SYS:Tools`, and `SYS:Utilities` exist where expected.

5. **Verify the desktop visually.**
   - Check menu open/close, Tools menu enablement, GUI settings, About, window
     depth/front/back, and a few Wanderer tools.
   - Use `aros-ctl shot` / `GrabScreen` for evidence.

6. **Add startup/shutdown and host-volume hardening checks.**
   - Loop launch/quit to catch brittle startup and stale host resources.
   - Exercise `MacRO:`/`MacRW:` path translation, examine, rename/delete, and
     Unicode names.
   - Success: repeated starts do not produce the "first launch crashes, next
     launch works" pattern, and host-volume regressions are visible quickly.

## Next After The First Six

7. Create Darwin host-passthrough `bsdsocket.library`.
8. Create CoreAudio-backed AHI audio.
9. Finish wiring and hardening the existing native Mac app shell.
   The menu bar, About panel, icon, screenshot path, schema-driven Settings
   window, settings JSON, `aros-host.conf`, and app bundle helper already exist.
   The remaining work is launcher/config parity, AROS-facing settings handlers,
   runtime volume mount/unmount, lifecycle/quit behavior, and menu validation.
10. Resume the 68k JIT once executable-memory policy and desktop stability are
    boring.

## Explicit Non-Goals / Future Features

- Whole-system save-state on app exit is **not currently implemented** for the
  hosted AROS session. The existing 68k `core.snapshot` machinery is for
  crash/debugging inside the isolated JIT sandbox; it is not a restorable image
  of Exec tasks, devices, host windows, timers, open files, and AROS RAM.
- A future user-facing snapshot/resume feature would need a deliberate
  checkpoint contract across RAM, CPU/task state, Exec scheduler state,
  devices/resources, host shims, file handles, timers, and versioned restore
  metadata. Treat it as its own feature, not app-exit polish.

## Useful Desktop Targets To Investigate

- `workbench-system-wanderer`
- `workbench-prefs`
- `workbench-tools`
- `workbench-utilities`
- `workbench-system-aboutaros`
- `workbench-prefs-zune`
- `workbench-prefs-wanderer`
- `workbench-tools-editor`
- `workbench-tools-screengrabber`
- `workbench-utilities-multiview`
- `workbench-utilities-clock`

Some of these may reveal missing libraries or aarch64/toolchain issues. Build
them one batch at a time and feed failures back into `LoadMatrix` / deployment
checks.
