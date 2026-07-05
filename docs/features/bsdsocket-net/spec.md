# Implementation spec — host BSD sockets backing `bsdsocket.library` (Apple-native)

> Status: drafting (Role A) · Target: aarch64-darwin hosted · Drafted 2026-06-24
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted in producing this spec, and any resemblance
to existing implementations is coincidental.** Implement only from this spec + the approved
sources cited by tag: `[PUB]` POSIX / Apple `man` pages / published standards, `[AROS]`
in-tree AROS headers and modules (paths given), `[OURS]` this project's spikes (the
H-series in `NOTES.md`). `[DERIVED]` items are independently-derived requirements flagged
for extra verification; each stands solely on its cited `[PUB]`/`[AROS]`/`[OURS]`
justification — implement from that justification. Our design is kqueue + one pump thread,
derived from the AmiTCP autodoc semantics + macOS `kqueue` docs + this project's H-series.

## Scope

**In.** A host-passthrough `bsdsocket.library` for `aarch64-darwin` that maps the AmiTCP
LVO surface (`socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/`sendto`/`recvfrom`/
`CloseSocket`/`WaitSelect`/`Errno`/…) onto macOS's native BSD sockets in
`libSystem.dylib`, reached through `hostlib.resource`. The macOS kernel does the real work
(TCP state machine, routing, the NIC); AROS code reaches it via the standard LVO calls it
already expects. The load-bearing deliverable is a **`WaitSelect` and blocking-call-avoidance
model** built on non-blocking host sockets + a **kqueue host pump thread that raises an AROS
`Signal`** when fds become ready — so a guest `recv`/`accept`/`connect` never freezes AROS's
single underlying execution thread (H6).

**Decision (confirmed with the project owner).** **Forward, don't reimplement.** Map
`bsdsocket.library` onto the host stack; do **not** bring up AROS's own AROSTCP/SANA-II
stack on an emulated NIC. Rationale in [design.md](design.md): the host has a battle-tested
stack one syscall away, and localhost loopback needs no entitlement (unlike raw BPF/`utun`),
which keeps verification hermetic and TCC-free.

**Decision.** **Apple-native readiness primitive: kqueue.** macOS has no `epoll`; `kqueue`
is the platform idiom for scalable fd-readiness, `[PUB]` (`kqueue(2)`, `kevent(2)`). A
`poll(2)` pump is an acceptable fallback if a kqueue problem surfaces (note, don't build
both) — the bridge contract below is written so the readiness source is swappable.

**Out (non-goals, this spec).** AROSTCP/SANA-II; an emulated NIC; IPv6 (AROS `AF_INET6=28`
vs macOS `AF_INET6=30` differ — IPv4-first; §"Open questions"); a non-localhost DNS
resolver as a bring-up gate (`gethostbyname` is designed here but its blocking-resolver
offload is deferred past the core suite); `sendmsg`/`recvmsg` (LVO 45/46),
`GetSocketEvents` (LVO 50), and the Roadshow extensions (LVO 61+) beyond declaring their
vectors; multi-host portability of this module to Linux (the dir name is chosen to allow it
later — §"Build / integration" — but Linux is not a deliverable here). **Also out — the
shipped network commands that cannot ride a socket forwarder:** the raw-socket utilities
`ping`/`traceroute` (they need host root / `SOCK_RAW` — `ping.c:631`; a privilege grant or a
`SOCK_RAW`→`SOCK_DGRAM` ICMP shim is a separate feature) and the stack-introspection/config
tools `netstat`/`route`/`arp`/`ifconfig`/`ip`/`ipf`/`ipnat` (they read/configure the AROS
stack via `kvm`/AROSTCP internals — `netstat/main.c` — and there is no AROS stack when the
Mac owns it). The supported surface is socket-client apps (HTTP/SMB/…) plus the
resolver-gated `resolve`/`nslookup`/`hostname`; see design.md "What proves the internet
works in AROS — the test tools".

## Architecture

Two layers joined by the existing **H3 host-call boundary** — no new flat C ABI of our own:
unlike the Cocoa/Metal shim, the "host side" here is just `libSystem.dylib`'s BSD socket
symbols, whose signatures are fixed by POSIX `[PUB]`. The only net-new host-side code is the
**pump thread**, which is small and also calls libc (`kqueue`/`kevent`/`pthread_create`).

```
AROS side (aarch64, AROS crosstools)              Host side (Apple libSystem)
┌────────────────────────────────────┐            ┌──────────────────────────────┐
│ bsdsocket.library (new, all-darwin) │  hostlib + │ libSystem.dylib BSD sockets  │
│  · per-task SocketBase (LVO 1 Open) │  H3 shim   │  · socket/bind/connect/...   │
│  · LVO table: socket..WaitSelect    │ ─────────► │  · O_NONBLOCK fds            │
│  · dTable: AROS fd → host fd        │            │                              │
│  · errno xlate → per-task errnoPtr  │ ◄───host fd│                              │
│                                     │   + errno  │                              │
│  WaitSelect / blocking recv:        │            ├──────────────────────────────┤
│   register fds, Wait(readySig|mask) │   raises    │ kqueue PUMP THREAD (new)     │
│            ▲                        │   AROS Sig  │  · real pthread              │
│            └────────── Signal() ◄───┼────────────┤  · blocks in kevent()        │
│                                     │            │  · never runs AROS code      │
└────────────────────────────────────┘            └──────────────────────────────┘
```

- **AROS module** `[AROS]` — a new library, structurally a port of the in-tree
  Windows-only `arch/all-mingw32/bsdsocket/` template (which is host-neutral *above* the
  host-call layer): same `bsdsocket.conf` LVO functionlist, same per-task base shape, same
  `Forbid`/`Permit` + errno-offset discipline. Retarget the host interface from Winsock to
  libc, and **replace** the stubbed `WaitSelect`/`recv`/`connect` and the Windows
  `host_socket.c` (`ResolverThread` + `KrnCauseSystemIRQ`) with the kqueue-pump bridge.
- **Host pump thread** `[OURS]` — one real OS pthread that blocks in `kevent()` and, on
  readiness, raises an AROS `Signal` into the waiting task. It never executes AROS code, so
  it does not perturb the H4/H6 scheduler invariants (one underlying thread driven by
  SIGALRM). This is the only piece with no in-tree precedent.
- **Host-call crossing** — every libc socket call and the pump's libc calls cross the H3
  AAPCS64↔Apple-arm64 shim (`hosted/abishim.S`, `hosted/host.c`) `[OURS]`, the same
  boundary `hosted/device.c` proved for `pread`/`pwrite` (H11).
- Spike-phase paths: host-socket probes + the pump live under `hosted/` (the Phase-2
  "prove the mechanism in a file" style, like `hosted/device.c`). At graft, the AROS side
  lands in `arch/all-unix/bsdsocket/` (§"Build / integration").

## The host interface (`HostSockInterface`) — `[PUB]` + `[OURS]`

There is no hand-authored C ABI to invent: the host calls are POSIX BSD sockets whose
signatures are fixed by the standard `[PUB]`. The AROS side builds a struct of libc function
pointers (the libc analogue of mingw32's `WinSockInterface`), resolved once at library init
via `hostlib.resource` against `libSystem.dylib` — the same `HostLib_Open` /
`HostLib_GetInterface` path the mingw32 port uses for `Ws2_32.dll`
(`arch/all-mingw32/bsdsocket/bsdsocket_init.c`) `[AROS]`, retargeted to libc. The symbols
already resolve on darwin: `arch/all-unix/hidd/unixio/unixio.h` maps `HOST_OS_darwin` →
`LIBC_NAME "libSystem.dylib"`, and `unixio_class.c`'s `libc_symbols[]` already imports
`socket`/`sendto`/`recvfrom`/`bind` across unix hosts `[AROS]`.

Required host symbols (all `[PUB]` POSIX, in `libSystem.dylib`):

```
socket  bind  listen  accept  connect  shutdown  close
send  recv  sendto  recvfrom  setsockopt  getsockopt  getsockname  getpeername
fcntl   /* set O_NONBLOCK */                ioctl  /* FIONBIO / FIONREAD */
kqueue  kevent                              /* the pump's readiness primitive */
pthread_create  pthread_kill  write/read    /* pump thread + its wake pipe */
getaddrinfo  freeaddrinfo  gai_strerror     /* resolver, deferred */
```

The pump-control surface (our own, tiny, ASCII, independent work) — a handful of functions the
AROS side calls to drive the pump, defined in §"The bridge":
`pump_start()`, `pump_register(hostfd, want, sb)`, `pump_unregister(hostfd, sb)`,
`pump_drain(sb, out_ready[])`, `pump_wake(sb)`. These are `[OURS]`; their *behaviour* is
specified below, not their internals.

## SocketBase — the per-task open contract — `[AROS]`

`bsdsocket.library` is unusual: **each task gets its own library base.** The contract,
grounded two ways (the AROSTCP `struct SocketBase` in
`workbench/network/stacks/AROSTCP/bsdsocket/api/amiga_api.h`, and the simpler mingw32
`struct TaskBase` + `BSDSocket_OpenLib` in `arch/all-mingw32/bsdsocket/bsdsocket_intern.h`
/ `bsdsocket_open.c`) `[AROS]`:

- `OpenLibrary("bsdsocket.library", …)` → `Open` (LVO 1) looks up the calling task; if
  absent, `MakeLibrary`s a **per-task base** sized `sizeof(struct TaskBase)`
  (`bsdsocket_open.c`).
- That base holds: the **descriptor table** `dTable` (default size **`FD_SETSIZE`** = 64
  — `bsdsocket_open.c` `SetDTableSize(FD_SETSIZE, tb)`), `errnoPtr`/`errnoSize` (default
  `&errnoVal` / `sizeof errnoVal`), and the per-task signal masks
  `sigintr`/`sigio`/`sigurg` (default `sigintr = SIGBREAKF_CTRL_C`).
- **Consequence for Role B and for app code:** every task doing socket I/O must
  `OpenLibrary` `bsdsocket.library` itself — the fd table, errno, and signal masks are
  **per-task, not shared**. A socket descriptor is meaningful only to the `SocketBase` that
  created it (until explicitly handed off via `ObtainSocket`/`ReleaseSocket`).

Port the `TaskBase`/`OpenLib`/`CloseLib` shape **as a template** from mingw32 (it is
host-neutral), then **add** the per-base fields the bridge needs (§"The bridge"):
a `readySig` signal bit allocated with `AllocSignal()` `[AROS]`, and the per-base pump
registration bookkeeping.

## The LVO surface — `[AROS]`

The authoritative LVO map is `workbench/network/common/include/defines/bsdsocket.h`
(offset via `AROS_LC<N>(…, <LVO>, BSDSocket)`); prototypes in
`clib/bsdsocket_protos.h`; the machine-readable functionlist with the register mapping is
`arch/all-mingw32/bsdsocket/bsdsocket.conf` (host-neutral — **copy verbatim**). Verified
offsets and register mappings to implement:

| LVO | Function | Regs | Notes |
|----:|----------|------|-------|
| 1  | `Open` (per-task base) | — | LVO 1; makes the `TaskBase` (above) |
| 2  | `Close` | — | tears the per-task base down; closes its open fds |
| 5  | `socket(domain,type,protocol)` | D0,D1,D2 | create + `O_NONBLOCK`; allocate AROS fd |
| 6  | `bind(s,name,namelen)` | D0,A0,D1 | copy `sockaddr` (near-verbatim, §"Address") |
| 7  | `listen(s,backlog)` | D0,D1 | |
| 8  | `accept(s,addr,addrlen)` | D0,A0,A1 | non-blocking; park-on-`EWOULDBLOCK` |
| 9  | `connect(s,name,namelen)` | D0,A0,D1 | non-blocking; `EINPROGRESS`→park on write-ready |
| 10 | `sendto(...)` | D0,A0,D1,D2,A1,D3 | |
| 11 | `send(s,msg,len,flags)` | D0,A0,D1,D2 | park-on-`EWOULDBLOCK` for write-ready |
| 12 | `recvfrom(...)` | D0,A0,D1,D2,A1,A2 | |
| 13 | `recv(s,buf,len,flags)` | D0,A0,D1,D2 | park-on-`EWOULDBLOCK` for read-ready |
| 14 | `shutdown(s,how)` | D0,D1 | |
| 15 | `setsockopt(...)` | D0,D1,D2,A0,D3 | |
| 16 | `getsockopt(...)` | D0,D1,D2,A0,A1 | |
| 17 | `getsockname(s,name,namelen)` | D0,A0,A1 | |
| 18 | `getpeername(s,name,namelen)` | D0,A0,A1 | |
| 19 | `IoctlSocket(s,req,argp)` | D0,D1,A0 | `FIONBIO`/`FIONREAD` |
| 20 | `CloseSocket(s)` | D0 | pump-unregister, close host fd, free AROS fd |
| 21 | `WaitSelect(nfds,r,w,e,tv,sigmask)` | D0,A0,A1,A2,A3,D1 | **the crux** (§"Concurrency") |
| 22 | `SetSocketSignals(intr,io,urg)` | D0,D1,D2 | sets `sigintr`/`sigio`/`sigurg` |
| 23 | `getdtablesize()` | — | returns `dTableSize` |
| 27 | `Errno()` | — | reads the latest per-task errno |
| 28 | `SetErrnoPtr(ptr,size)` | A0,D0 | redirect per-task errno storage |
| 35 | `gethostbyname(name)` | A0 | resolver (deferred offload, §"Resolver") |
| 44 | `Dup2Socket(d1,d2)` | D0,D1 | dTable duplication |
| 49 | `SocketBaseTagList(tags)` | A0 | `SBTC_*` config (`socketbasetags.h`) |

`SocketBaseTags(...)` is the varargs wrapper over `SocketBaseTagList()`. `select()` is
`#define`d to `WaitSelect(…, NULL)` in `bsdsocket.conf`. LVOs 29–43 are the
`inet_*`/`get*by*` resolvers; declare their vectors, stub the non-IPv4 ones for now.
The `WaitSelect` signature is fixed by `clib/bsdsocket_protos.h` (`AROS_LP6`):

```c
int WaitSelect(int nfds, fd_set *readfds, fd_set *writefds,
               fd_set *exceptfds, struct timeval *timeout, ULONG *sigmask);
/* regs D0,A0,A1,A2,A3,D1 ; SocketBase, LVO 21 */
```

## The concurrency model — the load-bearing constraint

This is the heart of the spec. Three intertwined requirements; each stands on an
independent justification.

### R-NONBLOCK — every host socket is non-blocking, always

**Requirement.** Set `O_NONBLOCK` on each socket immediately after `socket()` (and on any
fd returned by `accept()`), via `fcntl(fd, F_SETFL, O_NONBLOCK)` or `ioctl(fd, FIONBIO, 1)`.
A guest socket op never issues a *blocking* host syscall: every op that can block
(`connect`, `recv`, `send` on a full buffer, `accept`) returns immediately, and the library
turns a "would block" result into "the AROS task waits" (R-PARK).

**Justification `[OURS]` + `[PUB]`.** AROS runs on a **single underlying execution thread**
driven by SIGALRM (H4/H6, `hosted/exec.c`); one blocking host syscall on that thread freezes
*all* AROS tasks. POSIX guarantees a non-blocking socket fails fast with `EWOULDBLOCK`/
`EAGAIN` (or `EINPROGRESS` for `connect`) instead of sleeping in the kernel `[PUB]`
(`fcntl(2)`, `connect(2)`). The in-tree mingw32 port reaches the same conclusion by a
different route — it sets sockets non-blocking implicitly at creation
(`arch/all-mingw32/bsdsocket/socket.c`) `[AROS]`. `[DERIVED]`: that a host-socket
bsdsocket must use non-blocking host fds and absorb the wait elsewhere — independently
derived; the requirement stands on H6 + POSIX alone.

### R-PUMP — one kqueue host thread converts fd-readiness into an AROS Signal

**Requirement.** A single dedicated **host pthread** owns a `kqueue` fd and blocks in
`kevent()`. It maintains a registration set: `(host fd, direction, target SocketBase)`. When
the kernel reports a registered fd ready (`EVFILT_READ` / `EVFILT_WRITE`), the pump:

1. records which fd(s)/direction(s) became ready, keyed by target `SocketBase` (so a later
   `WaitSelect`/`recv` can learn *which* fd woke it without a full re-probe), then
2. raises the target task's **per-base `readySig`** via `Signal(task, readySig)` `[AROS]`,
3. returns to `kevent()`.

The pump **never** touches socket *data* (no `recv` in the pump) and **never** runs AROS
code — it only waits in the kernel and signals. It must itself be wakeable to (re)register
fds and to shut down: include a self-pipe (or an `EVFILT_USER` trigger) so the AROS side's
`pump_register`/`pump_unregister`/shutdown can interrupt a blocked `kevent()` `[PUB]`
(`kqueue(2)` `EVFILT_USER`; the self-pipe trick).

Use **edge-or-level deliberately**: register with `kevent()` defaults (level-triggered
readiness) so a readiness that arrives before the task parks is not lost across a
re-registration; the lost-wakeup guard is R-RACE.

**Justification `[OURS]` + `[PUB]` + `[AROS]`.** A blocking `kevent()` must run on a *real*
OS thread so it never parks AROS's single underlying thread — H4/H6 assume exactly one
SIGALRM-driven execution thread, and an *additional* host thread that only sleeps in the
kernel and signals does not violate that (it executes no AROS scheduler code) `[OURS]`. The
`Signal()` it raises is the in-tree exec primitive (`hosted/signal.c`, grounded against
`rom/exec/signal.c`), and it is safe to call from the pump for the same reason the device
task signals across the H10 boundary: `Signal` is `Forbid`-bracketed and on one underlying
thread a compiler barrier orders it (H6) `[OURS]`/`[AROS]`. `kqueue`/`kevent` are the
documented macOS readiness primitives `[PUB]`. This *replaces* mingw32's
`ResolverThread`+`KrnCauseSystemIRQ` (`host_socket.c`) — a Windows event-object mechanism
with no darwin analogue `[AROS]`. `[DERIVED]`: that readiness is delivered to the guest by
raising an exec `Signal` from outside the guest's normal flow, and that one pump thread
suffices rather than a thread per socket — independently derived; our mechanism (kqueue +
`exec.Signal`) follows from H4/H6/H9 + macOS `kqueue` docs.

### R-DARWIN-WAKE — host-thread `Signal` is UNSAFE on darwin; poll the stash on the timer

**Finding (2026-06-28, grounded — overrides the `Signal`-from-pump mechanism in R-PUMP
step 2 / R-PARK / R-WAITSELECT on darwin-aarch64).** On this hosted port a foreign
host thread (or host interrupt context) **must not** raise an `exec` `Signal` into an
AROS task. The two proven darwin drivers both discovered this and both resolve it by
**polling `timer.device` (`Delay()`)** instead of taking a host-context wake:

- `arch/all-darwin/hidd/cocoa/cocoa_input.c:546` — verbatim: *"Poll on the timer
  (`Delay`) rather than a VBlank interrupt server: a task woken from the host
  SIGALRM/VBlank interrupt context runs in 'supervisor mode' under the threaded darwin
  scheduler, which trips every semaphore op. The `timer.device` wakeup path is the one
  the rest of the boot uses safely."* `[AROS]`
- `arch/all-darwin/hidd/cocoa/cocoa_clipboard.c` — the **working** clipboard bridge is
  the *"polling variant — robust under the threaded host scheduler"*: a low-pri task
  polls `host_pb_change_count()` on a `Delay(10)` tick; the host poller never `Signal`s
  AROS. `[AROS]`/`[OURS]`

**Consequence for this feature.** Keep the kqueue pump exactly as built — it still does
the efficient in-kernel readiness wait and stashes results for `pump_drain` (so we are
*not* busy-polling the sockets). But the AROS side's "park" is **not** `Wait(readySig)`
woken by the pump; it is a **`timer.device` `Delay()` poll loop** that, each tick, calls
`pump_drain()` (the cheap atomic stash) and tests its `*sigmask`/`sigintr` bits via
`SetSignal(0,…)`, with the caller's `timeout` as the deadline. The pump's
`ps_create_cb` wake callback degrades to setting an `_Atomic` "ready" flag the poll reads
(R-W1) — it does **not** call `Signal`. Latency is one timer tick (~10–20 ms), which is
immaterial for sockets and is the price of safety on this port. This is the
hybrid: **kqueue for efficient readiness, timer-poll for the safe AROS handoff.**
`[DERIVED]` from the two `[AROS]` drivers above + H6; it supersedes the `Signal`-seam of
[host-wake-pattern.md](../host-wake-pattern.md) R-W2 *on darwin* (the seam's spike
self-pipe and graft stay valid on hosts where host-context `Signal` is safe).

### R-PARK — a would-block guest call becomes register-then-poll-then-retry

**Requirement.** When a host socket op returns `EWOULDBLOCK`/`EAGAIN` (or `EINPROGRESS` for
`connect`), the library does **not** spin and does **not** block the host. It:

1. `pump_register(host fd, want, sb)` — `want` = read for `recv`/`accept`, write for
   `send`/`connect`,
2. `Wait(readySig)` `[AROS]` — parks *this AROS task only* (other tasks keep running),
3. on wake, `pump_unregister` (or leave registered if more I/O is expected) and **re-issue
   the same non-blocking host op**, which now succeeds (or yields the next `EWOULDBLOCK`,
   looping).

`connect` is the special case: `EINPROGRESS` → register for **write**-readiness → on wake,
read `SO_ERROR` via `getsockopt` to learn success/failure `[PUB]` (`connect(2)`
"asynchronous error" idiom). A `timeout`/SIGALRM-counter must keep advancing throughout —
the proof that the underlying thread never blocked (spike [N3]).

**Justification `[OURS]`.** This is exactly the H11 "block for a reply, get signalled" loop
(`hosted/device.c`: the device task `WaitPort`s, does the host call, `ReplyMsg`s; the client
`Wait`s for the reply), with the kqueue pump playing the role of the device task's reply.
The `Wait`/`Signal` substrate is H9 (`hosted/signal.c`) and the port/handoff substrate is
H10 (`hosted/msgport.c`) `[OURS]`. `[PUB]` for the `connect`/`SO_ERROR` idiom.

### R-WAITSELECT — `WaitSelect` waits on fds *and* exec Signals at once (LVO 21)

**Requirement (the single hardest function — mingw32 left it `#warning TODO`,
`arch/all-mingw32/bsdsocket/waitselect.c`).** Implement LVO 21 to honour the AmiTCP
autodoc contract (`workbench/network/stacks/AROSTCP/bsdsocket/autodoc/auto_socket.c`)
`[AROS]`: examine the read/write/except `fd_set`s like `select()`, **and simultaneously**
wait on the exec Signals named in `*sigmask`. Steps:

1. Walk `readfds`/`writefds`/`exceptfds` (bit-tested with `FD_ISSET`, per
   `sys/net_types.h`), mapping each set AROS fd → its host fd via `dTable`, and
   `pump_register` each with its direction.
2. Form the wake mask = `readySig | *sigmask` (and the per-task `sigintr`, i.e.
   `SIGBREAKF_CTRL_C`, so a break interrupts the wait — autodoc semantics). If `timeout`
   is non-NULL, arm a timed wake (a one-shot exec `timer.device` request or a kqueue
   `EVFILT_TIMER` registered with the pump; either raises a distinct bit into the mask).
3. `Wait(readySig | *sigmask | sigintr | timerSig)` `[AROS]`.
4. On wake, determine the source(s):
   - **fd source:** rebuild the output `fd_set`s. Either read the readiness the pump
     stashed for this `SocketBase` (`pump_drain`), or do a **zero-timeout non-blocking
     re-probe** of the registered fds (a `kevent` with a zero timespec, or per-fd
     `FIONREAD`/`POLLOUT` test) — clearing bits for fds not actually ready. The non-blocking
     re-probe is the source of truth so a stale/level event can't report a false ready.
   - **signal source:** rewrite `*sigmask` (the in/out arg) to **the subset of the caller's
     original `*sigmask` that actually arrived** — *not* `readySig`, *not* `sigintr`.
   - `pump_unregister` every fd registered in step 1.
5. Return value, per the autodoc: the **count of ready fds**; `0` if the timeout expired
   *or* the wait was broken by a `sigmask` signal / break with no fd ready; `-1` + per-task
   errno on error. When a `*sigmask` signal or break preempts with no fd ready, **clear all
   output `fd_set`s** before returning 0.

**Justification `[AROS]` + `[OURS]`.** The behavioural contract (examine fds + wait on
`*sigmask`; in/out 6th arg; count/0/-1 return; clear-fds-on-signal) is the published AmiTCP
autodoc semantics in the AROS tree (`.../autodoc/auto_socket.c`) `[AROS]` — it is the
external standard. The *implementation* — translate the fd_sets into
pump registrations, combine with `*sigmask`, single `Wait`, then re-probe — is the H9
`Wait`/H10 port pattern composed with R-PUMP `[OURS]`. `[DERIVED]`: that WaitSelect is
realised as "wait on a combined synthetic mask, then report which non-socket signals fired
in the out-mask, clearing fd_sets if a non-socket signal preempted" — independently derived
and consistent with the AROS autodoc; implemented here from the autodoc + H9.

### R-RACE — no lost wakeups when an fd event and an exec Signal race

**Requirement (the subtle part).** The window between "decide to park" and `Wait()` must not
drop a readiness or a `*sigmask` signal that arrives in it. Discipline:

- **Check before park.** `Wait()` already tests `tc_SigRecvd` *before* parking
  (`hosted/signal.c`), so a `Signal` raised by the pump *between* `pump_register` and `Wait`
  is still observed and `Wait` returns immediately — register **before** computing the mask,
  and never assume the fd "isn't ready yet."
- **Re-probe after wake, don't trust the wake.** Because the post-wake fd state is
  established by a non-blocking re-probe (R-WAITSELECT step 4), a spurious or coalesced
  `readySig` simply yields "no fd ready" and the caller loops (for blocking `recv`) or
  returns 0 (for `WaitSelect`). Correctness never depends on the *count* of signals, only on
  the actual fd state + the actual `tc_SigRecvd` bits.
- **Serialise pump bookkeeping.** `pump_register`/`pump_unregister`/`pump_drain` mutate
  shared state read by the pump thread; bracket each with `Forbid()`/`Permit()` — and note
  the H6 rule: on the single underlying thread a **compiler barrier** suffices for AROS-task
  vs AROS-task ordering, but the **pump is a real second OS thread**, so shared pump state
  needs a genuine `pthread_mutex` (or a lock-free self-pipe handoff), *not* merely
  `Forbid` — this is the one place the H6 compiler-barrier shortcut does **not** apply.

**Justification `[OURS]`.** The "check `tc_SigRecvd` before parking" guarantee is H9
(`hosted/signal.c`, grounded in `rom/exec/signal.c`); the project explicitly records that a
Signal racing ahead of `Wait` is still seen (NOTES.md H9) `[OURS]`. The H6 caveat — that the
`Forbid` compiler-barrier shortcut holds only *within* the single underlying thread and a
real second OS thread needs a true lock — follows directly from the H6 lesson (a write that
must be visible across a true thread boundary needs more than a compiler barrier) `[OURS]`.
`[DERIVED]`: that the signal-queue/readiness handoff needs a real critical section and the
race is resolved by having the task check both sources after wake — both conclusions
independently derived from H6 + H9.

## errno translation — `[AROS]`

Host BSD errno (macOS values) → **AmiTCP** errno (the AROS values in
`workbench/network/common/include/sys/errno.h`) via a small fixed table, then written through
the per-task `errnoPtr` at width `errnoSize` ∈ {1,2,4} (the mingw32 port does the analogous
offset `SetError(err - WSABASEERR, …)`; ours is a value-map, not an offset) `[AROS]`. AmiTCP
numbering overlaps BSD but is **not** identical — translate, never pass through. Verified
AROS values (from `sys/errno.h`) and their macOS counterparts (`/usr/include/sys/errno.h`,
`[PUB]`):

| Symbol | AROS (AmiTCP) | macOS | Map needed? |
|--------|--------------:|------:|-------------|
| `EAGAIN`/`EWOULDBLOCK` | 35 | 35 | identity |
| `EINPROGRESS` | 36 | 36 | identity |
| `EALREADY` | 37 | 37 | identity |
| `ECONNRESET` | **54** | 54 | identity |
| `EISCONN` | 56 | 56 | identity |
| `ENOTCONN` | 57 | 57 | identity |
| `ETIMEDOUT` | 60 | 60 | identity |
| `ECONNREFUSED` | 61 | 61 | identity |
| `EBADF` | 9 | 9 | identity |
| `EINVAL` | 22 | 22 | identity |
| `EOPNOTSUPP` | 45 | **102** | **map 102→45 — NON-identity** |

Many AmiTCP values were taken *from* 4.3BSD, so the socket-range table is **mostly identity**
on macOS (which is also BSD-derived) — but the low range (file/process errnos) diverges and a
handful of socket errnos may too. **Do not assume identity:** build the table explicitly,
entry by entry, and let spike [N4] catch any mismatch.

> **[N4]/[NERR] result (confirmed 2026-06-28):** the explicit table earned its keep — macOS
> renumbered **`EOPNOTSUPP` to 102** (out of the BSD range), while AmiTCP keeps it at **45**, so
> a socket op returning `EOPNOTSUPP` on Darwin **must** be mapped `102 → 45` (a raw passthrough
> would corrupt it). Everything else in the BSD socket range (35–65) and the common low errnos
> verified identity. Table + host unit test: `hosted/bsdsocket/errno_xlate.{c,h}` +
> `errno_test.c` (`make bsdsock-errno`, 25/25). The table is pure int→int (no `errno.h`
> dependency) so the same file compiles in the host test and the AROS module. `errnoSize` may be 1/2/4 — store with
the right width (`SBTC_ERRNOBYTEPTR`/`WORDPTR`/`LONGPTR` tags, `socketbasetags.h`). Resolver
errors map to the per-task `h_errno` (`SBTC_HERRNO`/`SBTC_HERRNOLONGPTR`).

> Note for Role B: the AROS *value comment* in design.md said `ECONNRESET=61`; the header
> actually defines `ECONNRESET=54` and `ECONNREFUSED=61`. Trust the header, not the prose.

## Address & data marshalling — `[AROS]` + `[PUB]`

- **`sockaddr` is near-verbatim.** AROS's `struct sockaddr { uint8_t sa_len; sa_family_t
  sa_family; char sa_data[14]; }` and `struct sockaddr_in { uint8_t sin_len; sa_family_t
  sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; }`
  (`sys/socket.h`, `netinet/in.h`) carry the BSD `sa_len`/`sin_len` byte and
  network-byte-order `sin_port`, the **same** layout as macOS's BSD-derived `sockaddr_in`
  `[AROS]`/`[PUB]`. `AF_INET=2` matches macOS; `SOCK_STREAM=1`/`DGRAM=2`/`RAW=3` match;
  `IPPROTO_TCP=6` matches; `SOL_SOCKET=0xffff`. So the address copy is close to a `memcpy`
  — but **verify field widths at port time** (`sa_family_t` width; whether macOS sets
  `sa_len`) before trusting a raw copy. `AF_INET6` differs (AROS 28 vs macOS 30) — IPv4 only.
- **Buffers stay on the switched task stack.** `send`/`recv`/`sendto`/`recvfrom` operate
  directly on AROS-task memory, on whatever task called the LVO (a *switched* task stack
  under SIGALRM preemption) — exactly the H11 shape proven safe by `hosted/device.c` doing
  `pread`/`pwrite` on a switched stack `[OURS]`. No bounce buffer is required for the data
  path.
- **`fd_set` is the AROS layout, 64-wide.** `FD_SET`/`FD_CLR`/`FD_ISSET`/`FD_ZERO` and
  `FD_SETSIZE=64`, `fd_mask=long`, `NFDBITS=sizeof(long)*8` come from
  `workbench/network/common/include/sys/net_types.h` (**not** `sys/socket.h`) `[AROS]`.
  WaitSelect indexes by **AROS** fd; translate to host fds via `dTable` before any host
  `kevent`/`select`.

## The descriptor table (`dTable`) — `[AROS]` + `[OURS]`

`dTable` maps AROS fd numbers (0..`dTableSize`-1, default 64) → `{host fd, per-socket
flags, pump-registration state}`, so AROS fd numbering is decoupled from host fd numbering
(and `Dup2Socket` (LVO 44), and `ObtainSocket`/`ReleaseSocket` fd-handoff between tasks,
work as AmiTCP expects). `getdtablesize` (LVO 23) returns its size; `SBTC_DTABLESIZE` resizes
it. `socket()` allocates the lowest free AROS fd; `CloseSocket` `pump_unregister`s, closes
the host fd, and frees the slot. Bracket descriptor bookkeeping with `Forbid`/`Permit` — H6
compiler barrier for the AROS-side bits, but anything the pump thread also reads needs the
real lock (R-RACE). `[AROS]` for the contract (mingw32 `bsdsocket_open.c` `dTable`),
`[OURS]` for the pump-registration extension.

## Unattended verification — `[OURS]` H11 discipline

The whole suite is hermetic and TCC-free because **the server is on the same Mac, on
localhost**, started by the harness itself. Localhost loopback needs no entitlement on macOS
(unlike raw BPF/`utun`). Each marker is one runnable binary printing one `[N#] PASS/FAIL`,
driven by the existing bash watchdog (TERM-then-KILL, since stock macOS has no GNU `timeout`
— NOTES.md "portable timeout"), reaping a hung connect into a FAIL. Every spike asserts
**values**, never "it didn't crash." The harness asserts at **both** ends (the host echo
server logs the bytes it echoed; AROS asserts `recv == sent`) — the H11 two-sided check
(`hosted/device.c`: client checks `read==write` *and* main re-reads through the host).

- **[N1] `socket`/`connect`/`send`/`recv` round-trip.** Harness `fork`s a localhost TCP
  echo server bound to `127.0.0.1:0`, reads back the OS-assigned port, passes it to AROS
  (the channel `run.sh`/`bootrun.sh` already use). AROS (initially the standalone H11
  scaffold + the host-socket interface): `socket(AF_INET,SOCK_STREAM,0)` → `connect` →
  `send("PING")` → `recv` → assert bytes == `PING`. **PASS** = exact round-trip; FAIL =
  mismatch/timeout. Proves the H3 shim carries socket syscalls and the fd/buffer marshalling
  is correct.
- **[N2] `WaitSelect` over multiple fds + an exec Signal — the load-bearing spike.** Open
  two sockets to the echo server; harness sends on one then the other with a gap. AROS
  `WaitSelect`s both read fds **plus** a self-`Signal` bit. Assert: WaitSelect wakes on the
  right fd each time, returns the correct ready count, rebuilds the `fd_set`s, **and** wakes
  on the exec Signal with no fd ready (returns 0, `*sigmask` rewritten to exactly the bit
  that fired). **PASS** = correct wake source every time **and** a concurrent SIGALRM counter
  keeps ticking (the underlying thread never blocked). This is R-PUMP↔R-WAITSELECT↔R-RACE
  end to end.
- **[N3] non-blocking connect + slow read (no thread freeze).** Point AROS at a server the
  harness deliberately makes slow to accept / dribbles bytes. Assert `connect` returns (maps
  to) `EINPROGRESS`, the **task** parks but the **process** does not (the preempt counter
  keeps advancing), then completes when the pump signals. Proves R-NONBLOCK + R-PARK under
  real latency.
- **[N4] errno fidelity.** `connect` to a closed port → assert AROS `Errno()` returns AROS
  `ECONNREFUSED` (61), not a raw/unmapped value; a `recv` on a would-block socket surfaces
  `EWOULDBLOCK` (35). Proves the translation table (and catches any non-identity entry).
- **[N5] graft into the real `bsdsocket.library`.** Build `arch/all-unix/bsdsocket/` as a
  real AROS module (per-task `OpenLibrary` → `SocketBase`, the real LVO jump table) and
  rerun [N1]/[N2] **through the library vectors** (`__AROS_GETVECADDR(SocketBase, LVO)`).
  PASS = same round-trip, now via LVO dispatch.
- **[N6] a tiny real fetch (stretch, network-gated).** Once the resolver decision lands
  (§"Resolver"): resolve + `connect` to a real host, send a minimal `HTTP/1.0 GET`, assert a
  `200`/known bytes in the reply. Gated behind network availability so the core suite stays
  hermetic on localhost.

## Resolver (`gethostbyname`, LVO 35) — `[AROS]` + `[OURS]`, deferred

`gethostbyname`/`getaddrinfo` can block in the host resolver for seconds — the same
"never block the underlying thread" hazard as a socket op (R-NONBLOCK), but there is no
non-blocking `getaddrinfo`. **Plan:** run the blocking resolver call on the pump thread (or a
second helper thread) and signal completion into the calling task's `readySig` — the same
R-PARK bridge, just with a blocking host call confined to a real OS thread. Map resolver
errors to per-task `h_errno`. **Deferred past the core suite** (localhost needs no DNS); [N6]
forces the decision. **UNVERIFIED:** whether calling `getaddrinfo` via `libSystem` from the
pump thread while the calling AROS task holds `Forbid()` is safe — check at port time. This
mirrors the role of mingw32's `ResolverThread` (`host_socket.c`) but reuses the kqueue pump
rather than a Windows event object `[AROS]`/`[OURS]`.

## Build / integration

- **Module location (decided): `arch/all-unix/bsdsocket/`**, with the readiness primitive
  isolated in a single `readiness_kqueue.c` (the one swappable file). A tree-wide search
  confirms **no host-passthrough `bsdsocket` exists for any unix host** — only Windows
  `arch/all-mingw32/bsdsocket/`; the unix hosts have only the Linux-only SANA-II NIC route
  (native AROSTCP). So the host-neutral core here — per-task base, `dTable`, the LVO bodies,
  errno discipline, and the `WaitSelect`↔`Signal` bridge mingw32 left as a `#warning TODO` —
  is net-new for AROS **and benefits every hosted AROS flavour**: swap `kqueue`→`epoll`/`poll`
  in `readiness_kqueue.c` and `linux-*` hosts get the same module. We build and verify **only**
  the darwin/kqueue backend in our loop (the Linux `epoll` backend is a bounded follow-on, not
  claimed here). Per-host specifics beyond the readiness file are small and isolated: the errno
  value table (Linux errno ≠ BSD/AmiTCP) and the `sockaddr` `sa_len` byte (BSD has it, Linux
  doesn't).
- **Copy host-neutral, replace host-specific.** Copy `bsdsocket.conf` verbatim; port the
  `TaskBase`/`OpenLib`/`CloseLib`/`dTable` shape from mingw32; **rewrite** the host interface
  build (`bsdsocket_init.c` → libc instead of Winsock), the per-call host invocation, and
  **delete** the Windows `host_socket.c` (`ResolverThread`/`KrnCauseSystemIRQ`) in favour of
  the kqueue pump.
- **Host-call crossing.** Every libc socket call and every pump libc call crosses the H3
  shim (`hosted/abishim.S`, `hosted/host.c`); resolve libc symbols via `hostlib.resource`
  (`arch/all-hosted/hostlib/`).
- **Where it slots in the boot.** Hosted AROS now boots the full module set and `dos.library`
  to a Wanderer desktop (`graft/WORKFLOW.md`, root README), so this no longer gates on
  bring-up. `bsdsocket.library` is a normal (non-resident) disk-based library — it loads
  **after** the `dos.library` bring-up (WORKFLOW item F2/23). The feature is designed/grounded;
  it lands on top of the booted OS. Spike-phase ([N1]–[N4]) runs against the standalone `hosted/`
  scaffold (no kickstart dependency), exactly as `hosted/device.c` does for H11.
- The AROS side pulls **no** macOS socket headers (it uses AROS's `sys/socket.h` etc.); the
  host calls are reached only as opaque libc function pointers through hostlib + the H3 shim.

## Open questions / UNVERIFIED

- **R-RACE pump locking.** The exact synchronisation between the AROS side and the real pump
  thread (a `pthread_mutex` around the registration set vs. a lock-free self-pipe command
  queue) — both are sound; pick at implementation time and prove with [N2]'s concurrent
  counter. The H6 compiler-barrier shortcut explicitly does **not** cover this true-thread
  boundary.
- **kqueue level vs. edge & re-registration cost.** Whether to keep fds permanently
  registered (and only toggle the wanted filters) or register-per-wait; level-triggering is
  assumed to avoid lost-readiness, **UNVERIFIED** against `EV_CLEAR` semantics under our
  re-probe.
- **`Forbid()` across a host call.** Whether holding `Forbid()` across a libc call that can
  internally take a libc lock (resolver, allocator) is safe on darwin — design.md flags this
  for `getaddrinfo`; confirm for the data path too.
- **`sockaddr` field widths.** `sa_family_t` width and whether macOS populates `sa_len`
  on return — verify before trusting a raw `sockaddr` copy.
- **IPv6.** AROS `AF_INET6=28` vs macOS `AF_INET6=30`; `sockaddr_in6` layout — IPv4-first,
  IPv6 is a follow-on, not a bring-up blocker.
- **errno table completeness.** The socket-range map is mostly identity on macOS, but the
  full table (incl. file/process errnos that may leak through, e.g. `EBADF`) must be built
  explicitly and exercised; [N4] covers the common ones only.
- **`timeout` realisation in WaitSelect.** Whether to arm the timeout via `timer.device` or
  a kqueue `EVFILT_TIMER` registered with the pump — both raise a distinct mask bit;
  unprototyped, decide at [N2].
- **Resolver thread + `Forbid` safety** (above), and whether the pump thread is the right
  home for it vs. a second helper thread.

## Spike status vs production contract

The `hosted/bsdsocket/` spike proves the mechanism ([N1]–[N3] PASS); a few of its
seams are deliberately host-side stand-ins for the AROS-side production contract,
and one design question is now **settled**.

- **Shared wake contract (`host-wake`).** The "a foreign host thread wakes an
  AROS task" pattern here — atomics across the true-thread boundary, single
  ownership of the wake target, and no lost wakeup between "decide to park" and
  `Wait()` — is the **same** contract recurring across kqueue (this feature),
  the pasteboard bridge, and CoreAudio. The rules are written once as `host-wake`
  and referenced, not re-derived per feature: (1) any flag shared between the
  pump/helper thread and the AROS side is `_Atomic` (or mutex-guarded) — the H6
  `Forbid` compiler-barrier shortcut does **not** cross a real OS-thread boundary
  (R-RACE); (2) check-`tc_SigRecvd`-before-park + re-probe-after-wake make a
  coalesced/spurious wake harmless (R-RACE); (3) the readiness-signal seam
  (`PumpSig` → AROS `Signal`) is the single swap point — the spike's self-pipe
  `ps_wait`/`ps_wake` becomes `Wait(readySig)`/`Signal(task, readySig)` with no
  change to the pump logic.

- **SETTLED — single-owner-fd.** A host fd is registered with the pump by
  **exactly one** waiter (`PumpSig` in the spike, `SocketBase` in the graft).
  This is sound because a socket descriptor belongs to exactly one AROS task (the
  per-task `dTable`/`SocketBase` contract above): two tasks never share a host fd
  in the pump. The contract is now **guarded, not implicit** — `pump_register`
  rejects (returns -1) an fd already owned by a different waiter, and
  `pump_unregister` is owner-matched so one waiter's teardown can never delete
  another's kqueue filters. (Per-`(fd,filter)` refcounted fan-out is therefore
  **not** needed and is explicitly out of scope; if a future cross-task fd
  hand-off via `ObtainSocket`/`ReleaseSocket` ever needs shared registration, it
  re-registers under the new owner's `SocketBase` rather than fanning out.)

- **Spike stand-ins (become AROS-side at graft).** The `PumpSig` self-pipe wake
  → `exec` `Signal`/`Wait`; the spike's `hs_send` returns a single-call,
  possibly-**partial** BSD byte count (the write-all loop is the separate
  `hs_send_all` helper) so the `send()` LVO preserves the partial-return
  semantics AROS apps depend on; host errno is passed through (the AmiTCP errno
  translation table, §"errno translation", is applied AROS-side, not in the
  spike).

## Provenance summary

`[PUB]` POSIX BSD sockets (`socket`/`connect`/`recv`/…, `fcntl` `O_NONBLOCK`, `connect`
`EINPROGRESS`/`SO_ERROR`); macOS `kqueue(2)`/`kevent(2)`/`EVFILT_*`; `getaddrinfo`; macOS
`<sys/errno.h>` values. · `[AROS]` `workbench/network/common/include/` (`defines/bsdsocket.h`
LVO map, `clib/bsdsocket_protos.h`, `sys/net_types.h` `fd_set`/`FD_SETSIZE=64`, `sys/errno.h`
AmiTCP values, `sys/socket.h`, `netinet/in.h`, `bsdsocket/socketbasetags.h`),
`workbench/network/stacks/AROSTCP/bsdsocket/{api/amiga_api.h, autodoc/auto_socket.c}`
(SocketBase + WaitSelect autodoc semantics), `arch/all-mingw32/bsdsocket/`
(`bsdsocket.conf`, `bsdsocket_open.c`/`bsdsocket_intern.h` per-task base + `dTable`,
`socket.c` non-blocking/errno discipline — host-neutral parts only; `waitselect.c`/`recv.c`/
`connect.c` are the `#warning TODO` stubs we fill; `host_socket.c` is the Windows part we
replace), `arch/all-unix/hidd/unixio/{unixio.h, unixio_class.c}` (`libSystem.dylib` libc
symbol plumbing), `arch/all-hosted/hostlib/` (`hostlib.resource`). · `[OURS]`
`hosted/device.c` (H11 host-call-on-a-switched-task, block-then-reply, two-sided verify),
`hosted/signal.c` (H9 Wait/Signal, check-`tc_SigRecvd`-before-park), `hosted/msgport.c`
(H10 ports), `hosted/abishim.S`+`hosted/host.c` (H3 host-call shim), `hosted/exec.c` (H4/H6
single underlying thread + SIGALRM + the `Forbid` compiler-barrier rule and its true-thread
limit), `NOTES.md` (H-series + "portable timeout" watchdog + "ground it, don't dream it"),
`graft/WORKFLOW.md` (boot state / where the library slots). · `[DERIVED]` the
non-blocking-host-I/O → pump → raise an exec `Signal` → `WaitSelect` bridge, that one pump
thread suffices over a thread-per-socket, and the lost-wakeup race — each independently
derived above from H4/H6/H9/H11 + the AmiTCP autodoc + POSIX/macOS `kqueue` docs.
Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted in producing it, and any resemblance to
existing implementations is coincidental.
