# Macaros release guide

This is the reproducible path from the development checkouts to a public
Apple Silicon disk image. The release artifact is self-contained: it must not
depend on the build tree, source checkout, Homebrew, or Xcode at runtime.

## Public-release gates

Do not publish a candidate until every item below is resolved.

- Build from named, clean commits of this repository, the AROS fork, Zed,
  Ferail, and the Rust/toolchain dependencies. Record those commits in the
  release notes.
- Publish the exact corresponding source for the bundled GPL Zed binary. The
  current local Zed port has commits not present on its remote, so a binary from
  that checkout is not yet a public-release input.
- Generate and review licence/notice output for every bundled application and
  its dependency graph. Keep the generated reports and licence texts under
  `notices/` in both the app and disk image.
- Build Ferail and the other bundled apps from a clean, pinned tree; do not copy
  an unexplained binary out of a mutable development image.
- Run the `x18` section of `graft/deploy-check` against every AArch64 payload.
  Macaros is hosted under Darwin, where Apple reserves `x18` and signal delivery
  does not preserve a guest value. All C/C++ objects need `-ffixed-x18`, Rust
  code needs the checked-in `+reserve-x18` target, and assembly must avoid it.
- A fully signed build additionally requires the Apple notarytool Keychain
  profile `D4Mac`, matching the shared signing reference in the parent source
  directory. The interim outer-only build cannot pass notarization while its
  embedded host code remains ad-hoc.

These are provenance and distribution gates, not boot-test gates. An unsigned
internal candidate can still be built and tested while they are being closed.

Macaros tests our own hosted AROS and application-platform concepts. It does not
target bare-metal Apple Silicon, and a native hardware port is not planned. See
[RELEASE-NOTES.md](RELEASE-NOTES.md) for the user-facing scope and ABI note.

## 1. Prepare the runtime tree

Build the AROS fork and host libraries, deploy them, and make one successful
desktop boot so all required payloads are staged:

```sh
graft/build-darwin-aarch64.sh
make cocoametal-dylib pasteboard-dylib coreaudio-dylib bsdsock-dylib emu68k-dylib
graft/aros-ctl deploy
graft/deploy-check
AROS_CTL_STARTUP_MODE=desktop graft/run-window.sh
```

The release builder intentionally copies only runtime directories. It excludes
the developer SDK, test disk images, temporary/user configuration, loose test
programs, and crash reports. Its audit fails if a memory snapshot or another
known private artifact crosses that boundary.

## 2. Run the pre-release tests

At minimum:

```sh
make cocoametal-abi cocoametal-shell cocoametal-statusbar
graft/startup-loop 3
graft/desktop-smoke
graft/hostvol-smoke
graft/clipboard-smoke
```

Also launch Zed, Ferail, and FFView from the desktop; verify sound,
network access, clipboard exchange, window resize, and a write through `MacRW:`.

Moonstone is not part of the default or public candidate. A private build can
restore it without changing the script:

```sh
MACAROS_INCLUDE_MOONSTONE=1 MOONSTONE_SRC=/path/to/assets \
  graft/make-aros-release.sh
```

Before distributing such a private variant, separately confirm permission to
redistribute its data, graphics, and soundtrack and add its notices.

## 3. Set the release version and build an unsigned candidate

`VERSION` is the single source of truth for the app bundle, About window, disk
image name, and build manifest. Update it with:

```sh
graft/set-release-version 0.2.2 1
graft/make-aros-release.sh
graft/make-aros-release.sh --check
```

Increase the build number when rebuilding the same public version. Do not edit
the version string in the app source or packaging scripts.

For a public candidate, make dirty or missing source checkouts fatal:

```sh
MACAROS_REQUIRE_CLEAN_SOURCES=1 graft/make-aros-release.sh
```

The app embeds `Documentation/BUILD-MANIFEST.txt` with source revisions, clean
or dirty state, repository URLs, and SHA-256 hashes of the principal binaries.
This records provenance; it does not by itself prove that a staged binary was
built from the named revision, so retain the component build logs as well.

The release audit requires both `AROS/Libs/emu68k.library` and its host-side
`Frameworks/libemu68k.dylib`; a package with only one half fails the build. The result
is `build/Macaros.app`. The `--dmg` form also creates the unsigned
`build/Macaros.dmg` delivery image. Copy the candidate to a directory outside
the source and build trees, launch it there, and confirm that it boots without those trees.
Test once with a fresh macOS user account as well; this catches dependencies on
existing files under `~/Library/Application Support/AROS` or `~/AROS/Shared`.

## 4. Sign and build the interim delivery image

Store credentials once (Apple ID, team ID, and an app-specific password):

```sh
xcrun notarytool store-credentials D4Mac
```

Only after John has tested and approved the exact unsigned image, run:

```sh
export MACAROS_SIGN_IDENTITY='Developer ID Application: Name (TEAMID)'
export MACAROS_SIGN_SCOPE=outer
graft/sign-macaros-release.sh --package-test
```

The current `outer` scope signs the `Macaros.app` bundle with Developer ID and
hardened runtime but preserves the build-time ad-hoc signatures on the hosted
AROS executables and bridge libraries. It then builds and signs the delivery
DMG. This scope cannot pass Apple notarization; the result is a signed but
unnotarized `build/Macaros.dmg`.

This is an interim arrangement. AROS switches tasks through Darwin signal
contexts, and enabling hardened runtime on the hosted engine stops the system
during `dos.library` bootstrap. `MACAROS_SIGN_SCOPE=full` signs every host Mach-O
with hardened runtime and is retained for testing a future portable scheduler
correction. When that work is ready, set the `D4Mac` notary profile and use
`--notarize`. Do not use the full scope for a release until the exact signed
image passes the complete boot and application test.

For an internal Developer-ID-signed candidate without notarization, use
`--package-test`. Use `--sign-only` when only the app bundle is required.

## 5. Verify the exact upload

Mount the final DMG on a clean Apple Silicon Mac. Run **Check Macaros
Compatibility.command**, drag Macaros to Applications, launch it, and repeat the
desktop/application smoke. Finally verify the signature and record checksums:

```sh
codesign --verify --strict --verbose=2 build/Macaros.app
codesign --verify --verbose=2 build/Macaros.dmg
shasum -a 256 build/Macaros.dmg
```

Keep the release manifest, checksums, notarization submission ID, exact source
revisions, notices, and smoke-test result with the GitHub Release.
