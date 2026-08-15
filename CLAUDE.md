# Macaros - agent routing

Hosted AROS on Apple Silicon (aarch64-darwin). **This repo is the host/graft
layer only.** The AROS OS source (kernel, modules, libraries) is a *separate*
checkout at `../aros-upstream` (branch `aarch64-darwin-graft`). Edit OS code
there, not here; commit it there and push to the jonx fork (`git remote`
`fork` = github.com/jonx/AROS), which is the off-machine backup and the place
we publish from.

## Before you build or run — read first, every time

These two are the most-repeated, most-expensive mistakes on this port. The docs
already solve them; read the doc instead of rediscovering the trap.

- **Building AROS from source** → read [docs/features/build/README.md](docs/features/build/README.md) **first**.
  - NEVER run a bare `make`. It tries to rebuild the 1–2 h LLVM toolchain and
    breaks on darwin-incomplete modules.
  - Build explicit module **metatargets** (`kernel-dos`, `kernel-kernel`,
    `kernel-dosboot`, …), one target per `make` call.
  - Build in a **stable** dir (`~/aros-build`), never a session scratchpad or /tmp
    (both get GC'd half-deleted → the "I keep having to redo it" loop).
  - Reuse the prebuilt toolchain (`~/aros-crosstools`); never rebuild LLVM.
- **A fix appears not to take when you run it** → read
  [docs/features/deployment/README.md](docs/features/deployment/README.md).
  There are several runnable copies of the same artifacts (`~/lib`, the boot
  image, app settings). You are probably editing one and running another.

## Where knowledge lives — read it, do not re-derive

- **Per-subsystem design / spec / status** → [docs/features/README.md](docs/features/README.md)
  is the index (each folder has a README, then `design.md` / `spec.md` for depth).
  Start there for any feature, driver, or host-bridge task.
- **Architecture decisions and rationale** → [decision log](docs/history/DECISION-LOG.md).
- **Drive / verify the running AROS headlessly** → `aros-ctl`, the control
  harness ([docs/features/control-harness/README.md](docs/features/control-harness/README.md)).
- **The independent-work / provenance rules** for any `spec.md` →
  [docs/features/CLEANROOM.md](docs/features/CLEANROOM.md).

## Contributing upstream (PRs to aros-development-team/AROS)

**Ask before anything reaches upstream.** Pushing to John's own repos
(`github.com/jonx/*`) needs no permission. Opening a PR, updating a branch that
is the head of an open upstream PR, or commenting on any upstream PR or issue
needs John's explicit go-ahead *for that action*. Being told to build something
is not permission to publish it. **When work is ready to ship, read
[.claude/skills/upstream-pr/SKILL.md](.claude/skills/upstream-pr/SKILL.md)
first** — it carries the approval gate, the project's own CONTRIBUTING rules
(discuss the change before making it; bump module versions; two sign-offs to
merge), and the pre-flight checks.

Anything sent upstream ships as John's, clean and product-neutral:

- **Comment sparingly, and never as changelog.** Add a comment only when it is
  really needed for non-obvious intent. Do NOT explain *why the change was made*
  or *what it replaces* in the code — that rationale goes in the PR message,
  commit message, or GitHub issue. A resolved TODO usually just needs the fix.
- **Name no other products or brands** in code, comments, or PR text (fits the
  provenance posture, see [CLEANROOM.md](docs/features/CLEANROOM.md)).
- **No Claude/AI mention anywhere, commits included** — no `Co-Authored-By:
  Claude` trailer, no "Generated with" footer. The only attribution is John.

## Keep the docs alive — same turn, not later

If you discover a durable fact or a doc is wrong/stale, fix the doc in the **same
session**. Author each fact once and link to it; do not restate it in a second
place. Task lists and session notes do not belong in these docs.

**When you add, remove, or rename a `docs/features/<folder>/`** (or change what a
feature does), update the **Quick index** table in
[docs/features/README.md](docs/features/README.md) in the **same turn** — add/edit
the row (folder · one-line · the right entry file: `README.md`, else `design.md`).
A folder that exists but is missing from the index is a bug; treat it as one.
Likewise, if you add a routing target here in CLAUDE.md, make sure it resolves to
a real file before you finish.

**When you finish a user-visible feature or a notable fix, add a line to
[CHANGELOG.md](CHANGELOG.md) in the same turn** — reader-facing (what changed
and why it matters, not a commit echo), newest first, under a `## YYYY-MM-DD -
title` header (reuse the day's header if one exists). This is the Macaros repository's
public changelog; OS-side detail stays in the fork's git history. Skip it only
for pure internal churn (refactors, doc-only edits, tooling that users never
see).
