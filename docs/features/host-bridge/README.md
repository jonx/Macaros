# Host bridge — tapping the host from hosted AROS

Hosted AROS runs as a guest on a real OS (here macOS/aarch64), so a lot of AROS
facilities are really just the **host's** facility borrowed through a thin bridge:
the clock (host `time`), sockets (host BSD sockets), the clipboard (NSPasteboard),
the display (Cocoa/Metal), CPU info (`sysctl`), and now randomness (host
`arc4random_buf`).

Every one of these reaches the host the same way, through **`hostlib.resource`**
(`HostLib_Open` / `HostLib_GetInterface` / `HostLib_GetPointer`). This page is the
standard way to add a new one, and the small helper that removes the boilerplate.

## The two shapes

1. **Borrow an existing host libc function.** The host already implements it, so
   there is *no host-side code to write* — just open the host C library and resolve
   the symbol. Synchronous, simple. Examples: time (battclock), `sysctl` (processor),
   `arc4random_buf` (randomness).

2. **Write a custom host dylib.** The host API is complex, stateful, async, or
   Objective-C, so you write a `.dylib` (in [`hosted/`](../../../hosted/)) that
   exposes a plain C entry surface, then bind to it from AROS. Examples:
   [`hosted/bsdsocket`](../../../hosted/bsdsocket/) (kqueue pump),
   [`hosted/clipboard`](../../../hosted/clipboard/) (NSPasteboard),
   [`hosted/cocoametal`](../../../hosted/cocoametal/) (display/input).

Both shapes share the *open + resolve* dance. Shape 1 has nothing else. Shape 2 has
the (unavoidably custom) dylib on top.

## The helper: `<aros/hostbind.h>`

A header-only convenience layer over `hostlib.resource` (source:
`compiler/include/aros/hostbind.h` in `aros-upstream`). Two calls:

```c
#include <aros/hostbind.h>

/* (1) borrow a host libc symbol -- NULL if no host / missing */
typedef void (*arc4_buf_fn)(void *, size_t);
arc4_buf_fn f = (arc4_buf_fn)HostBind_LibcSym("arc4random_buf");
if (f) f(buf, len); else /* native fallback */;

/* (2) open a custom host dylib + bind a symbol table into a struct */
static const char *libs[] = { "libmybridge.dylib", 0 };
static const char *syms[] = { "mb_open", "mb_read", "mb_close", 0 };
struct MyIFace { int (*mb_open)(void); int (*mb_read)(void*,int); void (*mb_close)(void); };
ULONG unresolved = 0;
struct MyIFace *api = HostBind_Interface(libs, syms, &unresolved);
if (api) api->mb_open();
```

It probes the host C library by name (`libSystem.dylib`, then the `libc.so` forms),
caches the handle per translation unit, and returns `NULL` cleanly on a native build
so the caller can fall back. A worked, compilable example is in
[`hosted/hostbind/`](../../../hosted/hostbind/).

## The convention that matters most

**Expose the host facility as a *normal* AROS function** — a `posixc` function for
libc-ish things, a `.library`/`.device` for stateful ones. Then every consumer (C
code, Rust, anything) calls it the ordinary way, with **no per-consumer glue**.

The payoff is concrete. `arc4random_buf` is a plain `posixc` function, so the Rust
`std` pal calls it directly with zero glue. `bsdsocket`, by contrast, is reached via
inline library-call stubs, so Rust needs a hand-written `extern "C"` glue
(`hosted/rust/aros_net_glue.c`) to use it. Same host underneath; the difference is
purely how the AROS side is exposed. Wrap host taps as normal functions and the glue
disappears.

## What can't be standardized

The custom host dylib itself (shape 2) is real, feature-specific code. And async
bridges need more than a call: `bsdsocket` has a kqueue pump + a readiness-signal
seam so AROS tasks can `Wait()` on host I/O. `HostBind` only covers the open+resolve;
the pump stays bespoke (and could become its own reusable helper if more async
bridges appear).

## Status / scope

`HostBind` currently backs **`arc4random`** (the first user). The older bridges
(`bsdsocket`, the Cocoa HIDD, `hostcpu`) still hand-roll the dance and can migrate to
it incrementally when next touched — no rush, they work.

Across the whole AROS tree there are ~30 copies of this boilerplate, but most are
other host ports (Windows/Linux/Android/iOS/X11/SDL) that aren't ours. `HostBind` is
proposed as an AROS-wide cleanup to the maintainers (raised on Slack); we use it for
our own bridges now and share it upstream when there's agreement on the shape.
