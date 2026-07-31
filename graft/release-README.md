# Macaros

AROS for Apple Silicon. A complete AmigaOS-like desktop running natively on
macOS, in a window, with two large applications on it.

Drag **Macaros.app** to your Applications folder and launch it. The first
launch may need a right-click and *Open*, because macOS is careful with apps it
has not seen before.

## The desktop

Macaros boots to the Wanderer desktop. Three application icons sit on it:

- **Zed** - the Zed code editor, built for AROS. Editing, syntax highlighting,
  project panel, and a terminal running a real AROS shell (Cmd-J opens it).
- **Ferail** - a power-user file manager with a duplicate finder, a disk-usage
  treemap, and an archive browser.
- **Moonstone** - the 1991 Amiga game, rebuilt in Rust from its own data.

Double-click an icon to start it. The editor and the file manager are large
programs and take a few seconds to load.

Everything is also reachable from a shell: the three applications live in `C:`,
so `Zed`, `Ferail` and `Moonstone` work as commands, as does the rest of the
AmigaDOS command set.

## Sharing files with the Mac

A folder on your Mac is mounted twice inside AROS:

- `MacRW:` read and write
- `MacRO:` the same folder, read only

By default the folder is `~/AROS/Shared`. Anything you drop in it shows up in
AROS immediately, and files AROS writes there appear on the Mac. Zed keeps its
settings in that folder, so they survive an update of the app.

To use a different folder, set `AROS_SHARE` before launching:

    AROS_SHARE=/path/to/folder open -a Macaros

## Also on board

Media playback (the FFView image and video viewer), audio through CoreAudio,
networking through the Mac's stack, a shared clipboard with macOS (copy in one,
paste in the other), and a resizable window that changes the AROS screen mode
with it.

## Known limits

- The editor has no language server yet, so no completion or go-to-definition.
- There is no `git` command inside AROS, so the editor's version-control
  features stay quiet.
- The game is silent: its music is decoded but AROS has no audio backend for it
  yet, so those files are left out of this build.
- The AROS volume lives inside the app bundle and is read only. Work in
  `MacRW:` or `RAM:`.

## Licence and credit

AROS is distributed under the AROS Public Licence. The bundled applications
carry their own licences. Ported and assembled by John Knipper.
