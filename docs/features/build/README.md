# Build workflow — Darwin/aarch64 hosted AROS

Companion to [deployment/](../deployment/README.md). That doc covers **deploy +
run** of already-built artifacts; this one covers **compiling AROS from source**
for the darwin-aarch64 hosted target — and, more importantly, the traps that make
people "redo the whole thing again." Read both: build produces the modules,
deploy stages and runs them.

> The OS source is a **separate** checkout at `../aros-upstream` (branch
> `aarch64-darwin-graft`). You build from there, commit there, and push to the
> jonx fork (remote `fork`). This repo (`aros-aarch64`) is only the host/graft
> layer.

---

## TL;DR — the reliable recipe

**One command** rebuilds the whole boot + desktop set into the stable tree,
reusing the preserved toolchain (this is also the recovery tool after the macOS
`/tmp` cleaner or a disk-full purge eats a tree):

```sh
graft/rebuild-aros.sh            # boot module set + desktop userland -> ~/aros-build
# TARGETS="kernel-dos kernel-intuition" graft/rebuild-aros.sh   # a subset
```

It builds each metatarget in its own `make` call (avoids the mmake stale-DB
failure), logs per target under `~/aros-build/rebuild-logs/`, continues past
individual failures with a summary, writes the `AROS.boot` signature, and sets
up the `objcopy` shim + PATH. It never runs a bare `make` and never rebuilds
LLVM. If `~/aros-build` isn't configured yet, it prints the one-time configure
line to run first.

<details><summary>What that automates (the manual recipe)</summary>

```sh
# 1. Stable locations (NOT a session scratchpad, NOT /tmp) + reuse the toolchain
BUILD=~/aros-build ; XT=~/aros-crosstools
ln -sf "$XT/bin/llvm-objcopy" "$XT/bin/objcopy"        # macOS lacks objcopy; stable shim
export PATH="$XT/bin:/opt/homebrew/bin:$PATH"

# 2. Configure ONCE — REUSE the toolchain (skip the ~1-2h LLVM build), no X11
cd "$BUILD"    # (rm -rf "$BUILD" first only for a truly clean reconfigure)
../aros-upstream/configure \
    --target=darwin-aarch64 --with-toolchain=llvm \
    --with-aros-toolchain=yes --with-aros-toolchain-install="$XT" \
    --without-x
#   -> must print "checking whether to build crosstools... no"

# 3. Build the BOOT MODULE SET + desktop via explicit metatargets (NOT a bare `make`)
make kernel-exec kernel-kernel kernel-dos kernel-dosboot kernel-utility \
     kernel-intuition kernel-graphics kernel-layers ... \
     workbench-libs-muimaster ... kernel-hidd-cocoa kernel-bootstrap-hosted \
     workbench-c workbench-system-wanderer workbench-classes-zune ...
```
</details>

The four rules that save you days are: **(1)** build in a stable directory,
**(2)** reuse the toolchain, **(3)** never run a bare `make`, **(4)** build module
metatargets, not the `kernel`/`workbench-libs` aggregates. `rebuild-aros.sh`
encodes all four.

---

## 1. Build in a STABLE directory

The single biggest time sink: building inside an **ephemeral session scratchpad**
(a `/private/tmp/.../scratchpad/arosbuild` tree). When the session ends
that tree is garbage-collected — and the GC is *partial*: it removes `Makefile`,
`mmake.config`, `config.status`, the root `gen/`, and even **strips the clang
resource headers** (`stdarg.h`, `stddef.h`, …) out of the toolchain. The leftover
binaries still look present, so the next `make` runs against a half-deleted tree
and produces a Frankenstein kickstart → `can't open dos.library v36` → `dosboot`
guru. *This is the "I keep having to redo it" loop.*

**`/tmp` is not stable either.** The macOS periodic maintenance job deletes
`/tmp` files untouched for ~3 days; over a multi-day gap it gutted
`/tmp/arosbuild` and left the same half-deleted-tree symptoms (2026-07-04:
`Libs/` down to 3 libraries, `C:` empty, kickstart reduced to `exec.library` +
`kernel.resource`). The same fate awaits `/tmp/aros-crosstools`.

Use a home location: **`~/aros-build`** for the build and **`~/aros-crosstools`**
for the preserved toolchain (both are the defaults of
`graft/build-darwin-aarch64.sh`, and `run-window.sh`/`aros-ctl` search
`~/aros-build` first; `/tmp/arosbuild` remains a legacy fallback).

## 2. The cross-toolchain — do NOT rebuild it (~1–2 h)

There are two toolchain stories; only one works:

| Approach | Result |
|---|---|
| Thin wrapper around Homebrew clang (retarget system clang to aarch64 ELF) | **Dies at `-noposixc`** — Homebrew clang isn't the AROS-patched clang. A dead end; do not try it. |
| AROS's own from-source patched **clang 20.1.0** (`--with-toolchain=llvm`) | Works — but building it from source is the ~1–2 h step you want to avoid. This is what `graft/build-darwin-aarch64.sh` and the recipe below do. |

The working toolchain is self-contained and **relocatable** (only depends on
system libs + Homebrew `zstd`). Preserve and reuse it instead of rebuilding:

```sh
cp -a <build>/bin/darwin-aarch64/tools/crosstools ~/aros-crosstools
~/aros-crosstools/bin/clang --version   # -> clang 20.1.0, Target: aarch64-unknown-aros
```

Then point `configure` at it with `--with-aros-toolchain=yes
--with-aros-toolchain-install=$HOME/aros-crosstools`. Confirm configure prints
**`checking whether to build crosstools... no`**. Keep the copy in `$HOME`,
not `/tmp` — the tmp purge (§1) eats it too.

**Guard:** a bare `make` (default target) will still try to *build* the toolchain
from source. The crosstools mmakefile now **refuses** unless you opt in:

```
make AROS_ALLOW_LLVM_REBUILD=1 <target>   # only if you really mean it
```

Without that variable it prints a "REFUSING to build LLVM from source" banner and
exits — so nobody triggers the 1–2 h build by accident.
(`tools/crosstools/llvm/mmakefile.src`.)

**Stripped resource dir?** If you see `fatal error: 'stdarg.h' file not found`,
the GC ate the compiler's builtin headers. **Do NOT blindly copy them from
Homebrew clang** — this is a silent footgun. Homebrew is now clang **22**, whose
freestanding `stdarg.h` defines `va_start` via `__builtin_c23_va_start`, a
builtin the crosstools' clang **20** binary does not have. clang-20 then treats
every `va_start` as an implicit call to a *nonexistent* function — a **warning,
not an error**, so the build still succeeds — and all varargs are silently
broken at runtime. The whole system "builds" but the boot path faults with no
useful diagnostic. Restore the **matching clang-20.1.0** freestanding headers:

```sh
# the two that actually differ and bite: __stdarg_va_arg.h (the va_start macro)
# and float.h. The rest of the freestanding set is version-stable.
XT=~/aros-crosstools/lib/clang/20/include
for h in __stdarg_va_arg.h float.h stdarg.h stddef.h ; do
  curl -fsSL "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-20.1.0/clang/lib/Headers/$h" -o "$XT/$h"
done
```

The robust fix is to **preserve the toolchain** (copy `crosstools` to
`~/aros-crosstools`, §2 top) so the headers never get stripped in the first
place — then you never reach for Homebrew's mismatched set.

## 3. Build module metatargets — never a bare `make`

A bare `make` builds the *default* (full distribution) target, which:
- pulls in the **toolchain build** (see the guard above), and
- breaks on **darwin-incomplete, non-essential modules** that abort the run.

Build the **boot module set** with explicit metatargets instead. They use the
prebuilt compiler directly and never touch the toolchain build.

The boot set (47 modules, from the kickstart `AROSBootstrap.conf` + the list
`run-window.sh` appends) maps to metatargets roughly as:

- `rom/<x>` → **`kernel-<x>`** (e.g. `kernel-dos`, `kernel-kernel`,
  `kernel-dosboot`, `kernel-utility`, `kernel-aros`, `kernel-oop`,
  `kernel-intuition`, `kernel-graphics`, `kernel-layers`, `kernel-keymap`,
  `kernel-debug`, `kernel-bootloader`)
- `rom/devs/<x>` → `kernel-<x>` (`kernel-console`, `kernel-input`,
  `kernel-keyboard`, `kernel-gameport`, `kernel-clipboard`)
- `rom/hidds/<x>` → `kernel-hidd[-<x>]` (`kernel-hidd`, `kernel-hidd-gfx`,
  `kernel-hidd-input`, `kernel-hidd-kbd`, `kernel-hidd-mouse`)
- `workbench/libs/<x>` → `workbench-libs-<x>` (`-muimaster`, `-cybergraphics`,
  `-datatypes`, `-gadtools`, `-iffparse`, `-locale`, `-asl`, `-commodities`,
  `-coolimages`, `-rexxsyslib`, `-stdc`)
- the Cocoa display HIDD → **`kernel-hidd-cocoa`**
- the host bootstrap (`AROSBootstrap`) → **`kernel-bootstrap-hosted`**

Resolve any target you're unsure of:

```sh
grep -rhoE 'mmake=[a-z0-9-]+' ../aros-upstream/rom/dos/mmakefile.src
```

> `kernel` (the aggregate) is **narrow** on hosted — it builds only the
> low-level support modules (`exec`, `expansion`, `timer.device`,
> `hostlib.resource`, `processor.resource`, `unixio.hidd`, `emul-handler`). It
> does **not** build `dos.library` / `kernel.resource`. Don't rely on it.

> **mmake gotcha:** if `make` rebuilds `mmake` itself mid-run (e.g. you touched
> all source to force a recompile), the metatarget DB goes stale and the *next*
> target in the same invocation fails with `[MMAKE] Nothing known about project
> kernel-kernel`. Workaround: build **one target per `make` call** in a loop
> (each call reloads the DB), or run `make` once to settle, then the real build.

### 3b. The full desktop needs more than the kickstart

The 47-module kickstart boots to a **CLI**. The Wanderer **desktop** additionally
needs the on-disk userland, and the chain is unforgiving — each missing piece is
a *different* dead end:

- **C: commands** — `make workbench-c` (~119 commands). The boot runs
  `C:AROSMonDrvs` to load display drivers; stale/missing → the screen never opens.
- **Userland libraries** — `make workbench-libs-<x>`. The one that bites first is
  **`icon.library`**: `AROSMonDrvs` opens it to enumerate `DEVS:Monitors`, and
  without it **no monitor loads** → the display check fails → `Display driver(s)
  failed to initialize. Entering emergency shell.` Others needed for a clean
  desktop: `kms`, `lowlevel`, `diskfont`, `reaction`, `workbench`, `version`,
  the `mathieee*` set.
- **Wanderer + classes** — `make workbench-system-wanderer workbench-classes-zune
  workbench-libs-workbench`, plus `workbench-datatypes-picture`/`-png`, plus the
  Cocoa display HIDD/monitor `make kernel-hidd-cocoa`.
- **Tools-menu utilities** — `make workbench-utilities-clock
  workbench-utilities-multiview workbench-utilities-more`. Wanderer's Tools menu
  launches `SYS:Utilities/{Clock,MultiView,More}`; a stale one crashes on launch
  (old binary vs the merged `stdc.library` -> SIGSEGV in `__stdc_program_end`).
  These live in `workbench/utilities/`, so they are NOT covered by `workbench-c`
  or the `.userland-targets` set -- rebuild them explicitly with the desktop set.
- **Workbench apps (Tools/, System/, Prefs/)** — the icon set is staged
  wholesale, so every app you do not build is a **dead icon** on the desktop
  (`.info` with no binary behind it). Upstream builds them all through the
  `workbench` aggregate (each app dir hooks in via a `#MM- workbench :` line),
  which we cannot run on darwin; the cherry-picked equivalents live in
  `rebuild-aros.sh` `APP_TARGETS` (`workbench-tools` = Calculator/GraphicDump/
  InitPrinter/PrintFiles, plus `-ahirecord`, `-hdtoolbox`, `-installaros`,
  `-sysexplorer-app`, `workbench-devs-diskimage-gui`, `workbench-prefs-boot`,
  the `external-openurl-*` set, `kernel-usb-trident`). Two traps:
  - The `workbench-system` **aggregate is broken** (`workbench-system-vmm-app`
    fails on missing generated locale strings, and VMM is pointless hosted).
    Its own two files (`System/FixFonts`, `System/CLI` — the target of the
    `Shell.info` project icon) have no standalone metatarget;
    `rebuild-aros.sh` builds them by invoking the directory's generated
    mmakefile directly (`build_workbench_system_base`).
  - **The 64-bit taglist trap** (FIXED for SysExplorer + Zune GUI Settings,
    2026-07-16, upstream 303db32b + 471cb63f): a **bare C int as a taglist
    tag or value in a variadic MUI call**. Arguments in x1-x7 are safe
    (w-register writes zero-extend), but one spilled to a stack vararg slot
    is stored as 32 bits and read back as 64 -- the stale upper half turns
    0/TRUE into a garbage pointer like `0x100000000` (fault addresses with
    only high-half bits set are this bug). Fix = cast the tags and values
    to IPTR at the offending call site. The safe SDK variadic macros
    (`MUIMASTER_YES_INLINE_STDARG`) are NOT a wholesale fix: they break the
    `VGroup ... End` idiom (the closing paren lives inside `End`, invisible
    to macro argument scanning). Expect more of these lurking in other
    32-bit-heritage Zune contribs.

Rule of thumb: **rebuild the whole desktop set together** after any
genmodule/startup/ABI change. A mixed tree (kickstart new, userland old) yields
the misleading `can't open dos.library` / volume-requester / emergency-shell
symptoms even though every individual module "built fine."

## 4. Permanent fixes already in the source (don't re-discover these)

These were the recurring "we keep fixing it every build" items; they now live in
the source so a clean regenerate keeps them:

- **`KOBJ_LDFLAGS` / `--allow-multiple-definition`**
  (`config/make.cfg.in` + `config/make.tmpl`). The weak `__aros_libreq_<base>`
  version marker is emitted by clang as a *non-weak* block-scope static, so it
  collides across a module's objects. The fix is `--allow-multiple-definition`,
  but the kickstart **kobj** link is a raw `$(AROS_LD) -Ur` that uses
  `USER_LDFLAGS`, **not** `LDFLAGS`. Putting the flag only on `LDFLAGS` (the
  compiler-driver path) was the trap: regular modules linked, `kernel.resource`
  never did. There's now a dedicated `KOBJ_LDFLAGS` hook.
- **`--without-x`** — the X11 HIDD is pointless on darwin (Cocoa is the display)
  and its host tool `makexkeytable` can't link against a real libX11 here.
- **`-mcmodel=large` + `-D__arm64__`** (aarch64 block in `make.cfg.in`) — needed
  so weak-symbol access doesn't route through a GOT the kickstart loader lacks,
  and so macOS host headers take the `__arm64__` path.
- **genmodule must NOT emit a weak `SysBase`** (`tools/genmodule/writestart.c`).
  An in-flight change once added `struct ExecBase *SysBase __attribute__((weak));`
  per module. Combined with `--allow-multiple-definition` (above), the linker
  then kept the **weak NULL** copy instead of the real global, so every module
  read its own NULL SysBase. `kernel.resource` crashes earliest: `krnPrepareExecBase`
  dereferences `SysBase->ex_DebugFlags` before any init sets it → SIGSEGV at the
  kernel entry, *before* `[KRN]` ever prints. Keep `SysBase` a normal strong
  global. Verify: `llvm-readelf -s <module> | grep -w SysBase` must say
  `GLOBAL`, never `WEAK`.
- **SIGALRM foreign-thread guard** (`arch/all-unix/kernel/kernel.c` +
  `kernel_intern.h`, darwin only). The VBlank tick is a process-directed `SIGALRM`
  (`ITIMER_REAL`); it can land on a libdispatch/Metal/AppKit worker thread
  (spawned by a Cocoa call on AROS's thread, inheriting its *unblocked* mask).
  `core_IRQ` now records AROS's host thread (`pthread_self()` in `core_Start`,
  exposed via a new `KernelInterface` entry) and **drops** any tick delivered to
  a different thread — otherwise it runs the scheduler / bumps the global
  `SupervisorCount` off-thread → `ObtainSemaphore called in supervisor mode!!!
  → Privilege violation`. This was the residual intermittent ("every other time")
  crash that survived the build fixes.

## 5. Known darwin-broken, non-essential modules (skip them)

The darwin port has no clean "build everything" path yet. A few non-essential
modules fail to build and will abort an **aggregate** target — which is the real
reason to build module metatargets instead:

| Module | Symptom | Handling |
|---|---|---|
| X11 HIDD | `makexkeytable`: undefined `_XCreateWindow`… (no libX11) | `--without-x` |
| `security.library` | link: undefined `stricmp` | not boot-critical — omit from the build set |

If you hit a new one, check whether the module is in the boot set (above). If
not, just leave it out.

## 6. Troubleshooting — symptom → cause

| Symptom | Cause | Fix |
|---|---|---|
| `make: No rule to make target 'kernel-dos'` | `Makefile`/`mmake.config` gone (scratchpad GC) | rebuild in a stable dir; re-run `configure` |
| `The configure script must be executed before running 'make'` on EVERY target | a git branch switch in the source tree touched `configure`'s **mtime** (content unchanged), so the Makefile guard thinks the config is stale | `cd ~/aros-build && ./config.status && touch config.status` (`rebuild-aros.sh` now does this automatically); a real `configure` content change still needs a real reconfigure |
| `can't open dos.library v36` then `dosboot` guru | ABI skew: modules built across genmodule/startup/struct changes | one coordinated rebuild of the whole boot set |
| `fatal error: 'stdarg.h' file not found` | stripped toolchain resource headers | restore from Homebrew clang (§2) |
| `ld.lld: duplicate symbol __aros_libreq_SysBase` | `KOBJ_LDFLAGS` not reaching the kobj link | fixed in `make.cfg.in`/`make.tmpl` (§4) |
| Suddenly building `LLVMSupport`/clang from source | configured without `--with-aros-toolchain=yes` | the guard now blocks it; reuse the toolchain (§2) |
| SIGSEGV at kernel entry, **before** `[KRN]` prints | weak `SysBase` (genmodule regression) → kernel reads NULL | keep `SysBase` GLOBAL; `readelf -s` to verify (§4) |
| SIGSEGV in `kernel.resource` startup, **before** `[KRN]` (fault `0xfffffffffffff480`) | hosted kickstart `memset`/`memcpy` bound to the weak `-lstdc` StdCBase stub (NULL StdCBase early) — `-lstdc.static` missing from the general `LDFLAGS` | `-lstdc.static` in `config/make.cfg.in` `LDFLAGS` (NOT just `KERNEL_LDFLAGS` — hosted kickstart is compiler=target). Verify `llvm-nm kernel.resource \| grep -w memset` is `T`, not `W`/`__memset_StdCBase_wrapper` |
| Builds clean but the boot faults with no diagnostic | clang‑22 `va_start` header compiled by the clang‑20 binary | restore clang‑20.1.0 freestanding headers (§2) |
| `[MMAKE] Nothing known about project kernel-kernel` | mmake rebuilt itself mid-run; stale metatarget DB | one target per `make` call (§3) |
| `make <target>` "succeeds" instantly but installs nothing | **typo'd metatarget: mmake exits 0 on an unknown target** (`[MMAKE][0] Nothing known about target X` is not an error). Cost a full debug cycle: `kernel-clipboard`/`workbench-libs-stdc`/`workbench-libs-cybergraphics` were silent no-ops (real names `workbench-devs-clipboard`, `compiler-stdc`, `workbench-libs-cgfx`) | grep the make output for `Nothing known about target` after adding any new target name; resolve names via `grep -rhoE 'mmake=[a-z0-9-]+' <srcdir>/mmakefile.src` |
| Wanderer requester `Could not open … "muimaster.library"` (deps all on disk) | one of Wanderer's hard deps is truly absent — commonly `cybergraphics.library` (target is `workbench-libs-cgfx`, NOT `-cybergraphics`) or `stdcio.library` (`compiler-stdcio`; without it every C: command is also silently mute) | `version <lib>.library` from the boot shell bisects it: "object not found" = missing lib |
| `Display driver(s) failed… Entering emergency shell` | `icon.library` missing (no monitors load) **or** no `AROS.boot` signature | build the userland libs (§3b); stage `AROS.boot` (deploy doc) |
| `Please insert volume "THEMES"` requester | AROSDefault theme not staged | `run-window.sh` stages it now; see deploy doc |
| `ObtainSemaphore called in supervisor mode!!!` (intermittent) | VBlank `SIGALRM` delivered to a foreign host thread | SIGALRM guard in `core_IRQ` (§4) |
| Random guest wedges/corruption under input load (e.g. window resize freezes the app; no `[KRN]` trap) | **stale pre-`-ffixed-x18` objects**: a `make.cfg` flag change does NOT invalidate `.o` files, so modules built before the flag keep allocating x18, which macOS zeroes on any signal/kernel entry | `find gen -name '*.o' -not -path '*tools*' -delete`, rebuild the full metatarget set; verify with `graft/deploy-check` (x18 section; clean modules have ≤4 x18 refs). Saving x18 in the context can't help — the host has already clobbered it before our handler runs (`hosted/x18probe`) |

## 7. After changing AROS source — commit and push to the fork

The build fixes live in `../aros-upstream` on branch `aarch64-darwin-graft`.
Commit them there and push to the jonx fork (the off-machine backup and where
we publish from):

```sh
cd ../aros-upstream
git push fork aarch64-darwin-graft
```
