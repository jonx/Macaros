# Getting started

A newcomer's path from an empty machine to a running system. If you have never
touched this project, start here and read top to bottom. Deep dives are linked as
you go; you do not need them to get something on screen.

There are two payoffs, cheapest first:

1. **Run a classic 68k Amiga binary** on Apple Silicon — no AROS build, ~2 minutes.
2. **Boot hosted AROS** (AmigaOS) in a live Mac window — one longer build.

---

## 0. What you need

- A **Mac with Apple Silicon** (M1 or newer) running a recent macOS. This is an
  `aarch64-darwin` project; Intel Macs and Linux are not targets.
- **Xcode Command Line Tools**: `xcode-select --install`
- **[Homebrew](https://brew.sh)**.

Everything else is installed in the steps below.

---

## 1. The fast path — run a 68k Amiga program (~2 min)

This needs only *this* repo and Homebrew's LLVM. It builds `run68k`, a
68k→AArch64 JIT that runs self-contained classic-Amiga binaries natively.

```sh
brew install llvm lld
git clone https://github.com/jonx/Macaros.git    # this repo
cd Macaros
make run68k                                            # -> build/run68k
build/run68k hosted/jit68k/apps68k/bin/mandel.exe      # prints a Mandelbrot, exits 0
```

The program's stdout is yours to pipe, its exit code is the program's own `D0`,
and any fault drops a self-contained crash-bundle `.tar.gz`. More sample
programs: [hosted/jit68k/apps68k/README.md](hosted/jit68k/apps68k/README.md).
Full design: [docs/features/68k-jit/](docs/features/68k-jit/design.md).

That is the whole 68k story — no cross-toolchain, no OS build. The rest of this
page is the bigger payoff: booting real AROS.

---

## 2. Get the source — two sibling checkouts

Hosted AROS is built from **two repositories that must sit side by side**:

```
some-parent-dir/
├── Macaros/     ← THIS repo — the host / graft layer (scripts, host shims, docs)
└── aros-upstream/    ← the AROS OS source (kernel, libraries, Workbench)
```

`Macaros` is the Mac-side glue. `aros-upstream` is a fork of AROS carrying
the `aarch64-darwin-graft` branch (the native AArch64 backend + the Darwin host
port). Clone both as siblings:

```sh
cd some-parent-dir
git clone https://github.com/jonx/Macaros.git Macaros
git clone -b aarch64-darwin-graft https://github.com/jonx/AROS.git aros-upstream
```

The scripts assume `../aros-upstream` relative to this repo. If you put it
elsewhere, pass the path explicitly where the docs mention `AROS_SRC` / the
`configure` path.

---

## 3. The toolchain — you do not install it, the build produces it

There is **nothing to download and install** as a compiler. When you configure
AROS with `--with-toolchain=llvm`, the build compiles its own patched
cross-compiler from the official LLVM release tarballs:

- **Compiler:** clang/LLVM **20.1.0**, target `aarch64-unknown-aros`
- Version pinned in the OS source at `config/llvm_def`
- Fetched from `llvm-project` release `llvmorg-20.1.0`
- Linker is LLVM `ld.lld`; the binutils are the `llvm-*` tools

Two rules that will cost you a day each if you skip them:

- **Do not substitute your system clang.** A plain Homebrew/distro clang is not
  the AROS-patched one and dies at `-noposixc`. Let the build produce clang 20.1.0.
- **Keep it exactly at 20.1.0.** If you ever patch the toolchain's freestanding
  headers from a newer clang, `va_start` compiles as a call to a builtin that
  does not exist. It is a *warning, not an error*, so the build succeeds and all
  varargs silently break at runtime with no useful crash.

Building the toolchain from source is a **one-time ~1–2 hour** step. After that
the `crosstools` directory is relocatable — copy it aside and reuse it, and you
never pay that cost again (see step 4).

Host prerequisites for that first LLVM build:

```sh
brew install llvm cmake zstd autoconf automake gawk bison flex netpbm libpng gnu-sed
python3 -m pip install mako --break-system-packages
```

---

## 4. Build hosted AROS

The full recipe, the traps, and a symptom→cause table live in
**[docs/features/build/README.md](docs/features/build/README.md)** — read it once.
The essentials:

```sh
# macOS has no objcopy: expose llvm-objcopy under that name, and put Homebrew on PATH
mkdir -p /tmp/graft-tools
ln -sf /opt/homebrew/opt/llvm/bin/llvm-objcopy /tmp/graft-tools/objcopy
export PATH="/tmp/graft-tools:/opt/homebrew/bin:$PATH"

# Configure in a STABLE build dir (never a temp that gets cleaned mid-build).
# Point it at your aros-upstream checkout with an ABSOLUTE path (the build dir is
# under /tmp, so a relative ../aros-upstream would not resolve).
AROS_SRC=$(cd ../aros-upstream && pwd)     # run this from the Macaros repo
BUILD=/tmp/arosbuild
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"
"$AROS_SRC/configure" --target=darwin-aarch64 --with-toolchain=llvm --without-x
#   first run: compiles clang 20.1.0 (~1–2 h). Reuse it next time (below).

# Build the boot module set with EXPLICIT metatargets — never a bare `make`.
# (A bare make tries to rebuild LLVM and breaks on darwin-incomplete modules.)
make kernel-exec kernel-kernel kernel-dos kernel-dosboot kernel-utility \
     kernel-intuition kernel-graphics kernel-layers kernel-keymap \
     kernel-console kernel-input kernel-keyboard kernel-clipboard \
     kernel-hidd kernel-hidd-cocoa kernel-bootstrap-hosted \
     workbench-libs-stdc workbench-libs-cybergraphics
```

**Reuse the toolchain on later builds** so you skip the 1–2 h LLVM step:

```sh
cp -a /tmp/arosbuild/bin/darwin-aarch64/tools/crosstools /tmp/aros-crosstools
# then configure with:
#   --with-toolchain=llvm --with-aros-toolchain=yes \
#   --with-aros-toolchain-install=/tmp/aros-crosstools
# configure must print: "checking whether to build crosstools... no"
```

The four rules that save you days (from the build doc): build in a **stable
directory**, **reuse the toolchain**, **never a bare `make`**, build **module
metatargets** not the aggregates. The 47-module boot set boots to a CLI; the full
Wanderer **desktop** needs additional userland — see the build doc §3b.

---

## 5. Build the host shims and run it

The Mac-side host capabilities (display, clipboard, audio, sockets) are dylibs
built from *this* repo:

```sh
cd path/to/Macaros
make cocoametal-dylib pasteboard-dylib coreaudio-dylib bsdsock-dylib   # -> build/*.dylib
```

Then deploy and boot a live Cocoa/Metal window:

```sh
./graft/aros-ctl deploy                              # stage dylibs + the Cocoa monitor into the boot tree
./graft/deploy-check                                 # sanity-check every runnable copy is fresh
AROS_CTL_STARTUP_MODE=desktop ./graft/run-window.sh  # boot AROS to the Workbench desktop
```

A window titled "AROS" opens. Click it for keyboard focus and type at the shell.
`run-window.sh` with no env var boots to a CLI; `desktop` mode boots Wanderer.

Package it as a double-clickable app:

```sh
./graft/make-aros-app.sh                              # -> build/Macaros.app
```

Or drive it **headlessly** (no window-server session, no Screen-Recording
prompt) — this is how the project verifies itself unattended:

```sh
./graft/aros-ctl run                                 # boot, then type/click/screenshot from the CLI
```

If a change you made "doesn't take," it is almost always a stale copy — this port
has several runnable copies of the same artifacts. Read
[docs/features/deployment/README.md](docs/features/deployment/README.md).

---

## 6. Where to go next

| You want to… | Read |
|---|---|
| The big picture and why the project is shaped this way | [README.md](README.md) · [ROADMAP.md](ROADMAP.md) |
| Architecture decisions and the bug log | [NOTES.md](NOTES.md) |
| Build details, traps, symptom→cause table | [docs/features/build/README.md](docs/features/build/README.md) |
| Deploy/run and the "several copies" trap | [docs/features/deployment/README.md](docs/features/deployment/README.md) |
| Drive/verify AROS headlessly | [docs/features/control-harness/README.md](docs/features/control-harness/README.md) |
| The full host-feature set (built + planned) | [docs/features/README.md](docs/features/README.md) |
| How the two repos relate + the patch snapshot | [GRAFT.md](GRAFT.md) · [graft/README.md](graft/README.md) |

Welcome aboard.
</content>
