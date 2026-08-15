# Introducing Macaros: an Amiga-shaped operating system inside a native Mac app

What if an AmigaOS-compatible desktop did not arrive as a virtual machine or a
picture of an old computer, but as an ordinary application on a modern Mac?

That is Macaros. It packages AROS - the open-source reimplementation of AmigaOS - as
a native Apple Silicon application. Double-click it and the Wanderer desktop
boots in a resizable Mac window. AROS is still an operating system, with its own
tasks, libraries, devices, command line, filesystem conventions, and desktop.
But macOS owns the physical hardware and provides the services around it.

This choice is the heart of the project. Instead of reverse-engineering every
undocumented part of an M-series Mac, Macaros lets AROS reach the display through
Cocoa and Metal, audio through CoreAudio, networking through native sockets, and
the clipboard through NSPasteboard. The result is not bare-metal AROS, and it is
not pretending that an M-series Mac is a particular vintage Amiga. It is a new
native AArch64 port of AROS, hosted as a well-behaved Mac application.

## What you can do with it

Macaros boots a complete Wanderer desktop with the familiar AmigaDOS command
set. The window resizes with the AROS screen, keyboard and mouse events follow
the normal AROS input path, and the menu bar, About panel, settings, and app icon
belong naturally on macOS.

The host integration goes well beyond drawing a screen:

- A folder on the Mac appears as both `MacRW:` and `MacRO:` inside AROS, giving
  applications a deliberate read/write or read-only view of the same files.
- Text moves in both directions between the AROS clipboard and the Mac
  clipboard.
- AHI audio is backed by CoreAudio, while AROS networking uses the Mac's socket
  stack and DNS configuration.
- FFView can open images and video, with a Metal compute path accelerating the
  expensive conversion and scaling work.
- The native exFAT filesystem driver supports modern removable-media and disk
  image workflows from inside AROS.
- A native 68k-to-AArch64 JIT can run a growing set of classic Amiga programs.
  It is a compatibility layer rather than a promise that every old binary works
  today.

The delivery image also includes substantial applications. Zed demonstrates
that a modern editor stack can be brought across to AROS. Ferail brings a modern
file-manager experience, including search, duplicate detection, archive
browsing, and disk-usage views. Both are substantial modern applications, not
small demonstrations, and both run as native AArch64 AROS programs. Moonstone
experimentation remains easy to enable in private builds, but its code and
assets are deliberately not part of this release.

Together they answer an important question. Macaros is not only a kernel that
reaches a desktop; it is becoming an application platform.

## Native where it matters

“Native” can mean several different things, so it is worth being precise.

The AROS system and its AArch64 applications execute as ARM64 code. The Macaros
host is also ARM64 code and uses public macOS frameworks. There is no x86
translation in the normal path. Classic 68k software is the exception: when it
runs, the JIT translates 68k instructions to AArch64 while AROS provides the OS
environment around the program.

This architecture produces a useful division of responsibility. AROS owns the
operating-system semantics. macOS owns the hardware-facing implementation.
Small, explicit bridges connect the two. Each new bridge - display, audio,
clipboard, sockets, files - is one more host capability made available through a
normal AROS interface.

## Built to be observed

One of the less visible parts of Macaros may be its most unusual feature: the
whole system is designed to be driven and checked without someone sitting at
the keyboard.

The `aros-ctl` harness can start and stop AROS, type commands, send mouse input,
resize the display, inspect tasks, collect logs, and capture the framebuffer.
Screenshots come from Macaros's own offscreen render target, so automated tests
do not need macOS Screen Recording permission and cannot accidentally capture
another application.

That makes the project approachable as systems research, but it also points
toward something larger. The same control surfaces could turn AROS into an
embeddable subsystem: an Amiga-shaped environment that another application, a
test runner, or an agent can call into rather than merely watch.

## What is still unfinished

Macaros is a young platform. Classic 68k compatibility is substantial but
incomplete. Zed does not yet ship with language-server or Git integration.
Some host settings still take effect only after a restart. Release engineering
also matters now: every downloadable build needs pinned sources, complete
licence notices, a clean runtime image, Developer ID signing, notarization, and
tests on a Mac that has never seen the development tree.

Those limitations are part of the story, not a reason to hide it. AROS now boots
natively on Apple Silicon, reaches useful Mac services, runs large modern
applications, and can be exercised end to end by an unattended harness. The
remaining work is increasingly the work of turning a successful port into a
durable product and ecosystem.

Macaros is AROS on a Mac, but the name also describes the ambition: preserve the
compact, direct feeling that made Amiga systems distinctive, then give it a
native place on the machines people use now.
