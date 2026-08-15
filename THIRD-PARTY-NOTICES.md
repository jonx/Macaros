# Third-party notices

Macaros combines independently licensed open-source projects. The exact source
revisions and hashes of the principal binaries are recorded in the
`BUILD-MANIFEST.txt` embedded in each release. Full licence texts and generated
dependency reports are shipped beside this file in `Licenses/`.

This inventory describes the standard release containing Zed and Ferail.
Moonstone is not included in that release.

## AROS

AROS is distributed under the AROS Public License 1.1. Individual components in
the AROS source tree may instead use compatible licences including the GNU GPL
and LGPL; the upstream author and licence summaries are included as
`AROS-LICENSE-AUTHOR`, `AROS-LICENSE-GPL`, and `AROS-LICENSE-LGPL`.

- Project: <https://aros.org>
- Release source fork: <https://github.com/jonx/AROS>
- Main licence: `LICENSE` in the release documentation

The AArch64/Darwin port, exFAT driver, host integration, and release tooling are
available from the source repositories identified in the build manifest.

## Zed

The bundled Zed editor is licensed under GNU GPL version 3 or later. Some
separately identified Zed components, including GPUI, are licensed under Apache
2.0. The release includes `ZED-LICENSE-GPL`, `ZED-LICENSE-APACHE`, and the
generated target-specific Rust dependency report
`ZED-RUST-DEPENDENCIES.md`.

- Upstream project: <https://github.com/zed-industries/zed>
- AROS port and corresponding source: <https://github.com/jonx/zed-aros>

The corresponding source for the exact binary revision must be publicly
available before a Macaros release is published.

## Ferail

Ferail is dual-licensed under MIT or Apache 2.0. Its binary incorporates Rust
dependencies and artwork with their own permissive licences. The release
includes `FERAIL-LICENSE-MIT`, `FERAIL-LICENSE-APACHE`, Ferail's maintained
`FERAIL-THIRD-PARTY-NOTICES.md`, and the generated target-specific report
`FERAIL-RUST-DEPENDENCIES.md`.

- Project and source: <https://github.com/jonx/Feraille>

## Emu68

Macaros's legacy 68k execution layer uses selected, unmodified files from Emu68
under the Mozilla Public License 2.0. Those files are isolated under
`hosted/jit68k/emu68/`; the surrounding JIT engine, loader, bridges, and
independent interpreter were developed separately.

- Project: <https://github.com/michalsc/Emu68>
- Adopted revision: `305f686f84712f88c4d80d35769af5c60a4e988b`
- Licence: `MPL-2.0.txt`
- File-level provenance and boundary: `EMU68-NOTICE`

The MPL-covered source files, including any file-level modifications, are
available in the Macaros source repository.

## FFmpeg

Macaros includes statically linked FFmpeg 8.1.2 libraries for media decoding.
They were configured without GPL, non-free, or version-3 components
(`CONFIG_GPL=0`, `CONFIG_NONFREE=0`, `CONFIG_VERSION3=0`) and are distributed
under LGPL version 2.1 or later together with applicable permissive component
notices.

- Project: <https://ffmpeg.org>
- Licence overview: `FFMPEG-LICENSE.md`
- LGPL text: `FFMPEG-COPYING.LGPLv2.1`

The release build uses static libraries. Relinkable object/source materials and
the exact build scripts must remain available with the corresponding Macaros
source release so recipients can exercise the LGPL's relinking rights.

## Apple system frameworks

Macaros links to system-provided macOS frameworks such as AppKit, Metal,
CoreAudio, CoreGraphics, and Foundation. These frameworks are supplied by the
user's copy of macOS and are not redistributed in the disk image.

## Regenerating the reports

The Rust reports are generated against the custom AROS target, not the host Mac
dependency graph. Zed uses its checked-in `cargo-about` configuration and
template. Ferail uses `graft/ferail-licenses.toml` with the same template. A
release maintainer must regenerate and review both reports whenever either
application's lock file changes.
