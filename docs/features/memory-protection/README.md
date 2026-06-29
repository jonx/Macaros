# Memory protection, resource tracking & virtual memory

> Status: design + spec done · POC next (in `hosted/`) · Target: aarch64-darwin hosted ·
> Drafted 2026-06-29

The Amiga robustness wishlist — **MP** (memory protection), **SVM** (swappable virtual
memory), **RT** (resource tracking), **SMP** — asked concretely: *given this source tree and
a large Apple-Silicon host, how would we actually add them, and which are worth it?*

**Key finding:** the blocker was never the MMU — it is the **shared-pointer ABI**
(`PutMsg` passes raw pointers that "must lie in shared memory"). So this is a family of
additive capabilities, not one feature, and being hosted on a powerful Mac opens a route the
bare-metal Amiga never had.

## The two routes worth building here

- **Intra-AROS diagnostic protection `[MP*]`** — keep one address space; use the host MMU
  (the already-real `KrnSetProtection`→`mprotect`) to **guard pages around stacks**, **trap
  the NULL page**, and make **ExecBase read-only**, so a wild write / overflow / NULL becomes
  an *immediate, attributed* fault instead of silent corruption. Most plumbing already exists
  (it extends the **built** [crash-handling](../crash-handling/design.md) trap handler). This
  is the AROS passage's literal ask — *alert on something dubious, say what happened, save the
  work*.
- **Multi-instance / process-per-app `[MI*]`** — *the M5 question.* Run each program in its
  **own AROS instance (its own Darwin process)** → hard, kernel-enforced isolation **for free**,
  resource tracking **for free** (host reclaims on exit), one window per app. Two of the four
  shared channels are **already built** — [host-volume](../host-volume/design.md) and
  [clipboard-bridge](../clipboard-bridge/design.md). Trade-off: it does *not* protect tightly
  *cooperating* apps (shared screens / message ports), so it complements rather than replaces
  intra-AROS MP. **A and C compose**: integrated desktop in one instance, risky/heavy apps each
  in their own.
- **Shared public arena `[B*]`** — *the "but can't the instances still talk?" question.* The
  bridge that lets C's isolated instances cooperate again: a **bounded shared region** mapped at
  the **same address in every instance** (`MAP_FIXED`), where opt-in public objects live — so a
  pointer means the same bytes everywhere ("same indexes, same computer"). On one machine it's
  real shared RAM (hardware-coherent — no DSM protocol). The cost isn't the mapping; it's that
  *anything shared becomes an SMP domain* (real cross-process locks, since `Forbid()` only stops
  one instance) and *shared objects may hold no private pointers*. So it's value-oriented and
  opt-in — **ARexx is the `[B1]` POC** (string-args host port). Status: ARexx is **not running**
  here (`rexxsyslib.library` deployed, but the interpreter — the official **Regina** port in AROS
  *contrib*, not the main repo — is **not built/deployed**) — yet `[B1]` needs only the messaging
  library, not the interpreter. Full survey: [../arexx-host-port/README.md](../arexx-host-port/README.md).
  This is AROS's own "protect programs that know about it," made concrete.

Plus: **RT** = light up the dormant `iet_RT` slot, opt-in, with ownership transfer (the policy
problem that stalled it); **SVM** = mostly free (macOS already pages us) — add a `MEMF_VIRTUAL`
managed backend only to over-commit RAM; **SMP** = declined on this target except Route B's arena
(`Forbid()`/`Disable()` mean "single CPU"). Details in the design.

## Files

- [design.md](design.md) — the ABI obstacle, the already-present hooks (table with file:lines),
  the three-route fork, the full multi-instance evaluation, RT/SVM/SMP, sequencing.
- [spec.md](spec.md) — the two POC ladders (`[MP1]`–`[MP3]`, `[MI1]`–`[MI2]`) as greppable
  build→run→PASS/FAIL spikes in `hosted/`, with per-contract provenance.

## Relationship to neighbours

Built on [crash-handling](../crash-handling/design.md) (reactive: bounded, symbolized guru) —
this is the *proactive* half. Reuses [native-modules](../native-modules/design.md) (R/W→R/X
flip) for read-only code, and the built [host-volume](../host-volume/design.md) /
[clipboard-bridge](../clipboard-bridge/design.md) / [bsdsocket-net](../bsdsocket-net/design.md)
as the multi-instance shared channels.
