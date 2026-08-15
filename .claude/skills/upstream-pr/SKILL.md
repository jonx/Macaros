---
name: upstream-pr
description: >-
  How to take finished work to the upstream AROS project
  (github.com/aros-development-team/AROS): what needs John's approval first,
  what the project itself requires, the pre-flight checks, and the mechanics.
  Use before pushing a PR branch, opening a PR, or commenting on any upstream
  PR or issue. Also use when deciding whether a piece of work is even
  PR-shaped yet.
---

# Taking work upstream

## The approval gate (read this first)

Two different rules, decided by WHERE the thing lands:

**Repos John owns (`github.com/jonx/*`: AROS, AROS-AArch64, Macaros, aros-raspi).**
Push freely. Branches, backups, force-push to his own working branches, new
repos. No need to ask. This is his workspace and keeping it backed up is a
good thing.

**Anything on `aros-development-team/AROS`.** Nothing happens without John
saying so for that specific action. That covers:

- opening a PR
- pushing a branch that is already the head of an open upstream PR
  (it updates the PR, so it is an upstream action even though the branch
  lives on the jonx fork)
- commenting on, editing, closing or reopening an upstream PR or issue
- reviewing or approving anyone else's PR

Approval is per action, not standing. "Do it, the right way" or "go ahead and
build it" authorises the WORK. It does not authorise publishing it. When the
code is ready, stop and present it (see "What to hand John"), then wait.

If in doubt: local commits and pushes to jonx are free, so do those, and let
the last step be the one you ask about.

## What the project itself requires

From `CONTRIBUTING.md` in the AROS repo root:

1. **Discuss the change before making it**, via the developer mailing list, an
   issue, or the Slack channel. This is a real step, not boilerplate. For a
   bug fix with an obvious correct answer nobody minds, but for anything that
   adds a feature, adds a setting, changes an interface, or picks between
   designs, the discussion comes first. Getting a maintainer to say "yes, that
   way" in Slack before writing code is worth more than a well-argued PR body
   after the fact.
2. **Follow the AROS coding conventions** (developers.aros.org styleguide).
   In practice: match the surrounding file, which is often older than the
   styleguide.
3. **Bump version numbers in any modules you touch, and update the README if
   applicable.** Easy to forget. Check for a `.conf` with a `version` field or
   a `version.h` next to what you changed.
4. **Two core-developer sign-offs to merge.** We cannot merge and cannot close
   issues; we can only open and comment.

## Is it PR-shaped yet?

Do not propose a PR until all of these are true:

- **It builds.** Compile-checked against **current `origin/master`**, not just
  against our branch. Fetch first; upstream moves.
- **It is verified**, with evidence you can name: a screenshot, a log line, a
  test run. If a part is unverified, that is allowed, but it must be stated in
  the PR body, not quietly omitted.
- **The branch is cut off `origin/master`** and contains only the change.
  Reconstruct commits on a fresh branch rather than cherry-picking bring-up
  history. Reconstructing has twice revealed that a "fix" was only fixing our
  own earlier bug and upstream was already correct.
- **Every commit has been read with `git show --stat`**, not just the files you
  meant to touch. A stray one-line change to an unrelated file once shipped a
  debug flag to every AROS user (PR #890 turned on `-DDEBUG=1` for pcixhci;
  #904 undid it).
- **No local build workarounds** rode along: `--allow-multiple-definition`,
  CROSSTOOLSDIR paths, `-lclang_rt`, forced `#define DEBUG 1`, commented-out
  metatargets. Keep upstream's link lines.
- **No bring-up instrumentation**: register dumps, self-test ladders, scan
  loops, unconditional `bug()` calls on the happy path. Real failures may
  print; success must be quiet.
- **Cross-repo dependencies are known.** Catalog directories (`Catalogs/`,
  `catalogs/`) are separate translation-team submodules. A new localised
  string is a two-repository change and cannot land in one PR. Check
  `.gitmodules` before designing a UI.

## What to hand John

When it is ready, give him one short message he can answer yes or no to:

- the branch name, and what it sits on
- one line on what the change does and why
- the diffstat, and the file list if it is small
- the evidence: what you tested and what you could not test
- the **full PR title and body text**, so he sees exactly what will be
  published in his name
- anything a maintainer might push back on, said plainly

Then stop. Do not push, do not open anything.

## Mechanics once approved

```sh
# branch is already committed in a worktree off origin/master
git push -u fork <branch>
gh pr create --repo aros-development-team/AROS \
  --base master --head jonx:<branch> \
  --title "<subsystem>: <what it does, lower case, no full stop>" \
  --body "$(cat <<'EOF'
...
EOF
)"
```

Link related PRs to each other with a short comment on each. If the PR closes
a tracker issue, end the body with `Fixes #<n>`.

## House style for anything published

These are absolute and apply to commits, PR titles, PR bodies and comments:

- **No Claude/AI mention anywhere, including commit trailers.** No
  `Co-Authored-By`, no "Generated with". The only attribution is John.
- **Name no other products or brands** in code, comments or PR text. Describe
  what the hardware or the specification requires, not what another system
  does. "Every reference stack programs this" is fine; naming one is not.
- **Comment sparingly and never as changelog.** A code comment states
  non-obvious intent. Why the change was made, and what it replaces, belongs
  in the commit message or the PR body.
- **Plain, concise English. No em-dashes.**
- Commit messages: a short lower-case subject line, then prose explaining the
  problem before the solution. Say what was observed, not what you assume.

## Writing the PR body

What maintainers have responded well to:

- Lead with the symptom, then the cause, then the fix.
- Include the diagnostic detail that proves the diagnosis, especially anything
  counter-intuitive (a request that succeeded with one flag and failed with
  another, a register that reads back wrong).
- State the blast radius: is this one board, one driver, or everyone?
- Say what you tested on, and name what you did not test.
- If you made a judgement call between designs, say so and offer the
  alternative. It invites a decision instead of an argument.
- Own your own mistakes directly when fixing them. It reads better than a
  neutral description and costs nothing.

## After opening

Watch for review comments and bring them to John rather than answering
unilaterally. Replying to a maintainer is an upstream action and needs the
same go-ahead.
