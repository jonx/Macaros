# AROS 68k runtime handover — 2026-08-04

Merge base at handover: graft `81aacb6`, aros-upstream `98f8296505`.
All gates green (T3EVENT, T3GEN, T3PORT, T3PROC, T3IDCMP, T3LHA, T2GUARD),
legacy corpus byte-identical. Both trees clean. Nothing pushed.

## The headline

**Photogenics 1.2 (PhotoDemo) runs and draws its entire interface** — menu bar,
tool palette, images panel, canvas — with no capability gap. A 1995 commercial
Amiga application, translated instruction by instruction, calling the real AROS
libraries. Windows drag and right-click opens its menu.

What it cannot yet do is respond to a click, because delivery reaches the port
the program binds but that program never reads it in the state it reaches. See
"the frame-wait contract" below: it is a scheduling question, not a delivery
one.

## What was built (all committed, all gated)

- **Guest-owned object adoption.** Classic code allocates its own `Gadget` list
  and hands it over; nothing issued a handle, so the crossing refused something
  completely ordinary. A structure the program owns is mirrored natively under
  its guest address, re-validated and converted on **every** crossing, adopted
  whole as a family, with the library's relinking written back as guest
  addresses. Gates: `T3OWNGAD` plus uncarryable-field and cycle controls.
- **The exec IPC layer**, which did not exist at all: `PutMsg`, `GetMsg`,
  `ReplyMsg`, `WaitPort`, `Wait`, `Signal`, `AllocSignal`/`FreeSignal`. Every
  structure involved is guest memory, so these are guest-list operations plus
  the signal that makes a `WaitPort` return.
- **A second 68k process**, cooperatively scheduled. Both contexts are 68k and
  share only guest memory and signal bits, so they never need to run at the same
  instant — only to make progress. One thread, one engine instance each,
  switching at `Wait`; a context that cannot proceed parks and hands the turn
  back. This removed the locking, the object-table races and the
  `ExecBase->ThisTask` problem that threads would have created.
- **IDCMP delivery**, pumped before a wait is answered (the loop is
  `Wait -> GetMsg -> ReplyMsg`; pumping at `GetMsg` only serves programs that
  poll). Only ports the program **bound** to a window are pumped; a worker's own
  `pr_MsgPort` is an ordinary mailbox and never receives native input.
- **A generic device contract.** `OpenDevice` opens the real device and hands
  the guest a base in `io_Device`. Sending a command is a separate contract and
  each unserved one is refused by name where it is sent.
- **Bridge Lab** — see `docs/features/bridge-lab/README.md`. A run becomes
  runtime-contract evidence: JSON Lines events, a reviewed registry
  (`graft/emu68k-runtime-contracts.json`), and `graft/bridge-lab report`.
- **Importer reach**: value-structure crossings, a retention rule that reads
  where a store LANDS, nullability from the library's own source, tag-domain
  promotion, BPTR results as tokens, and import-owned entries staying derivable
  so later rules reach vectors already promoted.

## The one open bug

A host-side **wild jump** — `pc == lr == fault address` — in translated code at
68k PC `0x29F6B2`, reproducible, in PPaint, immediately after two device opens
that both succeed and return valid bases (`keyboard.device -> 0x0022a000`,
`input.device -> 0x0022b000`).

**Do not re-investigate these. They are eliminated, each by direct measurement:**

| hypothesis | how it died |
|---|---|
| `OpenDevice` is defective | bisect cut placed AFTER the call: no crash. Native oracle opens keyboard/timer/input, cold and warm |
| short IORequest overwrite | `sizeof(struct IOStdReq)` is **96** on aarch64, `mn_Length` matches, the bridge asks for exactly that |
| the swapped stack | `DeviceProbe ... swap` opens fine on a 512 KB swapped stack; 2 MB behaves like 512 KB |
| `device_base` returns garbage | probed at runtime: returns valid bases for both devices |
| missing host symbol | the loaded dylib exports it and the binder fails closed anyway |
| stale objects / ABI skew | full module rebuild changes nothing |
| guest allocator overlap | it is a clean bump allocator with 8-byte rounding |

The lead: a `jsr` through a registered LVO base should **trap to the bridge**,
not transfer anywhere. So either the registration is not in effect for that base
in that engine instance, or the guest is calling through something else. Note
that contexts have their own engine instances and libbases are replayed per
context entry — that is the first place to look.

`graft/HANDOVER` note: `DeviceProbe` (in `workbench/c/`) is the native oracle;
`C:DeviceProbe <device> <unit> <flags> <count> [swap]`.

## The frame-wait contract, still `needs-review`

`scheduler.yield.frame_wait`: **a context that never calls a blocking primitive
must not starve its siblings.** Evidenced twice — PhotoDemo (a child polls
20,067 times, the context owning the IDCMP port runs twice) and the
`genframeyield` fixture, which reproduces it in 200 calls and **fails by
design**. Registry status stays `needs-review` because fixture evidence proves
the mechanism, not that the shape generalizes; an independent second legacy
program has not reached it yet. Promoting it will tighten `T3EVENT`
automatically — the gate asserts consistency between registry status and
observed behaviour, so no edit is needed.

## Two method rules this session paid for

1. **Verify the tree, not the commit.** An hour of results was void because the
   checkout sat on a branch that did not contain the commits being reasoned
   about — same file path, different content. Runs now stamp both repository
   SHAs, dirty state, and a **hash of the loaded dylib** (a source SHA cannot
   identify a stale artifact). Check `run.start` before trusting any run.
2. **Reproduce before diagnosing.** Three identical runs took two minutes and
   dissolved a question a four-case determinism matrix was about to be spent on.

## Standing constraints

Do not push either repository. Commit per meaningful change in the repo it
changes. Never a bare `make`; build explicit metatargets in `~/aros-build`
reusing `~/aros-crosstools`. No Claude/AI mention anywhere outward-facing,
commits included. Anything touching `aros-development-team/AROS` needs explicit
per-action approval.
