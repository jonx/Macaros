# ARexx / REXX on this port — the host-port message protocol, what runs, and where the official interpreter actually lives

> Status: status survey + grounded reference (drafted 2026-06-29) · The IPC route
> that consumes this is **Route B `[B0]`–`[B1]`** in
> [../memory-protection/design.md](../memory-protection/design.md#route-b--shared-public-arena-single-system-image-cooperation-b).
> Process: [../CLEANROOM.md](../CLEANROOM.md).

**Why this doc exists.** Route B's `[B1]` proof-of-concept is "an ARexx host port
crosses a process boundary." Writing it up raised the obvious reviewer question —
*ARexx isn't running here; is that because no official port exists, or because we
haven't deployed it?* The answer decides how `[B1]` is framed. This doc pins it down,
grounded in both AROS repos and the local tree, and corrects one over-broad claim the
Route B draft made (that `RexxMast` "does not exist anywhere in the tree").

The load-bearing finding: **ARexx on AROS splits into two layers that live in two
different repositories**, and `[B1]` needs only the layer that is already here.

---

## The two layers, and where each one lives

ARexx is not one component. It is a **message/port layer** (build a `RexxMsg`, find a
port by name, carry argument strings) and an **interpreter layer** (evaluate `.rexx`
source, run the `REXX:` public port that dispatches scripts). They ship separately.

| Layer | What it is | Repo | On this port |
|---|---|---|---|
| **Message / port** | `rexxsyslib.library` — `CreateRexxMsg`/`CreateArgstring`/`FillRexxMsg`; `struct RexxMsg`, `rm_Args[16]`, the `RX*` action codes | **main OS** (`aros-development-team/AROS` = local `aros-upstream`) | **built + deployed** in the boot set |
| **Support** | `rexxsupport.library` — extra host functions queried by the interpreter | **main OS** | source present; not deployed |
| **Front-end commands** | `RX`, `RXLIB` (the `Rexxc` drawer) | **main OS** (`workbench/rexxc/`) | source present; **not deployed** |
| **Interpreter** | `RexxMast` (the `RexxMaster` server / `REXX:` port) **+ the actual Rexx engine** | **contrib** (`aros-development-team/contrib`, `regina/`) | **absent** — not built, not deployed |

So the main OS repo — and therefore the local `aros-upstream` checkout — ships the
**protocol** but not the **interpreter**. That is not an omission in the checkout; the
interpreter was never in that repo. It is a separate, optional contrib component.

## Yes — there is an official port. It's Regina, in `contrib`.

The official AROS ARexx interpreter is **Regina REXX**, ported as `regina.library`,
fronted by a thin `RexxMast` that spawns the public-port server. It lives in the
**contrib** repository, not the main OS repo:

- **[`contrib/regina/`](https://github.com/aros-development-team/contrib/tree/master/regina)**
  — Regina REXX 3.x ported to AROS as `regina.library` (`libbasetype struct
  ReginaBase`, `ReginaVersion()`, `IsReginaMsg(struct RexxMsg *)`). Carries a
  `TODO.AROS`. This is the language engine.
- **[`contrib/regina/rexxmast/RexxMast.c`](https://github.com/aros-development-team/contrib/blob/master/regina/rexxmast/RexxMast.c)**
  — the `C:RexxMast` command. It `CreateNewProcTags`-spawns a process named
  **`RexxMaster`** (the public `REXX` port server that classic apps like HippoPlayer
  look for), then dispatches incoming `RexxMsg`s (`StartFile` / `AddLib` / `RemLib` /
  `AddCon`), delegating all language evaluation to `regina.library`. Its build line is
  `uselibs="regina_shared rexxsyslib"`, `targetdir=$(AROS_C)` → `C:RexxMast`.
  **RexxMast is glue onto Regina, not a standalone interpreter.**
- A second, older engine — **BREXX** — also sits in contrib at
  [`contrib/rexx/`](https://github.com/aros-development-team/contrib/tree/master/rexx)
  (its own `src/` with `bintree.c`/`lstring`/`dqueue.c`). It is *not* what `RexxMast`
  uses; note it only so its presence isn't mistaken for the live path.

The coupling is even visible in the **main-repo headers**: the `RexxMsg` action codes
carry a block commented *"added for AROS and regina only"* — `RXADDRSRC`, `RXSETVAR`,
`RXGETVAR`
([compiler/include/rexx/storage.h:66-74](../../../../aros-upstream/compiler/include/rexx/storage.h)).
The OS message layer was shaped around Regina as its intended interpreter.

This is also why distros (Icaros, AROS One, the nightlies) *can* run ARexx and forum
users *do* start `RexxMast`: those builds bundle the contrib Regina+RexxMast. A default
build of the main repo alone does not — and `RX.c` says so itself, printing *"Could not
start RexxMast; no Rexx interpreter seems to be installed"* when the spawn fails
([workbench/rexxc/RX.c](../../../../aros-upstream/workbench/rexxc/RX.c)).

The boot is wired for the drop-in: the Startup-Sequence runs the interpreter **only if
present** —
```
If EXISTS "C:RexxMast"
    Assign "REXX:" "S:"
    Run <NIL: >NIL: QUIET C:RexxMast
```
([workbench/s/Startup-Sequence:95](../../../../aros-upstream/workbench/s/Startup-Sequence#L95)).
On this port the `If EXISTS` is false, so nothing starts.

## Why `[B1]` doesn't need any of that

`[B1]` proves a **named public port + `RexxMsg` + string-args exchange crosses a process
boundary** (via Route B's arena). That is the *protocol*, and the protocol is wholly in
the deployed message layer — independent of the interpreter:

- A host **port** is just a named `MsgPort` an application creates and answers. Any app
  implements one directly; no interpreter is involved in *receiving* a command.
- The **payload** is value-oriented: `struct RexxMsg` carries `rm_Args[0..15]` argument
  **strings** built with `CreateArgstring`
  ([storage.h:22-37, 45-51](../../../../aros-upstream/compiler/include/rexx/storage.h)) —
  data crosses by string value, not by deep pointer graph. That is exactly the shape
  Route B's arena handles best.
- The **verbs** are the `RX*` action codes (`RXCOMM` = a command line, `RXFUNC` = a
  function call), also in the main-repo header
  ([storage.h:53-64](../../../../aros-upstream/compiler/include/rexx/storage.h)).
- The construction primitives are the **deployed** `rexxsyslib.library` calls —
  `CreateRexxMsg` / `CreateArgstring` / `FillRexxMsg` / `IsRexxMsg` / `ClearRexxMsg`
  ([rexxsyslib.conf:19-26](../../../../aros-upstream/workbench/libs/rexxsyslib/rexxsyslib.conf)).

The interpreter is needed only to **evaluate a script** that *sends* such commands (and
to run the system `REXX:` port). For a host↔app or app↔app message exchange, the sender
builds the `RexxMsg` itself. So `[B1]`'s honest scope is: *the ARexx host-port protocol,
on the already-deployed `rexxsyslib.library`* — with bringing up Regina an explicitly
separate track (below).

## What's actually deployed here today

- ✅ `rexxsyslib.library` — built and deployed in the boot set
  ([../../../graft/run-window.sh](../../../graft/run-window.sh), `graft/aros-ctl`).
- ⚠️ **but unproven on aarch64.** The arch tree carried m68k asm
  (`arch/m68k-all/rexxsyslib`); the generic C paths that this target falls back to have
  not been round-tripped here. A `CreateRexxMsg`→`CreateArgstring`→read-back check is
  exactly Route B's **`[B0]`** gate, run before crossing any boundary.
- ❌ `RexxMast` / `regina.library` — not built, not deployed → **no `.rexx` execution**.
- ❌ `RX` / `RXLIB` (`Rexxc` drawer) — source only, not deployed.

## Optional later track — bring up a real interpreter `[BR*]`

Out of scope for `[B1]`, recorded so the path is known and not re-discovered. This is
**UNVERIFIED** on aarch64 — no part has been built on this target.

- **`[BR0]` build `regina.library`** from `contrib/regina/` for aarch64 AROS. Regina is
  portable C; the AROS port already exists in contrib, so this is a *retarget*, not a
  port from scratch. Open risk: the contrib build has only been exercised on i386/x86_64;
  `TODO.AROS` is the known-issues list.
- **`[BR1]` build + deploy `RexxMast`** from `contrib/regina/rexxmast/` (`uselibs =
  regina_shared rexxsyslib`) into `C:`, so the Startup-Sequence `If EXISTS` fires and the
  `RexxMaster` / `REXX:` port comes up.
- **`[BR2]` deploy `RX`/`RXLIB`** (`workbench/rexxc/`) into the `Rexxc` drawer and prove
  `rx "say 1+1"` evaluates.
- **`[BR3]` host-driven script** — drive a `.rexx` end-to-end from the control harness as
  an unattended PASS/FAIL.

Do **not** fold `[BR*]` into Route B's PASS criteria — Route B is an **IPC** proof and
must stay green without an interpreter. `[BR*]` is a capability add-on.

## Honest debt & open questions

- **`rexxsyslib.library` on aarch64 is UNVERIFIED** until `[B0]` runs. Deployed ≠ proven.
- **Regina on aarch64 is UNVERIFIED.** The contrib port exists and is "official", but its
  build has not been confirmed on this target; treat `[BR0]` as real work with a known
  TODO list, not a checkbox.
- **Naming.** "Host port" here is the **ARexx** sense — an application's named command
  `MsgPort` — *not* a macOS-host port. No macOS facility is bridged in this doc; ARexx is
  an AROS-internal IPC subsystem. (A separate question — exposing an AROS ARexx port to a
  macOS process — would be a Route-B-arena-plus-host-shim composition, not specced here.)

## References

Grounded in the upstream tree (`/Users/user/Source/aros-upstream`) and the AROS GitHub
repos. Confirmed 2026-06-29.

Main OS repo (local `aros-upstream`) — the message/port layer that **is** here:

- [compiler/include/rexx/storage.h](../../../../aros-upstream/compiler/include/rexx/storage.h)
  — `struct RexxMsg` (`rm_Args[16]`, `rm_Action`, line 22-37); `ARG0`/`RXARG`/`MAXRMARG`
  (45-51); `RXCOMM`/`RXFUNC`/… action codes (53-64); the *"added for AROS and regina
  only"* `RXADDRSRC`/`RXSETVAR`/`RXGETVAR` block (66-74) — header-level evidence of the
  Regina coupling.
- [workbench/libs/rexxsyslib/rexxsyslib.conf:19-26](../../../../aros-upstream/workbench/libs/rexxsyslib/rexxsyslib.conf)
  — `CreateArgstring` / `DeleteArgstring` / `CreateRexxMsg` / `ClearRexxMsg` /
  `FillRexxMsg` / `IsRexxMsg` (the deployed construction primitives).
- [workbench/rexxc/RX.c](../../../../aros-upstream/workbench/rexxc/RX.c) — `SystemTags("RexxMast", …)`
  and the *"no Rexx interpreter seems to be installed"* fallback.
- [workbench/s/Startup-Sequence:95](../../../../aros-upstream/workbench/s/Startup-Sequence#L95)
  — `If EXISTS "C:RexxMast"` → `Assign REXX: S:` → `Run … C:RexxMast` (the optional drop-in).
- [workbench/libs/rexxsupport/](../../../../aros-upstream/workbench/libs/rexxsupport/) ·
  [workbench/libs/rexxsyslib/](../../../../aros-upstream/workbench/libs/rexxsyslib/) — the
  two main-repo libraries.

contrib repo — the interpreter that is **not** here (the official port):

- [contrib/regina/](https://github.com/aros-development-team/contrib/tree/master/regina)
  — Regina REXX as `regina.library` (`ReginaBase`, `ReginaVersion`, `IsReginaMsg`); `TODO.AROS`.
- [contrib/regina/rexxmast/RexxMast.c](https://github.com/aros-development-team/contrib/blob/master/regina/rexxmast/RexxMast.c)
  — spawns `RexxMaster`, dispatches `RexxMsg`s, `uselibs="regina_shared rexxsyslib"` → `C:RexxMast`.
- [contrib/rexx/](https://github.com/aros-development-team/contrib/tree/master/rexx) — the
  older, separate **BREXX** engine (not the live path).

Companion (this repo):

- [../memory-protection/design.md](../memory-protection/design.md#route-b--shared-public-arena-single-system-image-cooperation-b)
  — **Route B** `[B*]`: the shared-arena cooperation route whose `[B0]`/`[B1]` POC this
  doc grounds.

## Provenance

Independent work. No third-party interpreter implementation source — Regina, BREXX, or
otherwise — was read or consulted; the contrib paths above are cited from the public
repository's directory/build metadata only, to locate the official port, not transcribed
from. The protocol facts come from the AROS main-repo public headers/`.conf`, published
ARexx documentation, and this project's own tree. See
[../CLEANROOM.md](../CLEANROOM.md).
