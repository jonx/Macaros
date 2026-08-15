# Macaros 0.2.1 release notes

This release corrects the 0.2.0 delivery image by bundling the host-side
`libemu68k.dylib` required by `emu68k.library`. The package audit and compatibility
checker now reject an image missing either half of legacy support. Macaros also keeps
bounded local compatibility diagnostics and exposes them through the Help menu; it
does not upload them.

The packaged desktop now backs `CLIPS:` with `RAM:Clipboards`. This makes the
AROS clipboard writable when Macaros is launched directly from its read-only
delivery image, so the Edit menu and Command-C/Command-V clipboard bridge work
without configuration or files left over from a developer installation.

The About window now reads version 0.2.1 and build 6 from the same release
identity used to generate the app bundle, disk image, and build manifest.

The DMG and outer application bundle are Developer ID signed, but this release
is not Apple-notarized. Its embedded hosted engine and bridge libraries retain
ad-hoc signatures so AROS can use its current scheduler. If Gatekeeper blocks
the first launch, Control-click **Macaros.app**, choose **Open**, and confirm
once, or use **System Settings -> Privacy & Security -> Open Anyway**.

## What this release is

Macaros is a hosted Apple Silicon release of AROS and a place to test our own
operating-system and application-platform concepts in a system people can run.
It explores a native AArch64 AROS target, explicit bridges to host services,
modern AROS applications, transparent legacy execution, safer removable-media
support, and an unattended way to build, drive, and inspect the whole system.

This is intentionally a hosted system. AROS runs as an arm64 process under
macOS; macOS owns the hardware-facing drivers and Macaros exposes those services
through normal AROS interfaces. We are not planning a bare-metal Apple Silicon
version. That would be a different project with different hardware, boot, and
driver requirements.

## Included concepts and applications

- Full Wanderer desktop in a native Cocoa/Metal window.
- Clipboard, CoreAudio, networking, shared host volumes, media decoding, and
  Metal-assisted graphics paths.
- Zed and Ferail as native AArch64 AROS applications.
- The `emu68k.library` execution layer and JIT work for a growing set of legacy
  68k applications.
- The writable exFAT handler and its `EXFAT0` DOSDriver.
- `aros-ctl`, the unattended input, capture, logging, and inspection harness.

Moonstone is not included in this release. Its developer build path remains
available through `MACAROS_INCLUDE_MOONSTONE=1`, but its binary, icon, and assets
are absent from the standard image.

## Important developer ABI rule: do not use `x18`

Any code built for the Macaros AArch64 AROS target must treat general-purpose
register `x18` as reserved and unavailable.

In AAPCS64, `x18` is the platform register and its meaning is defined by the
platform ABI. Apple reserves it on arm64 platforms. Macaros runs hosted inside a
Darwin process, so AROS code does not get to choose a different contract for
that register merely because it is guest code.

There is also a concrete hosted failure mode. Macaros preempts AROS tasks using
Darwin signals. Our `hosted/x18probe` test places a sentinel in `x18`, receives a
signal, and inspects the signal context. macOS supplies zero for `x18`, so the
original value has already been lost before the AROS signal handler can save it.
Saving and restoring more guest context cannot repair that.

Use these rules for every Macaros-targeted component:

- C and C++: compile AROS-target code with `-ffixed-x18`.
- Rust: use `hosted/rust/aarch64-unknown-aros.json`, which enables
  `+reserve-x18`; do not replace it with a generic AArch64 target.
- Assembly: never use `x18` for temporary or persistent state.
- Prebuilt AArch64 AROS objects that may allocate `x18` are not compatible and
  must be rebuilt for this target.

This restriction follows from the hosted Darwin ABI. It is one more reason the
release is described as Macaros AArch64 AROS, not as a generic bare-metal AROS
ABI.

## Current limits

- Ferail is early. It cannot yet launch programs or open files by
  double-clicking them; use Wanderer or the AROS Shell for those operations.
- Legacy 68k compatibility is early and incomplete. Confirmed programs such as
  LhA work, but programs that require missing operating-system calls or
  original Amiga custom hardware may fail.
- Zed does not yet include the complete language-server or Git integration of
  its upstream desktop releases.
- Some host-volume and settings changes require restarting Macaros.
- This release is experimental. Keep important data outside the writable shared
  volume until the relevant workflow has been tested.

The delivery DMG and outer application bundle are Developer ID signed. As an
interim compatibility measure, the hosted AROS engine and bridge libraries keep
their build-time ad-hoc signatures and run without Apple's hardened runtime.
They do not have independent Developer ID signatures. The complete host layer
will move to hardened runtime only after a portable hosted-scheduler correction
has been implemented and tested.

The exact source revisions, states, repository URLs, and artifact hashes are in
the `BUILD-MANIFEST.txt` embedded in the app. Third-party licences and generated
dependency reports are included under `Documentation/Licenses`.
