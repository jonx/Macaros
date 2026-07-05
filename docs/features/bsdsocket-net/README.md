# Host BSD sockets — real networking for AROS via `bsdsocket.library`

> Status: **working on darwin-aarch64 (2026-06-28)** — sockets, `WaitSelect`, and
> DNS all proven live on booted AROS; **AROS reaches the real internet on Apple
> Silicon.** This is the practical "what & how"; for the design rationale and the
> implementation spec see [design.md](design.md) and [spec.md](spec.md).

## What it does

Gives the hosted AROS **working TCP/IP** by forwarding AROS's `bsdsocket.library`
(the AmigaOS networking API — AmiTCP/Roadshow) onto the Mac's native BSD sockets.
The Mac's kernel does the real work (TCP, routing, DNS, the NIC); AROS code reaches
it through the standard LVO calls it already expects — `socket()` / `connect()` /
`send()` / `recv()` / `WaitSelect()` / `gethostbyname()`. The result: an AROS
program (or a future browser, IRC client, …) can open a socket and talk to the
internet, with no AROS-side TCP stack and no emulated NIC.

Proven live, on booted AROS, through this library:

```
[N5b] PASS: round-trip echoed 'PING42' (6 bytes)                     # localhost TCP
[WS]  PASS: WaitSelect woke on read-ready, recv echoed the byte      # LVO 21
[N6]  PASS: AROS fetched over the internet from 1.1.1.1 -> 'HTTP/1.1 301 ...'
[DNS] PASS: resolved one.one.one.one -> 1.1.1.1, fetched 'HTTP/1.1 301 ...'
```

Screenshots of each verdict are regenerated locally by the smoke run
(`aros-ctl shot`; kept out of the repo).

## Quick start

```sh
cd ~/Source/aros-aarch64
make bsdsock-dylib                          # build the host pump shim
./graft/aros-ctl deploy                     # deploy it to ~/lib for HostLib_Open
graft/bsdsock-livetest.sh                   # host unit tests + the live nettest
```

`bsdsocket.library` and the `socktest`/`nettest` test commands are built into the
AROS tree (`make workbench-libs-bsdsocket-unix` and `…-test-quick` in the configured
build); the dylib is the one piece that lives in this repo and deploys to `~/lib`
beside `cocoametal.dylib`, `libpasteboard.dylib`, and `libcoreaudio.dylib`.

## How it works — forward at the socket layer, with a kqueue pump

There is **no AROS TCP/IP stack and no emulated NIC.** The new `bsdsocket.library`
marshals each socket LVO straight to `libSystem`'s BSD sockets, reached through
`hostlib.resource`. The only non-trivial part is *blocking*: AROS runs on a single
underlying thread, so a blocking `recv`/`connect` must not call a blocking host
syscall. Every host socket is `O_NONBLOCK`, and readiness is delivered by a **kqueue
pump thread**:

```
   AROS task (one of many, on AROS's single underlying thread)
      │  socket / connect / send / recv / WaitSelect / gethostbyname  (LVO)
      ▼
  bsdsocket.library  (arch/all-unix/bsdsocket)   ── per-task SocketBase
      │  · O_NONBLOCK host fds, AROS-fd→host-fd dTable
      │  · errno xlate (Darwin BSD → AmiTCP)
      │  hostlib.resource (HostLib_Open + GetInterface)
      ├─────────────► libSystem.dylib  ── the real socket()/connect()/recv()/...
      │
      └─────────────► libbsdsockhost.dylib (this repo)
                        · kqueue PUMP thread: blocks in kevent(), stashes readiness
                        · async DNS: getaddrinfo() on a detached thread
                        ▲
   on a would-block:  register the fd with the pump, then ───┘
   ▼  TIMER-POLL pump_drain on a Delay() tick  (NOT a host-thread Signal)
   re-issue the non-blocking op (the syscall is the truth) → done
```

**The load-bearing design call — timer-poll, not a host-thread `Signal`.** The spec
first assumed the pump thread would raise an `exec` `Signal` into the waiting task.
But on this hosted port that is **unsafe**: a task woken from host-thread/interrupt
context runs in "supervisor mode" under the threaded scheduler and trips every
semaphore op (`arch/all-darwin/hidd/cocoa/cocoa_input.c:546`), which is exactly why
both shipping darwin drivers (input, clipboard) **poll `timer.device`** instead. So
the kqueue pump stays — for efficient in-kernel readiness — but the AROS-side "park"
is a `Delay()` poll of the pump's readiness stash, never a `Wait` on a host Signal.
**kqueue for readiness, timer-poll for the safe handoff** (≈one timer tick of
latency, immaterial for sockets). Same pattern for `WaitSelect` and the DNS resolver.
Full detail: spec [§R-DARWIN-WAKE](spec.md) and [host-wake-pattern.md](../host-wake-pattern.md).

## What works today

| LVO surface | State |
|-------------|-------|
| `socket` `bind` `listen` `accept` `connect` `send` `sendto` `recv` `recvfrom` `shutdown` `CloseSocket` | **implemented** (non-blocking + the timer-poll park) |
| `setsockopt` `getsockopt` `getsockname` `getpeername` `IoctlSocket`(FIONBIO) | **implemented** |
| `WaitSelect` (LVO 21) | **implemented** — timer-poll over fds + the `*sigmask` signals |
| `Errno` `SetErrnoPtr` `getdtablesize` `SetSocketSignals` | **implemented** |
| `gethostbyname` (LVO 35) | **implemented** — async `getaddrinfo`, timer-polled |
| `gethostbyaddr` `get{net,serv,proto}by*` `inet_*` `Obtain/ReleaseSocket` `Dup2Socket` `SocketBaseTagList` `send/recvmsg` `GetSocketEvents` | **safe stubs** (secondary; fill as needed) |

### Which shipped AROS network commands work

Because we forward at the **socket** layer (the Mac owns the stack), the shipped
`C:` commands split — only socket-clients ride a forwarder:

| Works | Doesn't (needs the native AROS stack / raw-socket privilege) |
|-------|--------------------------------------------------------------|
| socket-client apps (HTTP, SMB, telnet/ftp clients), `nslookup`/`resolve` (DNS) | `ping`/`traceroute` — raw ICMP sockets need host root |
| anything calling `socket/connect/send/recv` | `netstat`/`route`/`arp`/`ifconfig`/`ip` — read/configure the *AROS* stack via `kvm` |

See [design.md → "What proves the internet works"](design.md) for the full matrix.

## Where the code lives

**Host side (this repo — `aros-aarch64`):**

- [hosted/bsdsocket/](../../../hosted/bsdsocket/) — `bsdsock_pump.c` (the kqueue
  readiness pump + the `ps_create_cb` wake seam), `bsdsock_shim.c` (non-blocking
  socket wrappers + `hs_set_nonblock`), `bsdsock_resolve.c` (async `getaddrinfo`),
  `errno_xlate.{c,h}` (Darwin→AmiTCP errno table). Built into
  **`build/libbsdsockhost.dylib`** via `make bsdsock-dylib` (exports in
  `bsdsock.exports`); deploys to `~/lib`.
- [hosted/bsdsocket/n5b/](../../../hosted/bsdsocket/n5b/) — the AROS test clients
  (`socktest.c`, `nettest.c`) and the host `echo_server.c`, kept for reproducibility.

**AROS side (`aros-upstream`, branch `aarch64-darwin-graft`):** the new module
`arch/all-unix/bsdsocket/` — a host-passthrough `bsdsocket.library`:

- `bsdsocket.conf` — the genmodule LVO functionlist (AmiTCP offsets, copied verbatim).
- `bsdsocket_init.c` — opens `libSystem` + `libbsdsockhost` via hostlib (`Disable()`
  around the dylib opens so pump threads inherit a blocked SIGALRM mask), starts the pump.
- `bsdsocket_open.c` — the per-task `SocketBase` (`dTable`, errno, sigmasks, the pump
  wake target).
- `bsdsocket_core.c` — the data path; `bsdsocket_sockopt.c`, `bsdsocket_misc.c`;
  `waitselect.c` (the real LVO 21); `bsdsocket_resolve.c` (`gethostbyname`);
  `bsdsocket_util.c` (the `PollFd` timer-poll park, errno store, dTable helpers);
  `bsdsocket_stubs.c` (the secondary LVOs).
- It's in `all-unix/` so the host-neutral core benefits every hosted AROS — only the
  kqueue file is darwin-specific (Linux would swap in `epoll`).

It loads into AROS exactly like the display/clipboard shims: by bare name through
`hostlib.resource`, which is why `libbsdsockhost.dylib` must sit in `~/lib` with
`DYLD_FALLBACK_LIBRARY_PATH` pointing there (`run-window.sh`/`aros-ctl` set this).

## Testing — layer by layer

### 1. Host-side unit tests (instant, no boot)

Run under host `clang` straight from the repo; each prints a `PASS`/`FAIL` verdict:

```sh
make hosted-bsdsocket    # [N1]–[N3] the kqueue pump: connect/send/recv, readiness, park
make bsdsock-abi         # [NABI] dlopen the dylib, resolve every exported symbol + the wake seam
make bsdsock-errno       # [NERR] the Darwin→AmiTCP errno table (25/25; catches EOPNOTSUPP 102→45)
```

### 2. Live tests on booted AROS

One command runs the host tests and the live `nettest` (WaitSelect + raw-IP fetch +
DNS), capturing a screenshot of the verdicts:

```sh
graft/bsdsock-livetest.sh        # builds+deploys the dylib, boots AROS, runs nettest
```

Or by hand:

```sh
clang -O2 hosted/bsdsocket/n5b/echo_server.c -o /tmp/echo_server
/tmp/echo_server &                       # localhost echo on 127.0.0.1:12345 (for [N5b]/[WS])
graft/aros-ctl run                       # boot AROS (discovers the build dir; set AROS_CTL_BOOTD to override)
graft/aros-ctl wait 16                   # reach the "1>" CLI prompt
graft/aros-ctl type "nettest"            # or "socktest" for just the localhost round-trip
graft/aros-ctl enter
graft/aros-ctl wait 16                   # DNS + two HTTP fetches take a few seconds
graft/aros-ctl shot /tmp/nettest.png     # the [WS]/[N6]/[DNS] verdicts render here
graft/aros-ctl stop;  kill %1
open /tmp/nettest.png
```

Reading it:

- **The screenshot** shows the AROS-side `[WS]`/`[N6]`/`[DNS]` PASS/FAIL lines (the
  console output isn't captured to a log — it renders in the window).
- **The echo server's stderr** is the host-side oracle for the round-trip: `client
  connected` → `echoed N bytes` → `client closed` means the `connect`/`send`/`recv`
  path worked end to end.
- `graft/aros-ctl crash` extracts any trap/Guru lines; `graft/aros-ctl log 40` tails
  the boot log; `graft/aros-ctl libs` lists the host dylibs AROS has mapped.

### Gotchas

- **Use the windowed boot** (`aros-ctl run`). The headless emergency-CLI boot
  currently crashes early in `dos`/`LoadSeg` — a pre-existing boot fragility,
  unrelated to bsdsocket (the library is never reached).
- The boot occasionally comes up to the **desktop** rather than a CLI prompt; if a
  typed command "does nothing", re-run — you want the `1>` prompt.
- `[N6]`/`[DNS]` need **host outbound internet** (they hit `1.1.1.1:80`); `[N5b]`/`[WS]`
  are hermetic on localhost.

## Status

| Path | State |
|------|-------|
| Library loads + initialises (hostlib + libSystem + the kqueue pump) | **proven live** ([N5a]) |
| TCP round-trip (`socket`/`connect`/`send`/`recv`) | **proven live** ([N5b], two-sided) |
| `WaitSelect` (LVO 21) | **proven live** ([WS]) |
| Outbound internet — raw IP `HTTP/1.0 GET` | **proven live** ([N6]) |
| DNS — `gethostbyname` + resolved fetch | **proven live** ([DNS]) |
| Secondary LVOs (resolvers beyond `gethostbyname`, `inet_*`, fd hand-off, …) | safe stubs |
| `WaitSelect` `exceptfds` (OOB) | not separately monitored (read/write only) |

## Provenance

Independent work: no third-party implementation source — emulator, agent, driver,
or otherwise — was read, searched, or consulted; built from the AmiTCP autodoc
semantics, the in-tree AROS modules, POSIX/macOS `socket`/`kqueue`/`getaddrinfo`
docs, and this project's H-series spikes. See [spec.md](spec.md)'s provenance banner.
