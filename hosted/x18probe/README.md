# x18probe — does macOS preserve `x18` across a signal?

A small standalone macOS program (no AROS) that answers the one question the
aarch64 port hinged on: **can a value left in register `x18` survive signal
delivery on Apple Silicon?**

```sh
./build.sh          # compile + run; prints the verdict
```

## Why it exists

AROS-hosted preempts tasks with signals, and on aarch64 it saves/restores the
interrupted register set (`x0..x28`, `x18` included) from the signal context. On
Linux and bare metal that keeps `x18` usable. On Apple Silicon `x18` is the
platform register the macOS kernel reserves for itself, so the real question is
whether the signal frame macOS hands the handler still holds the user's `x18`.

The probe reproduces the exact AROS mechanism: hold a sentinel in `x18`, let a
timer signal (`SIGALRM`) fire while holding it, then read `x18` back from the
signal context — the very value AROS's `SAVEREGS` would copy.

## Result

```
user put in x18      : 0xCAFEF00DDEADBEEF
x18 in signal context: 0x0000000000000000
verdict              : CLOBBERED -> macOS wiped it before AROS sees it
```

Every run. macOS zeroes `x18` in the signal frame before any AROS code runs, so
the host cannot preserve it no matter how faithfully it saves/restores. That is
why the aarch64-AROS target must build with `-ffixed-x18`, and it matches the
ffmpeg h264 crash (the decoder kept the bitstream pointer in `x18`; a preemption
zeroed it → SIGSEGV).

## Where the decision lives

- [NOTES.md](../../NOTES.md) — the `x18` reservation decision and rationale.
- [ffmpeg-native](../../docs/features/ffmpeg-native/README.md) — the h264 crash
  this explains and the `-ffixed-x18` fix.
- [debug-tools](../../docs/features/debug-tools/README.md) — the probe as a
  reusable "does the host preserve X across a signal?" template.
