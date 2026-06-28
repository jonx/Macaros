# Host BSD sockets — real networking via bsdsocket.library

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-24

## What & why

Give the hosted AROS working TCP/IP *immediately* by mapping AROS's
`bsdsocket.library` (the AmigaOS networking API) onto macOS's native BSD sockets.
The Mac's kernel stack does the real work — routing, TCP state machine, DNS,
the NIC — and AROS code reaches it through the standard `socket()/connect()/
send()/recv()/WaitSelect()` LVO calls it already expects. This is the Phase-2
thesis (NOTES.md "H11") applied to the network: *macOS owns the driver; AROS
reaches it via standard exec calls.*

The alternative — bring up AROS's own TCP/IP stack (AROSTCP) on an emulated NIC —
is a far larger effort and adds nothing for a *hosted* port: the host already has
a battle-tested stack one syscall away. We forward, we don't reimplement.

Concretely this unlocks the unattended-loop win: the harness runs a localhost
echo server on the Mac and asserts that AROS `connect()`s, `send()`s and
`recv()`s the exact bytes back — fully programmatic, no TCC, no manual step.

## Does it already exist?

Partly, in two non-overlapping pieces — and *neither* is a unix/darwin
host-socket `bsdsocket.library`. Honest verdict: **the feature is net-new code for
darwin, but there is a near-complete same-shape template to port from.**

1. **There is a host-passthrough `bsdsocket.library` — but it is Windows-only.**
   `arch/all-mingw32/bsdsocket/` is a full library that forwards `socket/bind/
   connect/send/recv/...` straight to the host's Winsock (`Ws2_32.dll`) via
   `hostlib.resource`. It has the right *shape* for what darwin needs:
   - `bsdsocket.conf` declares the entire BSD socket LVO functionlist with the
     exact register mapping (`arch/all-mingw32/bsdsocket/bsdsocket.conf`).
   - `bsdsocket_init.c` opens the host lib and builds a function-pointer interface
     (`HostLib_Open("Ws2_32.dll")` → `HostLib_GetInterface(...)` →
     `struct WinSockInterface`), grounded in `arch/all-hosted/hostlib/`.
   - Per-task base contract is real: `BSDSocket_OpenLib` makes a per-task
     `TaskBase` with its own `dTable`, `errnoPtr`, `sigintr`
     (`arch/all-mingw32/bsdsocket/bsdsocket_open.c`).
   - The host-side `socket()` sets the descriptor **non-blocking** at creation
     (`WSAEventSelect(...)` "implies setting the socket to non-blocking mode" —
     `arch/all-mingw32/bsdsocket/socket.c`), guarded by `Forbid()/Permit()`.

   **But its async/blocking story is unfinished and Windows-specific:**
   `WaitSelect`, `recv`, `connect` and several others are literally
   `aros_print_not_implemented(...)` / `#warning TODO` stubs today
   (`arch/all-mingw32/bsdsocket/waitselect.c`, `recv.c`, `connect.c`). Readiness
   is delivered by a host helper thread (`ResolverThread`) plus
   `KrnCauseSystemIRQ` (`arch/all-mingw32/bsdsocket/host_socket.c`) — a Windows
   event-object mechanism with no darwin analogue. So even the existing template
   leaves the hard part (WaitSelect ↔ AROS signals without blocking) as TODO.

2. **For *unix* hosts, the only host-backed networking is a SANA-II NIC, not a
   bsdsocket forwarder — and it's Linux-only.**
   `arch/all-unix/devs/networks/eth/` and `.../tap/` are SANA-II ethernet drivers
   that open a host raw/`tun` socket (so AROS runs its *own* AROSTCP stack on top).
   The actual host-socket code lives in
   `arch/all-unix/hidd/unixio/unixpkt_class.c` and is hard-gated `#ifdef
   HOST_OS_linux` (`PF_PACKET`/`AF_PACKET`, `/dev/net/tun`, `<linux/if_tun.h>`);
   the `#else` branches return `NULL`. **Darwin is not covered.** This is the
   "run AROS's own TCP/IP" route, which we are deliberately *not* taking.

3. **AROS's own TCP/IP stack exists but is irrelevant to this feature.**
   `workbench/network/stacks/AROSTCP/` (incl. `bsdsocket/api/...`, the
   AmiTCP/Roadshow-derived `bsdsocket.library` that sits on a SANA-II driver) is a
   real in-AROS stack with no host calls. We bypass it entirely.

**The one thing that *is* already darwin-ready:** the host libc socket symbols
already resolve on macOS. `arch/all-unix/hidd/unixio/unixio.h` maps
`HOST_OS_darwin` → `LIBC_NAME "libSystem.dylib"`, and unixio's `libc_symbols[]`
table imports `"socket"`, `"sendto"`, `"recvfrom"`, `"bind"` unconditionally
across unix hosts (`arch/all-unix/hidd/unixio/unixio_class.c`). So the
*plumbing to call host sockets from AROS* is present; only the library that
*uses* it for `bsdsocket` semantics is missing on darwin.

**What darwin-aarch64 needs (net-new):** a new host-passthrough
`bsdsocket.library` for the unix/darwin host — structurally a port of the
mingw32 one, but (a) targeting `libSystem.dylib` BSD sockets instead of Winsock,
and (b) with a real `WaitSelect` built on **kqueue + a host pump thread that
raises an AROS Signal**, replacing the Windows `KrnCauseSystemIRQ`/event-object
path. That async bridge is the genuine design content below.

**On prior art.** The "forward `bsdsocket.library` to the host's native BSD
sockets" approach is a natural design — mapping the AmiTCP socket LVOs onto the
host's `socket(2)` is the obvious shape for a hosted port — and we determined it
independently from the AmiTCP autodoc semantics + POSIX. Independent work: no
third-party implementation source — emulator, agent, driver, or otherwise — was
read, searched, or consulted in producing it, and any resemblance to existing
implementations is coincidental.

The design content below — a **per-task `SocketBase`**, non-blocking host sockets,
a pump that delivers host readiness back by raising an exec `Signal`, and a
`WaitSelect` that waits on fds *and* exec Signals at once — is derived from the
AmiTCP autodoc (`auto_socket.c`), the in-tree mingw32 shape, and this project's
H-series spikes; see "Background" and "Design".

This reframes the in-tree verdict only mildly: the AROS *tree* still has no
unix/darwin host-socket `bsdsocket` (confirmed — see below), but the *approach* is
sound and the WaitSelect↔Signal bridge that mingw32 left stubbed is the genuine
new design content here. No host-socket `bsdsocket` for hosted-AROS specifically
(Linux or darwin) exists in the tree — the in-tree hosted-AROS networking path is
AROSTCP-on-SANA-II, confirming the gap is real and not merely uncloned.

## Background: the AROS bsdsocket.library contract (grounded)

All header paths under `workbench/network/common/include/`. The contract is
AmiTCP/Roadshow — apps written to it are legion (Amiga networking is this API).

**The library + its LVOs.** The authoritative LVO map is
`defines/bsdsocket.h` (each function's offset via `AROS_LC<N>(...,<LVO>,
BSDSocket)`); prototypes in `clib/bsdsocket_protos.h`; the public umbrella in
`proto/bsdsocket.h` (which declares `extern struct Library *SocketBase;`). The
cleanest single machine-readable functionlist with the register mapping is
`arch/all-mingw32/bsdsocket/bsdsocket.conf`. Core entries we must implement:

| LVO | Function | Regs |
|----:|----------|------|
| 1  | `Open` (per-task base) | D0 |
| 2  | `Close` | — |
| 5  | `socket(domain,type,protocol)` | D0,D1,D2 |
| 6  | `bind(s,name,namelen)` | D0,A0,D1 |
| 7  | `listen(s,backlog)` | D0,D1 |
| 8  | `accept(s,addr,addrlen)` | D0,A0,A1 |
| 9  | `connect(s,name,namelen)` | D0,A0,D1 |
| 10 | `sendto(...)` | D0,A0,D1,D2,A1,D3 |
| 11 | `send(s,msg,len,flags)` | D0,A0,D1,D2 |
| 12 | `recvfrom(...)` | D0,A0,D1,D2,A1,A2 |
| 13 | `recv(s,buf,len,flags)` | D0,A0,D1,D2 |
| 14 | `shutdown(s,how)` | D0,D1 |
| 15 | `setsockopt(...)` | D0,D1,D2,A0,D3 |
| 16 | `getsockopt(...)` | D0,D1,D2,A0,A1 |
| 17 | `getsockname` | D0,A0,A1 |
| 18 | `getpeername` | D0,A0,A1 |
| 19 | `IoctlSocket(s,req,argp)` | D0,D1,A0 |
| 20 | `CloseSocket(s)` | D0 |
| 21 | `WaitSelect(nfds,r,w,e,tv,sigmask)` | D0,A0,A1,A2,A3,D1 |
| 22 | `SetSocketSignals(intr,io,urg)` | D0,D1,D2 |
| 23 | `getdtablesize()` | — |
| 27 | `Errno()` | — |
| 28 | `SetErrnoPtr(ptr,size)` | A0,D0 |
| 35 | `gethostbyname(name)` | A0 |
| 44 | `Dup2Socket(d1,d2)` | D0,D1 |
| 49 | `SocketBaseTagList(tags)` | A0 |

(LVOs 29–43 are the `inet_*`/`get*by*` resolvers; 45/46 sendmsg/recvmsg;
50 `GetSocketEvents`; Roadshow extensions at 61+ — all in `defines/bsdsocket.h`.)
`SocketBaseTags(...)` is the varargs wrapper over `SocketBaseTagList()`.
`select()` is `#define`d to `WaitSelect(...,NULL)` in `bsdsocket.conf`.

**SocketBase — the per-task open contract.** `bsdsocket.library` is unusual: each
task gets its *own* library base. `OpenLibrary("bsdsocket.library",...)` →
`Open` (LVO 1) → looks up the calling task; if absent it `MakeLibrary`s a
per-task base holding that task's `dTable` (descriptor table, default size
`FD_SETSIZE`), its `errnoPtr/errnoSize`, and its signal masks
(`sigintr = SIGBREAKF_CTRL_C`). Grounded two ways: the AROSTCP `struct
SocketBase` in `workbench/network/stacks/AROSTCP/bsdsocket/api/amiga_api.h`, and
the simpler mingw32 `struct TaskBase` + `BSDSocket_OpenLib` in
`arch/all-mingw32/bsdsocket/bsdsocket_intern.h` / `bsdsocket_open.c`. Consequence
for us: **every task doing socket I/O must `OpenLibrary` itself** — the fd table,
errno, and signal mask are per-task, not shared.

**WaitSelect — the crux.** `select()` plus an Amiga `Wait()` signal mask.
Signature (`clib/bsdsocket_protos.h`, `AROS_LP6`):
```c
long WaitSelect(long nfds, fd_set *readfds, fd_set *writefds,
                fd_set *exceptfds, struct timeval *timeout, ULONG *sigmask);
```
Semantics (autodoc
`workbench/network/stacks/AROSTCP/bsdsocket/autodoc/auto_socket.c`): it examines
the fd sets like `select()`, **and simultaneously waits on the exec Signals in
`*sigmask`**. The 6th arg is in/out — on return it is rewritten to the signals
that actually arrived. Return: count of ready fds; `0` = timeout expired *or*
interrupted by a break signal / a signal in `*sigmask`; `-1` + errno on error.
This is the AROS idiom for "block on the network *and* on a message at once" —
and it is exactly what a hosted forwarder cannot satisfy by calling a blocking
host `select()`, because that would freeze AROS's single underlying thread (see
Design). `fd_set`/`FD_SET`/`FD_SETSIZE`(=64)/`fd_mask`/`NFDBITS` come from
`workbench/network/common/include/sys/net_types.h` (not `sys/socket.h`).

**errno mapping.** AROS uses AmiTCP's BSD errno numbering, which is **not** the
Linux/macOS numbering — values must be translated, not passed through. From
`workbench/network/common/include/sys/errno.h`: `EAGAIN==EWOULDBLOCK==35`,
`EINPROGRESS=36`, `EALREADY=37`, `EISCONN=56`, `ECONNREFUSED=61`, etc. Storage is
per-task in `SocketBase->errnoPtr` (size `errnoSize` ∈ {1,2,4}); apps redirect it
with `SetErrnoPtr(&errno, sizeof errno)` (LVO 28) or the `SBTC_ERRNO*PTR` tags, and
read the latest with `Errno()` (LVO 27). The mingw32 port already does
host-errno→AROS-errno offsetting (`SetError(err - WSABASEERR, ...)` in
`socket.c`) — the darwin port does the analogous host-BSD→AmiTCP mapping (small
fixed table; see The bridge).

**Socket structs (present, darwin-compatible shapes).**
`workbench/network/common/include/sys/socket.h`:
`struct sockaddr { uint8_t sa_len; sa_family_t sa_family; char sa_data[14]; }`,
`SOCK_STREAM/DGRAM/RAW`, `AF_INET=2`, `AF_INET6=28`, `SOL_SOCKET=0xffff`.
`netinet/in.h`: `struct sockaddr_in { uint8_t sin_len; sa_family_t sin_family;
in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; }`,
`IPPROTO_TCP=6`. Note AROS's `sockaddr_in` carries a BSD `sin_len` byte — the
**same** layout as macOS's `sockaddr_in` (BSD-derived), so the address copy is
close to verbatim. `AF_INET=2` matches macOS; the family enum needs only a sanity
check, not a remap (`AF_INET6` differs across OSes — verify at port time —
**UNVERIFIED** whether AROS's 28 equals macOS's 30; for IPv4-first bring-up it's
moot).

**The host-call-on-a-switched-task pattern (grounded, H11).** A socket LVO runs
on whatever AROS task called it — a *switched* task stack, under SIGALRM
preemption. We already proved a host syscall can run safely there:
`hosted/device.c` (`device_task`) does the real `pread`/`pwrite` on a switched
task stack and the H1 ABI property holds. The blocking-vs-thread structure also
mirrors H11: the device task `WaitPort`s for an `IORequest`, does the host call,
`ReplyMsg`s (`hosted/device.c:164`), and the host-call shim is the same one from
H3 (`hosted/abishim.S` / `hosted/host.c`). bsdsocket reuses that exact machinery.

## Design

### Host side (macOS BSD sockets + kqueue pump)

The host work is plain `libSystem.dylib` BSD sockets — the API is essentially
identical to Amiga's (Amiga's *is* BSD sockets). Two pieces:

1. **The forwarding calls.** `socket/bind/listen/connect/send/recv/sendto/
   recvfrom/setsockopt/...` resolved from `libSystem.dylib` via
   `hostlib.resource` — the same `HostLib_Open`/`HostLib_GetInterface` path the
   mingw32 port uses for Winsock (`arch/all-mingw32/bsdsocket/bsdsocket_init.c`),
   retargeted to libc. (The symbols already resolve on darwin —
   `arch/all-unix/hidd/unixio/unixio.h`, `unixio_class.c` libc_symbols.) Each call
   goes through the H3 host-call shim (`hosted/abishim.S`) since it crosses the
   AAPCS64→Apple-arm64 ABI boundary.

2. **The kqueue pump — a dedicated host pthread.** Every AROS-visible socket is
   created with `O_NONBLOCK` (set right after `socket()`, as mingw32 does
   implicitly). A single host thread runs `kevent()` on a `kqueue` fd, registering
   `EVFILT_READ`/`EVFILT_WRITE` for the fds AROS is currently `WaitSelect`-ing on.
   When the kernel reports readiness, the pump does **not** touch socket data — it
   only raises an AROS Signal into the waiting task (see The bridge). This is the
   darwin replacement for mingw32's `ResolverThread` + `KrnCauseSystemIRQ`
   (`arch/all-mingw32/bsdsocket/host_socket.c`); kqueue is the macOS-native
   readiness primitive (macOS has no `epoll`, and `poll()` on a thread would also
   work but kqueue scales and is the platform idiom).

   Why a host thread at all: blocking `kevent()` must run on a *real* OS thread so
   it never parks AROS's single underlying execution thread. The H4/H6 scheduler
   model already assumes one underlying thread driven by SIGALRM; the pump is an
   *additional* host thread whose only job is to wait in the kernel and signal —
   it never runs AROS code, so it doesn't perturb the scheduler invariants.

### AROS side (bsdsocket.library backed by the host)

A new library, `arch/all-unix/bsdsocket/` (the readiness primitive isolated so only
`readiness_kqueue.c` is darwin-specific — see "Cross-host reuse and the in-tree
gap"), structured as a port of `arch/all-mingw32/bsdsocket/`:

- **Same LVO functionlist** — copy `bsdsocket.conf` verbatim (it's host-neutral).
- **Same per-task base** — `BSDSocket_OpenLib`/`CloseLib` and `struct TaskBase`
  (`dTable`, `errnoPtr`, `sigintr`) port unchanged from
  `arch/all-mingw32/bsdsocket/bsdsocket_open.c` / `bsdsocket_intern.h`. Replace the
  `WinSockInterface` with a `HostSockInterface` of libc function pointers.
- **Each socket LVO** = marshal args → call the host fn through the shim → map the
  result and host errno into the per-task `errnoPtr`. The data-moving calls
  (`send`/`recv`/`sendto`/`recvfrom`) operate on AROS-task buffers directly, on the
  switched task stack — exactly the H11 shape (`hosted/device.c`).
- **The descriptor table** (`dTable`) maps AROS fd numbers → host fd + per-socket
  flags, so AROS fd 0..N is decoupled from host fd numbers (and `Dup2Socket`,
  `ObtainSocket`/`ReleaseSocket` for fd hand-off between tasks work as AmiTCP
  expects). `getdtablesize`/`SBTC_DTABLESIZE` size it.

`Forbid()/Permit()` brackets the host call sequence where a descriptor's bookkeeping
is mutated (as mingw32 does in `socket.c`), with the H6 caveat noted: on a single
underlying thread a **compiler barrier** suffices (NOTES.md H6), not a CPU fence.

### The bridge (non-blocking sockets ↔ WaitSelect/signals; errno translation)

Three problems, three grounded solutions:

1. **Never block the underlying thread.** All sockets are `O_NONBLOCK`. A blocking
   AROS call (`recv` on a stream with no data, `connect` to a not-yet-accepted
   peer) must translate "would block" into "AROS task waits" — not into a blocked
   host syscall. The mechanism:
   - The non-blocking host call returns immediately with `EWOULDBLOCK` (or
     `EINPROGRESS` for connect).
   - The library registers the fd's readiness need with the kqueue pump and does an
     exec `Wait(sigmask)` on a per-base signal bit (the H9 Wait/Signal machinery,
     `hosted/signal.c`, grounded in `rom/exec/{wait,signal}.c`).
   - The pump's `kevent()` fires on readiness → pump `Signal()`s the waiting task →
     the task re-issues the non-blocking call, which now succeeds.
   This is precisely the H10/H11 "block for a reply, get signalled" loop
   (`hosted/device.c` WaitPort/ReplyMsg), with the kqueue pump playing the role of
   the device task's reply.

2. **WaitSelect = kqueue + Wait on one signal.** Implement LVO 21 as: translate the
   `fd_set`s into kqueue registrations; arrange that the *exec Signals* in
   `*sigmask` are also waited; call `Wait(kqueueReadySig | *sigmask)`. On wake,
   determine whether the kqueue bit or a `*sigmask` bit arrived, rebuild the
   output `fd_set`s (a non-blocking probe of registered fds, or read the kevent
   list the pump stashed), rewrite `*sigmask` to the signals that fired, and
   return the count — matching the autodoc contract
   (`.../autodoc/auto_socket.c`). This is the single hardest function and the one
   the mingw32 port left as a `#warning TODO` (`.../waitselect.c`); it is the real
   deliverable of spike [N2].

3. **errno translation.** Host BSD errno (macOS values) → AmiTCP errno
   (`workbench/network/common/include/sys/errno.h` values) via a small fixed table,
   then written through the per-task `errnoPtr` at `errnoSize`. Most numbers differ
   (e.g. macOS `ECONNREFUSED=61` happens to match AROS's 61, but `EAGAIN` macOS=35
   matches too — many AmiTCP values were *taken from* BSD, so the table is mostly
   identity with a handful of exceptions; **verify each at port time**, don't assume
   identity). `gethostbyname`/resolver errors map to the per-task `h_errno`
   (`SBTC_HERRNO`).

## Plan — spikes in the loop

Each marker is one runnable binary printing one PASS/FAIL, harness-driven (no TCC,
no manual step). Early spikes are standalone host-socket probes (the Phase-2
"prove the mechanism in a file" style); later ones graft into the real library.

- **[N1] socket()/connect()/send()/recv() round-trip.** The harness forks a
  localhost TCP echo server on the Mac (a few lines of host C, bound to
  `127.0.0.1:0`, port handed to AROS). The AROS side (initially the standalone H11
  scaffold + the host-socket shim) does `socket(AF_INET,SOCK_STREAM,0)`,
  `connect()`, `send("PING")`, `recv()` → assert bytes equal `PING`. PASS = exact
  round-trip; FAIL = mismatch/timeout. Proves the host-call shim carries socket
  syscalls and the fd/buffer marshalling is correct.

- **[N2] WaitSelect on multiple fds + an exec Signal.** Open two sockets to the
  echo server; the harness sends on one then the other with a gap; AROS
  `WaitSelect`s both read fds *plus* a self-`Signal` bit. Assert: WaitSelect wakes
  on the right fd each time, returns the correct ready count, rebuilds the
  `fd_set`, and *also* wakes when the exec Signal is raised with no fd ready
  (returning 0, `*sigmask` rewritten). This is the kqueue-pump↔Signal bridge — the
  load-bearing spike. PASS = correct wake source every time, underlying thread
  never blocks (a concurrent SIGALRM-counter keeps ticking).

- **[N3] non-blocking connect + slow read (no thread freeze).** Point AROS at a
  server the harness deliberately makes *slow* to accept / dribble bytes. Assert
  `connect` returns `EINPROGRESS` and the task parks (not the whole process — the
  preempt counter keeps advancing), then completes when the pump signals. Proves
  problem (1) of The bridge under real latency.

- **[N4] errno fidelity.** `connect()` to a closed port → assert AROS `Errno()`
  returns AROS `ECONNREFUSED` (61), not the raw host value; a `recv` on a
  would-block socket surfaces `EWOULDBLOCK` (35). Proves the translation table.

- **[N5] graft into the real `bsdsocket.library`.** Build the new
  `arch/all-unix/bsdsocket/` as an actual AROS module (per-task `OpenLibrary` →
  `SocketBase`, the real LVO table), and rerun [N1]/[N2] *through the library jump
  vectors* (the H8/H12 LVO dispatch). PASS = same round-trip, now via
  `__AROS_GETVECADDR(SocketBase, LVO)`.

- **[N6] a tiny real fetch (stretch).** Once DNS is decided (see Risks): resolve
  + `connect` to a real host, send a minimal `HTTP/1.0 GET`, assert a `200`/known
  bytes in the reply. Only meaningful once outbound routing/DNS work; gated behind
  network availability so the core suite stays hermetic on localhost.

## How we verify it unattended

The whole suite is hermetic and TCC-free because **the server is on the same Mac,
on localhost**, started by the harness itself:

1. Harness `fork()`s (or spawns from `run.sh`) a localhost echo/test server bound
   to `127.0.0.1:0`, reads back the OS-assigned port, and passes it to the AROS
   binary (argv/env/file — same channel `run.sh` already uses).
2. AROS connects, sends a known pattern, receives; the AROS side asserts
   `recv == sent` and prints the usual `[N#] PASS/FAIL` marker.
3. The harness *also* asserts from the host side (the server logs the bytes it
   echoed), so the round-trip is checked at both ends — like H11, where the client
   checks `read==write` *and* main re-reads the file through the host
   (`hosted/device.c`). No screen, no permission dialog, no human.

Localhost loopback needs no entitlement on macOS (unlike raw BPF/`utun`, which is
exactly why we forward at the *socket* layer, not the NIC layer). Timeouts use the
existing bash watchdog (NOTES.md "portable timeout") so a hung connect is reaped
into a FAIL, preserving the unattended guarantee.

## What proves "the internet works" in AROS — the test tools

Two complementary ways to verify, both grounded in what AROS actually ships and in
the unattended loop:

**1. The automated spikes (the loop's source of truth).** `[N1]` (localhost TCP
echo round-trip) and `[N6]` (a real `HTTP/1.0 GET`) are the hermetic,
harness-asserted proofs that bytes move both ways. `[N1]` is loopback-only (no DNS,
no entitlement); `[N6]` is the flagship "AROS is on the internet" demo.

**2. Shipped AROS network commands — but only the socket-client ones.** AROS's `C:`
ships `ping`, `traceroute`, `nslookup`, `netstat`, `route`, `arp`, `ifconfig`,
`ip`/`ipf`/`ipnat`, `resolve`, `hostname` (built from
`workbench/network/common/C/`). Because we forward at the **socket** layer (the Mac
owns the stack), they fall into classes — only the first two work through a
forwarder:

| Class | Commands | Through host-passthrough |
|-------|----------|--------------------------|
| Socket clients (TCP/UDP) | a fetch / `HTTP GET`, SMB (`smbfs`), any app on `bsdsocket.library` | **work** — this is the whole point |
| Resolver-gated | `resolve`, `nslookup`, `hostname` | work **once `gethostbyname` (LVO 35) lands** (deferred; §Risks) |
| Raw sockets | `ping`, `traceroute` | **don't** — `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` needs host root/entitlement (verified in `ping.c:631`); we avoid raw to stay TCC-free |
| Stack-internal | `netstat`, `route`, `arp`, `ifconfig`, `ip`/`ipf`/`ipnat` | **don't** — they read/configure the *AROS* stack via `kvm_read`/AROSTCP symbols (verified in `netstat/main.c`); with the Mac owning the stack there is nothing to introspect |

So the natural user-facing smoke test is **`resolve <host>`** (once the resolver
lands) plus a small **fetch** command — there is *no* shipped TCP-fetch tool
(`telnet`/`ftp`/`wget`) in the base tree, so `[N6]` doubles as that command. `ping`
is explicitly **not** a goal: a working `ping` needs either a privilege grant or a
`SOCK_RAW`→`SOCK_DGRAM` ICMP shim, and the stack-config tools need the native
AROSTCP stack — both belong to the AROSTCP-on-SANA-II route this feature rejects.

## Cross-host reuse and the in-tree gap

**This does not exist in AROS today — for any unix host.** A tree-wide search finds
exactly one host-passthrough `bsdsocket.library`: `arch/all-mingw32/bsdsocket/`
(Windows/Winsock — and even it leaves `WaitSelect`/`recv`/`connect` as
`#warning TODO`). The only host-backed networking for *unix* hosts is the SANA-II
NIC route (Linux-only, runs AROS's own AROSTCP);
`workbench/network/stacks/AROSTCP/bsdsocket/` is that native stack, not a forwarder.
So a working unix/darwin host-socket `bsdsocket.library` is **net-new for AROS**.

**And the bulk of it benefits every hosted AROS, not just darwin-aarch64.** The
design isolates the only platform-specific piece — the readiness primitive
(kqueue) — behind the `pump_*` seam; everything above it is host-neutral: the
per-task `SocketBase`/`dTable`, the LVO bodies, the errno discipline, and above all
the **`WaitSelect`↔`Signal` bridge** that mingw32 left unfinished. Swap
`kqueue`→`epoll`/`poll` in one file and the same module gives **every unix-hosted
AROS flavour** (`linux-x86_64`/`i386`/`arm`/`aarch64`) real host-socket networking;
the `WaitSelect` algorithm even ports back to finish the Windows stub. Per-host
specifics are small and isolated: the readiness backend, the errno value table
(Linux errno ≠ BSD/AmiTCP), and the `sockaddr` `sa_len` detail (BSD has it, Linux
doesn't). This is why the module is placed at `arch/all-unix/bsdsocket/`, not
`arch/all-darwin/` (see spec.md "Build / integration") — though only the darwin
kqueue backend is built and verified in our loop; the Linux `epoll` backend is a
bounded follow-on, not claimed here.

## Risks & open questions

- **WaitSelect↔Signal integration (highest risk).** Correctly rebuilding the
  output `fd_set`s and the in/out `*sigmask`, with no lost wakeups when a kqueue
  event and an exec `Signal` race, is the subtle part. Mitigation: the H9 lesson —
  `Wait` checks `tc_SigRecvd` *before* parking, so a Signal that races ahead is
  still seen (NOTES.md H9); apply the same discipline to the pump's signal. The
  mingw32 port never solved this (its `WaitSelect` is a stub), so there's no
  *in-tree* reference impl to copy — [N2] is genuinely new for AROS, and the
  race-free wake is derived independently from H9 + the AmiTCP autodoc.
- **Blocking-call avoidance.** Every host socket op must be non-blocking; a single
  accidental blocking call freezes AROS's one underlying thread. Enforce: set
  `O_NONBLOCK` at creation, and treat any host call that *can* block (`connect`,
  `recv`, `accept`) as a wait-then-retry, never a direct block. Spike [N3] exists
  to catch a regression here.
- **DNS / resolver.** `gethostbyname` (LVO 35) and friends can block for a long
  time in the host resolver. The mingw32 port dedicates a whole `ResolverThread`
  to this (`host_socket.c`). Plan: route blocking resolver calls onto the host
  pump thread (or a second helper thread) and signal completion — same bridge as
  the socket path. Deferred past the core suite (localhost needs no DNS); [N6]
  forces the decision. **UNVERIFIED:** whether macOS's `getaddrinfo` via libSystem
  is safe to call from the pump thread while AROS holds `Forbid()` — check at port
  time.
- **errno fidelity.** AmiTCP numbering overlaps BSD but isn't identical; a wrong
  map silently breaks app error handling. Mitigation: explicit table + spike [N4],
  no blind passthrough.
- **IPv6.** AROS `AF_INET6=28` vs macOS `AF_INET6=30` (**UNVERIFIED** — confirm
  against macOS `<sys/socket.h>`); `sockaddr_in6` layout must be checked too.
  IPv4-first; IPv6 is a follow-on, not a bring-up blocker.
- **sockaddr byte-order / `sin_len`.** AROS and macOS both carry BSD `sin_len`
  and network-byte-order `sin_port`, so the copy is near-verbatim — but
  `sa_family` is one byte in AROS's `sockaddr` (`sa_len; sa_family`) and must line
  up with macOS's `sa_family_t`; verify width at port time.
- **Where it slots in the kickstart.** Today's boot is a minimal 3-module kickstart
  that halts at a cold-start trap before any `dos.library`
  (`graft/WORKFLOW.md`). `bsdsocket.library` is a normal (non-resident) library —
  it can be added to the disk-based module set once the boot reaches a point where
  libraries load, *after* the `dos.library` bring-up (`graft/WORKFLOW.md` item
  F2/23). This feature is designed and grounded now; it lands after the cold-start
  walk, not before.
- **Listening/server sockets in AROS.** The plan focuses on client flows
  (`connect`/`send`/`recv`), which the unattended echo test exercises fully.
  `accept`/`listen` readiness is the same kqueue `EVFILT_READ`-on-listen-fd
  pattern (autodoc: "select true for reading on a listen()ed descriptor ⇒ accept()
  won't block" — `.../autodoc/auto_socket.c`); covered by the design, tested only
  lightly until an AROS-side server use-case appears.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- `arch/all-mingw32/bsdsocket/` — the host-passthrough template (Windows): `bsdsocket.conf` (LVO functionlist + regs), `bsdsocket_init.c` (hostlib interface build), `bsdsocket_open.c` + `bsdsocket_intern.h` (per-task `TaskBase`/`SocketBase`), `socket.c` (non-blocking create, `Forbid`/errno), `host_socket.c` (ResolverThread/IRQ — the part darwin replaces), `waitselect.c`/`recv.c`/`connect.c` (the `#warning TODO` stubs).
- `workbench/network/stacks/AROSTCP/bsdsocket/` — the native AmiTCP stack (not used): `api/amiga_api.h` (`struct SocketBase`), `api/amiga_api.c` (per-task open), `autodoc/auto_socket.c` (WaitSelect/select semantics).
- `workbench/network/common/include/` — the public contract: `defines/bsdsocket.h` (LVO map), `clib/bsdsocket_protos.h` (signatures), `proto/bsdsocket.h`, `libraries/bsdsocket.h` + `bsdsocket/socketbasetags.h` (`SBTC_*` tags), `sys/socket.h`/`netinet/in.h` (`sockaddr`/`sockaddr_in`), `sys/net_types.h` (`fd_set`/`FD_SETSIZE=64`), `sys/errno.h` (AmiTCP errno values).
- `arch/all-unix/hidd/unixio/` — host libc plumbing already on darwin: `unixio.h` (`HOST_OS_darwin` → `libSystem.dylib`), `unixio_class.c` (`libc_symbols[]` incl. `socket/sendto/recvfrom/bind`), `unixpkt_class.c` (the Linux-only raw-packet path we are *not* using).
- `arch/all-unix/devs/networks/{eth,tap}/` — SANA-II NIC drivers (Linux-only; the "run AROS's own TCP/IP" route we reject).
- `arch/all-hosted/hostlib/` — `HostLib_Open`/`GetInterface`/`Lock` (`hostlib.resource`).

This project:
- `hosted/device.c` (H11) — the host-call-on-a-switched-task pattern: `WaitPort`/host syscall/`ReplyMsg`, the model bsdsocket reuses for block-then-signal.
- `hosted/signal.c` (H9), `hosted/msgport.c` (H10) — Wait/Signal + ports, the wait-then-signal substrate for the kqueue bridge.
- `hosted/abishim.S`, `hosted/host.c` (H3) — the AAPCS64↔Apple-arm64 host-call shim every socket syscall crosses.
- `hosted/exec.c` (H4/H6) — the single-underlying-thread + SIGALRM scheduler model the pump thread must not perturb; the `Forbid` compiler-barrier rule.
- `NOTES.md` — H1–H12 spike log and the "ground it, don't dream it" discipline.
- `graft/WORKFLOW.md` — current boot state (3-module kickstart, cold-start halt); where this library slots after `dos.library`.

Independent work: no third-party implementation source — emulator, agent, driver,
or otherwise — was read, searched, or consulted in producing this feature, and any
resemblance to existing implementations is coincidental. The design stands on the
AmiTCP autodoc semantics, the in-tree AROS modules cited above, POSIX/macOS socket
and `kqueue` docs, and this project's H-series spikes alone.
