# Third-Party Notices

Ferail's own source is dual-licensed under **MIT OR Apache-2.0**
([LICENSE-MIT](FERAIL-LICENSE-MIT), [LICENSE-APACHE](FERAIL-LICENSE-APACHE)). A built
Ferail binary also incorporates third-party components whose licenses
require their copyright and permission notices to travel with redistributed
copies. Those notices are collected here.

This file covers the components that are **compiled or embedded into the
shipped binary**. The complete transitive Rust dependency graph (hundreds of
crates, overwhelmingly MIT and/or Apache-2.0) is pinned in
[`Cargo.lock`](https://github.com/jonx/Feraille/blob/main/Cargo.lock); to regenerate a full per-crate license listing,
run [`cargo about`](https://github.com/EmbarkStudios/cargo-about) or
[`cargo bundle-licenses`](https://github.com/sstadick/cargo-bundle-licenses)
over the workspace.

---

## GPUI and GPUI Component (Apache-2.0)

The UI framework and component library. Each is licensed Apache-2.0; the
Apache-2.0 license text is reproduced in [LICENSE-APACHE](FERAIL-LICENSE-APACHE).
Per Apache-2.0 §4(d), the upstream attribution notices are preserved below.

- **gpui**, **gpui_platform** — from the Zed editor project.
  <https://github.com/zed-industries/zed>
  Copyright © 2022–2025 Zed Industries, Inc. Licensed under Apache-2.0.
  (The `gpui` crate is deliberately licensed Apache-2.0, separate from the
  GPL-licensed Zed editor crates in the same repository.)

- **gpui-component**, **gpui-component-assets** — the UI primitives and the
  bundled icon assets. <https://github.com/longbridge/gpui-component>
  Copyright © 2024–2025 Longbridge. Licensed under Apache-2.0.

### GPL-3.0 severance (gpui → ztracing)

**The current build contains no GPL-licensed code — because this repository
severs the edge itself.** That is an active measure, not an upstream fix.

`gpui` reaches three small **GPL-3.0-or-later** crates from the Zed
repository — `ztracing` and, through it, `zlog` and `ztracing_macro` — which
would place copyleft obligations on any redistributed binary, despite `gpui`
itself being Apache-2.0. Outside Zed's own `--cfg ztracing` profiling builds
those crates are pure no-ops, so nothing is lost by removing them.

The dependency **is present in the `gpui` revision resolved here**, and since
zed `00cba838a` (2026-08-05) it is a *direct* `gpui → ztracing` edge, no
longer just `gpui → sum_tree → ztracing`. It is severed by
[`vendor/ztracing`](https://github.com/jonx/Feraille/tree/main/vendor/ztracing) — a **clean-room MIT/Apache
no-op stub** with the same public surface, written from the API contract
rather than derived from the GPL source — wired in through a `[patch]` in the
workspace `Cargo.toml`. Patching `ztracing` at the root keeps `zlog` and
`ztracing_macro` out of the graph entirely, and retired the earlier
`vendor/sum-tree` fork (a copy of Apache-2.0 `sum_tree` minus its ztracing
use, sufficient only while `sum_tree` was ztracing's sole consumer — see git
history for that crate).

> **Correction (0.2.2).** An earlier revision of this section stated the edge
> was already gone upstream, on the evidence that `ztracing` appeared nowhere in
> `Cargo.lock`. That evidence was misleading: the committed lockfile had been
> generated on a machine whose `[patch]` entries redirected `gpui` to an AROS
> fork that happened to drop `ztracing`. A normal clone re-resolved against
> upstream and pulled the GPL crates straight back in. **Do not treat the
> lockfile alone as proof of the licence surface** — verify against the
> resolved graph.

Verification, which should print nothing:

```sh
cargo tree -p ferail-gpui -i ztracing
cargo tree -p ferail-gpui -i zlog
```

Consequences worth knowing:

- When bumping the `gpui` pin, re-sync `vendor/sum-tree` against the new
  upstream sources (procedure in its README) and re-run the commands above.
  Their empty output is what keeps a redistributable binary MIT/Apache.
- Upstream tracks the same inconsistency at
  <https://github.com/zed-industries/zed/issues/55470>. It is acknowledged but
  stuck in legal — do not assume it lands on a timeline. If it does, delete
  `vendor/sum-tree` and the `[patch]` block.

Ferail's own source is MIT/Apache-2.0 regardless; this matters for the
**prebuilt binaries** published from 0.2.2 onward.

---

## Vendored crates carrying AROS support (MIT / Apache-2.0)

Three crates are vendored under `vendor/` unmodified except for additive
`target_os = "aros"` arms, and patched in from the workspace `Cargo.toml`.
AROS is not `unix`, `windows` or `wasm32`, and each of these matches
exhaustively over exactly those, so upstream does not merely lose a feature on
AROS — it fails to compile.

| Crate | Upstream version | Licence | Delta |
|---|---|---|---|
| [`vendor/tar`](https://github.com/jonx/Feraille/tree/main/vendor/tar) | 0.4.46 | MIT OR Apache-2.0 | AROS arms for `path2bytes`/`bytes2path`/`ends_with_slash`/`fill_platform_from`; AROS added to the existing symlink/permission/xattr fallbacks |
| `vendor/stacker` | 0.1.23 | MIT OR Apache-2.0 | AROS arm making stack growth a no-op (its probe is libc `mmap`/`mprotect`) |
| `vendor/filetime` | 0.2.26 | MIT OR Apache-2.0 | AROS arm reading times from std `Metadata`; set-times reported unsupported |

Each keeps its upstream `LICENSE-MIT` and `LICENSE-APACHE` as shipped, and each
behaves byte-identically to upstream off AROS — the arms are additive, no
existing arm is modified. `vendor/stacker` and `vendor/filetime` originate in
`zed-aros/vendor-aros`, where the AROS work was done; they are copied here so
Ferail's build does not depend on a sibling checkout for crates every platform
links.

> **Do not move these into the AROS-only patch file.** They differ in version
> from the registry, and cargo drops rather than downgrades to such a patch the
> moment a host command re-resolves the lock — leaving the AROS build broken in
> a way that looks like a missing toolchain.

## LHA / LZH decoding — `delharc` (MIT / Apache-2.0)

`.lha` / `.lzh` support links [`delharc`](https://github.com/royaltm/rust-delharc)
unmodified from crates.io. Pure Rust, no C, and taken with
`default-features = false` so its `std` feature (which enables `chrono/clock`)
stays off. Decoder only — Ferail cannot create LHA archives, which the
capability matrix reflects.

---

## Tree-sitter and its grammars (MIT / Apache-2.0)

The inline syntax-highlighted code preview embeds **tree-sitter** and 35
grammar crates, all compiled into the shipped binary. Their licenses require
the copyright and permission notices to travel with redistributed copies.

- **tree-sitter** — <https://github.com/tree-sitter/tree-sitter>
  Copyright © 2018–2025 Max Brunsfeld and contributors. Licensed under MIT.
- **Grammar crates** — 32 are MIT, 2 are `MIT OR Apache-2.0`, and 1 is
  Apache-2.0; `tree-sitter-graphql` (Copyright © 2025 Joohwan Oh) ships a
  `LICENSE` file rather than a manifest `license` field, and is MIT. Each
  grammar carries its own copyright holder — the authoritative per-crate
  list is pinned in [`Cargo.lock`](https://github.com/jonx/Feraille/blob/main/Cargo.lock) and can be regenerated with
  `cargo about` or `cargo bundle-licenses`.

The grammars reach the build transitively through `gpui-component`; the set
changes when that pin moves, so re-check this section on a rev bump.

---

## Icon artwork

Ferail embeds 53 SVG glyphs in `crates/ferail-gpui/resources/icons/` and
references the `gpui-component-assets` icon bundle at runtime. Their provenance
and per-glyph mapping are catalogued in
[docs/features/ICONS.md](https://github.com/jonx/Feraille/blob/main/docs/features/ICONS.md).

### Lucide (ISC License)

Most embedded glyphs, and the upstream `gpui-component-assets` bundle, derive
from [Lucide](https://lucide.dev). Local copies are re-saved at
`stroke-width="1.75"`; the artwork is unchanged.

```
ISC License

Copyright (c) 2020, Lucide Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### Bootstrap Icons (MIT License)

One glyph (`resources/icons/nav/cloud.svg`) is from
[Bootstrap Icons](https://icons.getbootstrap.com).

```
The MIT License (MIT)

Copyright (c) 2019-2024 The Bootstrap Authors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### Apple system icons (not redistributed)

On macOS, folder and file-type artwork is fetched at runtime from the system
via `NSWorkspace`/`IconForFile`. This Apple artwork is **never bundled or
redistributed** with Ferail — it is read from the user's own OS at display
time — so no Apple artwork ships in the binary.

---

## libmpv (optional video player — not bundled)

The optional `mpv` build feature (off by default) plays video through
**libmpv** (LGPL-2.1-or-later / GPL depending on build). Ferail does **not**
link, bundle, or redistribute libmpv: when the feature is enabled it loads a
**user-installed** libmpv at runtime via `dlopen`/`LoadLibraryW` from a path the
user supplies (or a system install such as Homebrew's). A default Ferail build
contains no mpv code at all. If you distribute a binary built `--features mpv`,
note that video playback relies on a separately-installed libmpv that Ferail
does not ship.
