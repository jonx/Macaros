# Handoff — Feraille/gpui_aros completion batch (2026-07-06)

Goal of the batch: finish everything the Feraille-on-AROS audit flagged —
native shell, dirty-rect repaint, wheel/nav automation, emul-handler fix,
stack guard, and the gpufx GPU path (shim compute section + `gpufx.library`
+ consumers) — then run one on-device battery and bring the docs current.
Most of it is DONE and committed; this file is the map to finish the rest.

## State per repo (all local, nothing pushed)

### aros-aarch64 (branch `graft/feraille-gpui`)
- `f720bd6` stable tree locations (~/aros-build, ~/aros-crosstools) + `graft/rebuild-aros.sh`
- `0b0215d` gpufx design doc (docs/features/gpufx/)
- `714997c` `CM_EV_WHEEL` (shim: real scroll + control-FIFO `W <dy> [dx]`)
- `68bdc19` `aros-ctl wheel` subcommand (+ control-harness docs)
- `66ecea0` `hosted/exwalk/` ExWalk emul-handler stress tool (built: `hosted/exwalk/ExWalk`)
- `4bd7ff6` UPSTREAM-NOTES item 35 closed; `5ca9955`/`6c4fd95` upstream-patches snapshot refreshes
- `01bb6ef` **gpufx GFX0**: GPU compute section in the shim (`cocoametal_gpu.m`,
  `cm_gpu_open/abi/scale/convert_yuv420`, shares the display MTLDevice+queue via
  `cm__gpu_adopt`; `make cocoametal-gpu` PASS: nearest byte-exact, bilinear ±1, yuv ±2;
  `make cocoametal-shader` regenerates the metallib header)
- `b7b3f96` docs: feraille-gpui README + index rows caught up

### aros-upstream (branch `crash-containment`)
- `045ad110` + `a1211f2e` cocoa.hidd: wheel → NewMouse rawkeys 0x7A-0x7D; nav-key
  VK map (home/end/pgup/pgdn/fwd-del)
- `4b738004` **emul-handler stack 16 KB → 64 KB** (`EMUL_HANDLER_STACKSIZE`,
  `emul_init.c`) — root cause of the DoExamineNext bus-faults: deep darwin
  ExamineNext frames + signals delivered on the task stack overflowed 16 KB and
  spilled into neighboring tasks.

### zed-aros (branch `aros-platform`)
- `06317a4c5a` **native shell + dirty-rect**: Intuition menus (gadtools strips per
  window, CommKey from keymap, MENUPICK/NextSelect queue), asl.library
  `prompt_for_paths`/`prompt_for_new_path` (blocking, modal), pointerclass cursor
  set, `PlatformWindow::resize` via `ChangeWindowBox`; renderer fingerprint-diffs
  frames (`src/damage.rs`, host-tested `cargo test -p gpui_aros`, 15 PASS) and
  rasterizes+blits only the damaged rect (`gpa_blit` now takes the subrect).
- `4f1dea747a` KeybindingKeystroke accessor fix (build-only, check missed it).

### Feraille (branch `aros-port`)
- `777e537` untrack `crates/feraille-aros-app/build/` (was a 1.6 GB tracked ELF)
- `2ad2c3e`/`60c56fb`/`2704101` the inherited in-flight app work, landed
- `010de8d` folder-size walker re-enabled on AROS (gate reverted)
- `10b9229` stack self-guard in `feraille_main.c` (RunCommand re-entry at 16 MB)
- `6826b48` `link-aros.sh` strips debug info before deploy

## In flight RIGHT NOW

1. **A background agent is building GFX1+GFX2** (`gpufx.library` in aros-upstream
   via native-modules + hostlib, `C:GpuFxTest` under `hosted/gpufx/`, and routing
   the ffmpeg viewer's manual YUV→RGB through it with the scalar fallback). It
   commits on the current branches and refreshes upstream-patches. Check
   `git log` in both repos before doing anything gpufx-related; if it died,
   the full spec is in docs/features/gpufx/README.md + the cm_gpu_* contract at
   the bottom of hosted/cocoametal/cocoametal.h (CPU reference fallbacks:
   hosted/cocoametal/gpu_test.c).
2. **Disk was FULL** (rustc dying silently with exit 101 and empty
   `rustc-ice-*.txt` files is the ENOSPC signature — yesterday's mystery ICE
   included). ~1.7 GiB freed by deleting the stale
   `Feraille/crates/feraille-aros-app/build/Feraille`. Free more before big
   builds: Feraille/zed-aros `target/` dirs are the hogs; `/tmp/arosbuild2`
   (scratch tree) can go once emul-handler needs no more rebuilds (the fixed
   module is ALREADY transplanted into `~/aros-build/.../boot/darwin/L/` and
   `C:ExWalk` staged). NOTE: Feraille's `.git` permanently holds the old 1.6 GB
   +73 MB binary blobs (committed before the untrack); the branch is local-only,
   so an interactive-free history rewrite (e.g. `git filter-repo --path
   crates/feraille-aros-app/build --invert-paths`) is safe if John wants the
   repo slim — ask him first.

## To finish (in order)

1. **Free disk** (>15 GB ideally), then rebuild the two AROS binaries:
   ```sh
   cd ~/Source/Feraille
   cargo +nightly-2026-06-27 build -p feraille-aros-app \
     --target ~/Source/Macaros/hosted/rust/aarch64-unknown-aros.json \
     -Zjson-target-spec -Zbuild-std=std,panic_abort
   crates/feraille-aros-app/link-aros.sh        # links, STRIPS, deploys C:Feraille
   cd ~/Source/zed-aros
   cargo +nightly-2026-06-27 build -p gpui_aros_smoke --target ... (same flags)
   crates/gpui_aros_smoke/link-aros.sh
   ```
   Trap that burned this session twice: piping cargo through `tail`/`grep`
   masks failures (pipe exit = last command). Check the artifact exists
   (`target/aarch64-unknown-aros/debug/libferaille_aros_app.a`), not the exit code.
2. **If the gpufx agent didn't finish**: complete GFX1/GFX2 per its spec above,
   deploy `gpufx.library` into the image's `Libs:` and `C:GpuFxTest`.
3. **On-device battery** (single AROS instance; `graft/aros-ctl run`, desktop mode):
   - ExWalk: `aros-ctl type 'ExWalk SYS:C WALKERS 8 ROUNDS 25 EXALL\n'` →
     PASS + rc 0, log free of "went out of stack limits" / emul-handler traps.
   - Wheel: `aros-ctl wheel 1` / `wheel -1` (and `wheel 0 ±1`) over GpuiSmoke →
     rawkeys 0x7A/0x7B/0x7C/0x7D reach the app; `aros-ctl key 116/121/115/119`
     (pgup/pgdn/home/end). Then tick the two boxes in zed-aros
     `crates/gpui_aros/PORTING.md` on-device checklist.
   - Feraille: launch via `feraille.startup` **without** the `Stack` line once
     (stack self-guard must relaunch and boot normally), then with it. Verify:
     window + listing (screenshot), menus (`aros-ctl menu`/`menuup`, or CommKey
     RAmiga+N → second window), wheel scrolls the list, folder sizes fill in
     (walker re-enabled — watch for emul-handler suspends), pointer changes over
     text fields, ASL requester appears where the app prompts for a path
     (disk-usage HTML export is the known save-dialog path), dirty-rect: idle
     app ≈ no repaint churn (watch host CPU), typing/hover repaint stays local
     (no visual smearing at damage edges — if smearing appears, suspect the
     damage clip in zed-aros `renderer.rs update_clip`).
   - gpufx: `aros-ctl type 'GpuFxTest\n'` → `GPUFX: PASS` + GPU-vs-CPU timings;
     ffmpeg viewer plays video with gpufx loaded (and still without the library).
   - Screenshots: `graft/aros-ctl shot` → Feraille `screenshots/` (gitignored)
     + one good one for docs if a doc references it.
4. **Docs, same session as the green battery**:
   - zed-aros `PORTING.md`: tick verified checklist items; note the menus/ASL/
     pointer/resize additions and the damage model (renderer section).
   - Feraille `docs/features/aros-port.md`: status additions (native shell,
     dirty-rect, stack guard, walker re-enabled, gpufx consumer state);
     feature table rows (menu bar / requesters / cursor → ✅ once verified).
   - aros-aarch64 `docs/features/gpufx/README.md`: milestones GFX0..2 status
     (agent may have done this), on-device results.
   - `docs/features/cocoa-metal-display/INTERFACE.md`: one pointer line to the
     gpufx section (cm_gpu_* is a separate dlsym contract, CM_ABI stays 2).
   - Memory: update `feraille-aros-port.md` (open items shrink) and
     `MEMORY.md` line; the ENOSPC→silent-rustc-101 gotcha is worth a line in
     `aros-build-tree-layout.md` or a new memory.
5. **Loose ends**: `hosted/cocoametal/cmshader.air` is a build artifact now
   also produced into `build/` by the `cocoametal-shader` rule — consider
   untracking it; `make cocoametal-abi` + `cocoametal-gpu` + `cargo test -p
   gpui_aros` are the fast host regression trio; Feraille host `cargo check`
   + `cargo test` were green at `6826b48`.
