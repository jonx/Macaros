# Macaros collaborator handoff

Macaros is the product and integration repository for running AROS as a hosted
Apple Silicon Mac application. This is the short orientation for anyone already
working on the project after the repository split.

## Where work belongs

- **Macaros:** macOS host bridges, `graft/` build and release tools, the
  unattended harness, 68k compatibility integration, packaging, release notes,
  third-party notices, and product documentation.
- **[jonx/AROS](https://github.com/jonx/AROS/tree/aarch64-darwin-graft):** the
  working AROS source fork and the AArch64/Darwin operating-system changes.
- **[zed-aros](https://github.com/jonx/zed-aros):** the AROS port of Zed.
- **[Feraille](https://github.com/jonx/Feraille):** Ferail application work.
- **AROS-AArch64:** archived. It is a redirect and must not receive new work.

The old QEMU `virt` experiment is retained in `boot/`; its notes are under
`docs/hosted/qemu-virt/`. It remains useful architecture evidence, but it is not
a plan for booting directly on Apple hardware.

## Non-negotiable technical rules

Macaros is deliberately **hosted under macOS**. A bare-metal Apple Silicon
version is not planned. macOS owns the hardware-facing services; AROS reaches
them through the host bridges.

All AArch64 code built for this target must leave `x18` unused. Apple reserves
it as a platform register, and Darwin signal delivery does not preserve a guest
value there during Macaros preemption.

- C/C++: use `-ffixed-x18`.
- Rust: use `hosted/rust/aarch64-unknown-aros.json`, which reserves `x18`.
- Assembly: never use `x18`, including as a temporary register.
- Rebuild incompatible precompiled objects; do not assume generic AArch64
  objects are safe.

Macaros includes the evolving legacy 68k execution layer and the writable exFAT
driver. Changes to either need their focused tests as well as the general hosted
suite.

## Working and release discipline

1. Work in the repository that owns the change and keep sibling source states
   identifiable.
2. Run the focused test while developing, then `make hosted-test` for integration
   changes. Release work also requires the desktop, 68k, exFAT, host-volume, and
   clipboard smoke tests described in `RELEASE.md`.
3. The normal public image contains Zed and Ferail. Moonstone is excluded unless
   a private build explicitly sets `MACAROS_INCLUDE_MOONSTONE=1` and supplies its
   binary and assets.
4. Update third-party notices whenever a bundled dependency or source revision
   changes. Every published binary must correspond to clean, publicly available
   source.
5. Build and test an unsigned candidate first. Do not sign, notarize, publish a
   GitHub release, or publish the accompanying blog article until John approves
   the tested candidate.

Most importantly: **do not push, submit patches, or open a pull request against
the AROS upstream project without first discussing the exact action with John
and receiving explicit approval.** Work against John's fork or a local branch by
default. Upstreaming is always a separate decision.

Start with `README.md`, `GETTING-STARTED.md`, and `RELEASE.md`; detailed designs
and verification evidence live under `docs/features/`.
