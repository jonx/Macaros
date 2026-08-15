---
name: fix-aros-issue
description: >-
  Fix (or triage) an issue from the upstream AROS tracker
  (github.com/aros-development-team/AROS) and ship it as a clean per-issue PR.
  Use when asked to work an AROS issue number, sweep the `app/todo` auto-issues,
  or reproduce/repair an AROS bug on the hosted aarch64-darwin build.
---

# Fixing an upstream AROS issue

One issue -> one PR, each reproduced and verified on the hosted build before it
ships. The work is published as John's, product-neutral, with no AI attribution.

## Non-negotiable rules

- **No Claude/AI mention anywhere, commits included.** No `Co-Authored-By:
  Claude` trailer, no "Generated with" footer. The only attribution is John.
  Commit with `git -c commit.gpgsign=false commit` and write the message
  yourself without a trailer.
- **Comment sparingly, never as changelog.** Add a code comment only for
  non-obvious intent. Do NOT write *why the change was made* or *what it
  replaces* in the code ("use X instead of Y because..."). That rationale goes
  in the PR message / commit message / GitHub comment, not the source.
- **Name no other products or brands** in code, comments, or PR text.
- Plain, concise English. No em-dashes (John is French, non-native).
- **A wrapped-grep "no matches" is not evidence.** The shell `grep` here skips
  non-UTF-8 files silently, and 21% of `../aros-upstream` is non-UTF-8. Before
  claiming something is absent, or counting call sites, follow
  [search-evidence](../search-evidence/SKILL.md).

## Repos and trees (see docs/features/build + deployment)

- OS source: `../aros-upstream`, branch `aarch64-darwin-graft`. Edit, build, and
  commit here. Push to remote `fork` (github.com/jonx/AROS).
- Pristine upstream: `../aros-upstream-master` (a worktree; keep it at
  `origin/master` for grepping current master).
- Build tree: `~/aros-build`. Crosstools: `~/aros-crosstools`. Never rebuild LLVM.
- PR branches are cut off `origin/master` (clean, upstream-targetable), but
  `origin/master` cannot build on darwin (no host layer) so you VERIFY on the
  `aarch64-darwin-graft` tree. Confirm the touched file is identical on both
  (`git diff --quiet origin/master HEAD -- <file>`) so the diff ports cleanly.

## Workflow

1. **Triage — is it ours to fix?**
   - In-tree? Contrib apps are NOT in this repo (Scout, MUI 5, ...). If the fix
     lives in AROS-contrib, it's out.
   - Our platform? We build/run aarch64-darwin hosted. Out of reach: m68k /
     Vampire, SMP, x86 install/boot, X11/SDL, native-GPU drivers (nouveau, ata,
     ahci, intelgma...).
   - Already resolved? Grep the TODO text or symptom **tree-wide** in
     `../aros-upstream-master` (not just the original path). Watch for
     renames/moves: a file can be renamed and the TODO travel with it, so a
     missing-at-old-path result is NOT proof it's gone.
2. **Reproduce** on the hosted build with `graft/aros-ctl` (run / wait / type /
   enter / shot). Ground the bug in an observed artifact before touching code.
3. **Root-cause** in the source. Read the actual code path; do not guess.
4. **Fix** in the `aros-upstream` working tree (minimal change).
5. **Build** the specific metatarget in `~/aros-build`
   (`PATH=~/aros-crosstools/bin:$PATH; make <metatarget>`). Never a bare `make`.
   Find the metatarget name in the module's `mmakefile.src` (`mmake=...`).
6. **Verify**: `aros-ctl deploy` -> `run` -> drive the repro -> `shot`. Test
   every affected case for a behaviour change. For high-blast-radius code
   (console write path, exec, dos), also confirm no regression to normal use.
7. **Author the PR branch** off `origin/master` in a dedicated worktree
   (`git worktree add`), apply the same change, commit (no trailer), then
   **revert the working tree** in `aros-upstream` so the graft branch stays
   clean (the fix lives only on the PR branch).
8. **Ship**: `git push -u fork <branch>`, then
   `gh pr create --repo aros-development-team/AROS --base master --head
   jonx:<branch>`. Body ends with `Fixes #<n>`. No brands, no AI mention.

## Triage-only outcomes (we are not maintainers, cannot close issues)

- Already fixed: comment "the TODO is no longer present in `<file>` on current
  master ... can be closed as resolved."
- Moved: comment where it now lives ("renamed to `<file>:<line>`").
- Not our call (needs a design decision or a big refactor): post the root-cause
  analysis + proposed approach as a comment for whoever picks it up.

## Gotchas learned

- `aros-ctl type` drops `>` (the shell redirect char). To test redirection,
  write a script file host-side into `SYS:` (= `~/aros-build/bin/darwin-aarch64/
  AROS/`) and `execute` it, or craft a file and `type`/`execute` it.
- `Type` flushes in 8192-byte writes, so a byte placed at file offset 8191
  reliably splits across two `Write()`s (useful for split-sequence repros).
- The `app/todo` bot auto-files an issue per `TODO` comment on merge; the issue
  URL pins the commit it was filed at, so it looks live even after the code
  moved. Most are stale.
- If John is rebuilding in parallel, avoid concurrent `make` in `~/aros-build`
  (shared mmake DB) unless told it's fine; the single hosted instance is shared
  too (reboot to run-verify only when free).
- When the touched file DIFFERS between graft and origin/master: implement and
  verify on the graft tree, then re-apply hunk by hunk to the PR branch with
  upstream anchors, and prove equivalence by diffing the added lines of both
  patches (`git diff | grep '^+'` on each side must match).
- SYS: is the live host dir `~/aros-build/bin/darwin-aarch64/AROS/` -- files
  written there (test binaries, scripts) are visible to the RUNNING instance
  immediately, while in-memory handlers/modules stay old until reboot. Useful
  to repro a bug on the old module with a fresh test binary.
- `CLAUDE.md` is git-excluded (local-only). Don't try to commit it.
