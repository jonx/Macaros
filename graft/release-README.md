# Macaros

Macaros brings AROS—the open-source AmigaOS-compatible operating system—to an
ordinary Mac app. It runs natively on Apple Silicon and opens the Wanderer
desktop in a resizable Cocoa/Metal window.

This release is a hosted environment for testing our own AROS and application
platform concepts. AROS runs inside a normal arm64 macOS process and uses the
Mac for hardware-facing services. A bare-metal Apple Silicon version is not
planned.

## Before installing

Macaros requires an Apple Silicon Mac and macOS 12 or newer. Double-click
**Check Macaros Compatibility.command** for a local check of the processor,
macOS version, memory, Metal graphics, disk space, and app signature.

To install, drag **Macaros.app** onto the **Applications** shortcut, then open it
from Applications. This interim release is Developer ID signed but not
Apple-notarized. If Gatekeeper blocks the first launch, Control-click the app,
choose **Open**, and confirm once. On macOS versions that require it, use
**System Settings -> Privacy & Security -> Open Anyway**.

## What is included

Two applications are placed on the Wanderer desktop:

- **Zed**, a native AROS build of the Zed editor.
- **Ferail**, a file manager with search, duplicate finding, disk-usage views,
  and archive browsing.

Macaros also includes the AmigaDOS command set, the FFView media viewer,
CoreAudio sound, networking through the Mac, a shared clipboard, and 2D GPU
acceleration. Its legacy compatibility layer runs a growing set of classic 68k
applications, and the included exFAT driver can mount supported exFAT media and
disk images from AROS.

Double-click a desktop icon to launch it. Zed and Ferail are large applications
and may take a few seconds to open.

## Sharing files with macOS

Macaros creates `~/AROS/Shared`. Inside AROS the same folder appears as:

- `MacRW:` for reading and writing
- `MacRO:` for read-only access

Keep documents and settings in `MacRW:`. The AROS system volume inside the app
is read-only so updates and code signing remain safe. Macaros keeps its other
per-user state in `~/Library/Application Support/AROS`.

## Clipboard

Plain-text clipboard sharing is enabled by default. Copy text in a Mac app,
focus the Macaros window, and choose **Edit -> Paste** or press **Command-V**.
To copy from an AROS Shell, select text and choose **Edit -> Copy** or press
**Command-C**, then paste it into a Mac app normally. Macaros maps those actions
to the Amiga clipboard keys expected by AROS; no setup is required.

Compatibility diagnostics are stored locally in the `Reports` folder there. Use
**Help -> Show Reports** to inspect them or **Help -> Report a Compatibility Problem...**
for instructions. Nothing is uploaded automatically, and session traces are bounded.

## Current limits

- Zed does not yet include a language server or Git command integration.
- Ferail is at an early stage. It cannot yet launch programs or open files by
  double-clicking them; use Wanderer or the AROS Shell to launch them instead.
- Legacy 68k support is early and incomplete. Confirmed programs such as LhA
  work, but programs that need unimplemented operating-system calls or classic
  Amiga hardware may fail. Check **Help -> Show Reports** after a failed test.
- Live addition or removal of host volumes still requires restarting Macaros.
- The delivery DMG and outer `Macaros.app` bundle are Developer ID signed. The
  hosted AROS engine and bridge libraries currently retain ad-hoc signatures
  and run without Apple's hardened runtime so the hosted scheduler continues
  to work. They do not have independent Developer ID signatures, so this image
  is not Apple-notarized.

## Licence and notices

AROS is distributed under the AROS Public Licence. Bundled applications and
third-party components retain their own licences. See **LICENSE** and
**THIRD-PARTY-NOTICES.md** in this disk image. Developers should also read
**RELEASE-NOTES.md**, especially the requirement that all AArch64 AROS code for
Macaros reserve Apple platform register `x18`. Macaros is ported and assembled
by John Knipper.
