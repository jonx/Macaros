# scan68k test corpus

Crafted 68k programs for the static hardware-use scanner ([T2a], see
[docs/features/68k-transparent-exec/](../../../docs/features/68k-transparent-exec/README.md)).
Assembled with the `apps68k` vasm; `bin/` is gitignored, rebuild with:

```sh
../../jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
    -o bin/<name>.exe <name>.s      # or just: make hosted-emu68k-t2scan
```

**Hardware bangers** (must route `FULL`): `chipbang` writes `$DFF182`,
`ciapeek` reads a CIA register, `vecwrite` installs an exception vector,
`superviolate` uses privileged machine control.

**Negative controls** (must route `JIT`, and they are the interesting half):

- `datadecoy` puts hardware addresses inline in the code hunk as a table. A
  linear scan sees them; the confidence grading must call them *weak*.
- `opdecoy` puts opcode-shaped words in a DATA hunk, which is not code.
- `computedhw` builds `$DFF180` at run time from two halves, so it appears
  nowhere in the image. **No static scan can find this one** - it routes `JIT`
  and the engine's runtime guard is what catches it. This is the honest limit
  the whole confidence design exists for.
- `color00` uses the exact `move.w Dn,$DFF180` CPU-calibration spelling the
  hosted engine implements as a flag-correct write sink. The literal remains
  weak scan evidence, but it routes `JIT` and completes. Other colour-register
  writes and computed accesses remain protected hardware events.

The regression also asserts that no real program (the `apps68k` corpus,
Dhrystone) is ever routed `FULL`: a wrong `FULL` sends a working program to an
emulator, which is the expensive mistake.
