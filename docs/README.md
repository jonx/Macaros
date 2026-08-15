# Macaros documentation

Start with the root [README](../README.md) to run Macaros and
[Getting started](../GETTING-STARTED.md) to build it.

## Current documentation

- [Feature index](features/README.md) — subsystem status, design, specifications,
  and verification.
- [Release guide](../RELEASE.md) — build, test, sign, notarize, and publish.
- [Collaborator guide](project/COLLABORATOR-GUIDE.md) — repository ownership and
  working rules.
- [Repository strategy](project/REPOSITORY-STRATEGY.md) — boundaries between
  Macaros, the AROS fork, Zed, and Ferail.
- [Project backlog](project/BACKLOG.md) — current cross-cutting follow-up work.

## Historical and porting material

- [Initial platform bring-up](hosted/initial-platform-bringup/README.md) — the
  runnable QEMU AArch64 kernel, hosted macOS spikes, roadmap, and graft notes
  that established the project. This is primarily useful when adding another
  AROS platform.
- [Architecture and decision log](history/DECISION-LOG.md) — chronological
  decisions and debugging evidence.
- [Historical handoffs](history/handoffs/) — dated snapshots retained for
  context, not current instructions.

The implementation-side guides remain beside their code in
[`graft/`](../graft/README.md) and [`hosted/`](../hosted/README.md).
