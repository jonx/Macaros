# hostbind — sample for tapping the host from AROS

A worked, compile-checked template for adding a new host bridge on hosted AROS using
the `<aros/hostbind.h>` helper. Copy `hostbind_sample.c` and keep the shape you need.

- **Design + rationale:** [docs/features/host-bridge](../../docs/features/host-bridge/README.md)
- **The helper itself:** `compiler/include/aros/hostbind.h` in `../aros-upstream`

## The two shapes

- **`sample_host_getpid()`** — shape (a): borrow a function the host libc already
  has (`HostBind_LibcSym`). No host-side code. This is how `arc4random` and
  `battclock` work.
- **`sample_bind_host()`** — shape (b): open your own `libXXXhost.dylib` and bind a
  symbol table into a struct of function pointers (`HostBind_Interface`). Use this
  when the host API needs custom C/Objective-C glue (see `../bsdsocket`, `../clipboard`).

Both return `NULL` cleanly on a native build so the caller can fall back.

## Compile-check

```sh
./build.sh          # clang against the AROS SDK; prints OK on success
```

`<aros/hostbind.h>` reaches the SDK when the includes are staged (it ships in
`compiler/include/aros/`, installed by any `make includes` / module build). If
`build.sh` warns it's missing, build `compiler-posixc` (or `make includes`) once.

## The rule of thumb

Expose the host tap as a **normal AROS function** (a `posixc` function, or a
`.library`/`.device`), so C code and Rust call it with no per-consumer glue. That is
the whole point: `arc4random` is a plain posixc function, so Rust's `std` uses it
directly; `bsdsocket` is inline-LVO, so Rust needs a hand-written glue. Same host
underneath, different ergonomics.
