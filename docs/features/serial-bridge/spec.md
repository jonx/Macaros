# Implementation spec — host-backed `serial.device` / `parallel.device` (POSIX termios)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Companion to [design.md](design.md). Process: [../CLEANROOM.md](../CLEANROOM.md).

## Provenance banner

**Independent work: no third-party implementation source — emulator, agent, driver,
or otherwise — was read, searched, or consulted in producing it, and any resemblance
to existing implementations is coincidental.** Implement only from this spec + the
approved sources cited by tag: `[PUB]` POSIX `termios`/tty `man` pages, Apple
`IOKit`-serial / `/dev/cu.*` docs, published standards; `[AROS]` in-tree AROS headers
and modules (paths given; APL/LGPL — ours); `[OURS]` this project's spikes (the
H-series, `hosted/*`, `graft/*`). `[DERIVED]` items are independently-derived
requirements flagged for extra verification; each stands solely on its cited
`[PUB]`/`[AROS]`/`[OURS]` justification — implement from that justification, never from
any reference. No identifier name, call sequence, file layout, or buffer-management
algorithm in this spec derives from any third-party implementation.

## Scope

**In.** Finish the **hosted/unix serial HIDD** for `aarch64-darwin` so AROS's existing
`serial.device` drives a real macOS character device (a `/dev/cu.*` tty or a host PTY
slave): the full `serial.device` command set (`CMD_READ`/`CMD_WRITE`/`CMD_CLEAR`/
`CMD_RESET`/`CMD_FLUSH`/`SDCMD_QUERY`/`SDCMD_SETPARAMS`/`SDCMD_BREAK`), with
`SDCMD_SETPARAMS` (baud/bits/stop/parity/handshake) mapped to host `termios`, and the
receive path delivered via `unixio.hidd`'s existing **darwin SIGALRM-timer fd-poll**
(no foreign thread, no host-context `Signal`). A thinner **`parallel.device`** secondary
on the same shim. Verified hermetically and headless by a **host PTY-pair loopback** +
byte-exact / termios-fidelity asserts.

**Decision (confirmed with the project owner).** **Finish the existing seam, don't
reimplement.** AROS already defines the serial-HIDD backend seam, the `serial.device`
shell is host-agnostic and reused verbatim, and the hosted/unix backend
(`arch/all-unix/hidd/serial/SerialUnitClass.c`) already opens + writes a host device
over `unixio.hidd`. We complete that file (un-`#if 0` the termios + async-receive
paths, retarget to macOS device names) rather than writing a new HIDD. Rationale in
[design.md](design.md) ("Does it already exist?").

**Decision.** **No host thread; ride `unixio.hidd`'s timer-poll wake.** Readiness is
delivered by `unixio.hidd`'s SIGALRM-IRQ fd-poll demux (already in-tree for darwin),
and the wake `Signal` fires from AROS IRQ context — the safe path per
[host-wake-pattern.md](../host-wake-pattern.md). The serial bridge therefore has **no
background pthread** (unlike CoreAudio / bsdsocket). `[OURS]`/`[AROS]`.

**Decision.** **`/dev/cu.*` (call-out) + `O_NONBLOCK`, not `/dev/tty.*`.** The call-out
device does not block on DCD; the dial-in `tty.*` would hang the non-blocking-open
contract `[PUB]`.

**Out (non-goals, this spec).** Inbound modem-status / DCD/RI line monitoring beyond a
best-effort `GetStatus` (returns 0 first cut, as the stub does today); non-standard
baud rates needing `IOSSIOSPEED` (standard `B*` table first; MIDI/odd rates a flagged
follow-on); `SERF_SHARED` multi-opener correctness over one host fd (faithful to the
shell, untested on a host tty — note, defer); the i386-pc / m68k / sam440 hardware
serial HIDDs (untouched); changes to the portable `serial.device`/`parallel.device`
shell; auto-discovery of USB-serial adapter names (runtime path map instead).

## Architecture

Three layers, all `[AROS]` except the new termios glue; joined by `unixio.hidd`'s
existing `hostlib.resource` libc-symbol mechanism (no hand-written ABI of our own — the
host side *is* libc, the bsdsocket lesson). The darwin host-wake is inherited, not
built.

```
AROS side (aarch64, AROS crosstools)                 Host side (Apple libSystem.dylib)
┌──────────────────────────────────────┐             ┌────────────────────────────┐
│ serial.device  (DOS/exec IORequest)   │  [AROS]     │ libSystem.dylib (dlsym'd by │
│   shell, reused verbatim              │             │  unixio.hidd via hostlib)   │
│     │ HIDD_SerialUnit_Write/SetParams │             │  open/read/write/poll/ioctl │
│     ▼                                 │             │  + NEW: tcgetattr/tcsetattr │
│ all-unix serial HIDD  [AROS, finish]  │  HostLib_   │    cfsetispeed/cfsetospeed  │
│   · SerialUnitClass: termios + write  │  Lock/      │    cfmakeraw/tcsendbreak    │
│   · serialunit_io receive handler ────┼──barrier──► │    openpty (verify)         │
│     │ Hidd_UnixIO_OpenFile/Read/Write │ ◄────────── │                             │
│     ▼            ▲ AddInterrupt(fd,RW) │  fd ready   │  a /dev/cu.* tty            │
│ unixio.hidd  [AROS, darwin-aware]      │             │  OR a host PTY slave        │
│   · SIGALRM timer poll → receive wake  │             │  (master = verify oracle)  │
└──────────────────────────────────────┘             └────────────────────────────┘
```

- **`serial.device` / `parallel.device` shell** `[AROS]` — `workbench/devs/serial/`,
  `…/parallel/`. Reused unchanged: the IORequest dispatch, the per-unit input ring,
  the read/write queues, `SU->su_Lock`, the `STATUS_*` state machine. The only file
  naming a host symbol is the HIDD backend.
- **the all-unix serial/parallel HIDD** `[AROS]` — `arch/all-unix/hidd/serial/`,
  `…/parallel/`. The `Hidd_SerialUnit` methods over `unixio.hidd`. Exists today;
  finish the termios + async-receive code, darwin-guarded.
- **`unixio.hidd`** `[AROS]` — `arch/all-unix/hidd/unixio/`. Owns the libc handle, the
  fd I/O methods, and — already, for darwin — the SIGALRM-timer fd-readiness demux. We
  **add libc symbols** to its `libc_symbols[]`; we do not change its wake logic.
- Spike-phase: bare host probes under `hosted/serial/`; at graft the AROS fill-in lands
  in the existing `arch/all-unix/hidd/serial/` files.

## The portable AROS contracts this binds to (grounded, `[AROS]`)

Restated from headers + live source so the implementer needs no third-party source.

- **The device command surface.** `struct IOExtSer` (`compiler/include/devices/
  serial.h:22–36`): `io_CtlChar`, `io_RBufLen`, `io_ExtFlags`, `io_Baud`, `io_BrkTime`,
  `io_TermArray`, `io_ReadLen`, `io_WriteLen`, `io_StopBits`, `io_SerFlags`,
  `io_Status`. Commands (`serial_init.c:50–61`; values `exec/io.h:44–53`): `CMD_READ=2`,
  `CMD_WRITE=3`, `CMD_CLEAR=5`, `CMD_RESET=1`, `CMD_FLUSH=8`, `CMD_START=7`/`CMD_STOP=6`,
  `SDCMD_QUERY=CMD_NONSTD=9`, `SDCMD_BREAK=10`, `SDCMD_SETPARAMS=11`, `NSCMD_DEVICEQUERY`.
  Flags `SERF_PARTY_ON/ODD/7WIRE/QUEUEDBRK/SHARED/EOFMODE/XDISABLED`
  (`serial.h:46–61`); status `IO_STATF_OVERRUN/READBREAK/XOFFREAD/XOFFWRITE`
  (`:67–76`); errors `SerErr_BaudMismatch/BufErr/InvParam/LineErr/DevBusy`
  (`:91–100`). **The shell handles all of this already** — we implement only what the
  HIDD methods below receive.
- **The HIDD seam (what we implement).** `workbench/hidds/serial/include/serial.h:
  185–196`: `HIDD_Serial_NewUnit/DisposeUnit`; `HIDD_SerialUnit_Init(unit,
  DataReceived, DRud, WriteData, WDud)`, `_Write(unit,data,len)→bytes`,
  `_SetBaudrate(unit,baud)→BOOL`, `_SetParameters(unit,tags)→BOOL`,
  `_SendBreak(unit,duration)→BYTE`, `_Start`/`_Stop`, `_GetCapabilities`, `_GetStatus`.
  `Init`'s callbacks are the device's `RBF_InterruptHandler` (received bytes →
  `serial_interrupthandlers.c:22`, copies into the input ring) and `WBE_InterruptHandler`
  (write-buffer-empty → next queued write). The backend calls `DataReceivedCallBack`
  from its receive path, `DataWriteCallBack` from its write-ready path.
- **How `SDCMD_SETPARAMS` arrives at the HIDD** (`serial_init.c:712–818`): the shell
  copies flags to the unit, and on a *baud change* calls
  `HIDD_SerialUnit_SetBaudrate(io_Baud)` (`:771`); on a *read/write-len or stop-bits
  change* builds `{TAG_DATALENGTH=io_ReadLen, TAG_STOP_BITS=io_StopBits, TAG_SKIP=0
  (parity TODO), TAG_END}` and calls `HIDD_SerialUnit_SetParameters(tags)` (`:792–798`).
  `SetParameters` tags + values: `TAG_DATALENGTH`(5/6/7/8), `TAG_STOP_BITS`(1/2),
  `TAG_PARITY`(`PARITY_EVEN=0x03`/`PARITY_ODD=0x04`)/`TAG_PARITY_OFF`, `TAG_SET_MCR`
  (`serial.h:163–174`). **Note (carry forward):** the shell currently passes parity as
  `TAG_SKIP` (`serial_init.c:795`, "!!! PARITY!!!"), so to honour parity the backend
  must read `io_SerFlags` `SERF_PARTY_ON/ODD` directly (the flags *are* copied to the
  unit at `:764`); a shell change to pass `TAG_PARITY` is a possible follow-on.
- **The host I/O methods.** `unixio.hidd` (`unixio.conf:54–63`):
  `OpenFile(name,flags,mode,errp)`, `CloseFile`, `ReadFile`, `WriteFile`,
  `Wait(fd,mode)`, `Poll(fd,mode,errp)`, `AddInterrupt(struct uioInterrupt*)`,
  `RemInterrupt`. `struct uioInterrupt { MinNode; int fd; int mode; void(*handler)
  (int,int,void*); void *handlerData; }` (`include/unixio.h:19–26`); modes
  `vHidd_UnixIO_Read=0x1`/`Write=0x2`/`RW`/`Error=0x10` (`:42–45`). The libc handle is
  `libSystem.dylib` (`unixio.h:35–38`). Every host call is bracketed
  `HostLib_Lock(); … AROS_HOST_BARRIER; HostLib_Unlock();` (`unixio_class.c:839–853`).

## The host-wake model — inherited, the load-bearing constraint (`[AROS]`+`[OURS]`)

This is the constraint that dominates every host-thread driver in this folder
(CoreAudio, sockets, clipboard) — *how does a blocked AROS task learn the host fd is
ready, given that a foreign host thread must not `Signal` AROS on darwin?* For the
serial bridge it is **already solved in `unixio.hidd` and reused, not re-derived.**

- **R-WAKE — readiness comes from the SIGALRM timer poll, not SIGIO.** On darwin,
  `F_SETOWN`+`O_ASYNC`→`SIGIO` is never delivered for pipes and fails (`EPERM`) on a
  tty (`unixio_class.c:855–867` — in-tree verbatim). So `unixio.hidd` installs its
  `SigIO_IntServer` demux **also on `SIGALRM`** (`:1177–1192`); every fd registered via
  `AddInterrupt` is non-blocking-`poll(fd,0)`'d each scheduler tick (`poll_fd` :110–137,
  `SigIO_IntServer` :139–156) and the registered handler is fired on readiness. The wake
  `Signal` (`WaitIntHandler` :158–162) runs from this **AROS IRQ context**, which is the
  path the in-tree comment and the two proven darwin drivers (input, clipboard) mark
  safe. **Requirement:** the serial backend obtains its receive wake **only** by
  `Hidd_UnixIO_AddInterrupt(fd, vHidd_UnixIO_RW, handler)` — it must **not** spawn a
  host thread and must **not** `Signal` an AROS task from any host-thread/interrupt
  context of its own. Justification: `[AROS]` `unixio_class.c` darwin path; `[OURS]`
  host-wake-pattern DARWIN-AARCH64 CAVEAT + bsdsocket §R-DARWIN-WAKE; `[DERIVED]` that
  the serial bridge can inherit this wake unchanged (no per-feature pump) — independently
  derived from the layering, stands on the two citations.
- **R-NONBLOCK — the host fd is non-blocking.** Open with `O_NONBLOCK|O_RDWR`
  (`SerialUnitClass.c:125`, kept). A `ReadFile`/`WriteFile` that would block returns
  short/`EAGAIN`; the shell already queues the remainder and the timer-poll re-fires the
  handler next tick. Justification `[OURS]` (H6 single underlying thread — no blocking
  host syscall on it) + `[PUB]` (`O_NONBLOCK`).
- **R-LATENCY — tick-granularity receive latency is accepted.** Received bytes surface
  within ≤ one SIGALRM period (the timer-poll interval), immaterial for a serial line.
  No `volatile` low-water optimisation is required. `[DERIVED]` from R-WAKE; the same
  stance bsdsocket §R-DARWIN-WAKE takes for sockets.

Because no host thread exists, there is **no cross-thread shared state** in this
feature — the H6 `Forbid`/compiler-barrier model holds throughout, and the
`host-wake-pattern.md` R-W1 atomics requirement (for foreign threads) does not arise.

## Sample/data model — a pure byte pipe (`[AROS]`+`[PUB]`)

Serial is a raw byte stream; there is no format conversion (the easy contrast with
CoreAudio's int16→float32). The backend's job is byte-faithful transport + line config.

- **R-RAW — `cfmakeraw` baseline.** On `OpenFile`, after `tcgetattr`, apply `cfmakeraw`
  (`[PUB]`: clears `ICANON`/`ECHO`/`OPOST`, sets 8-bit-clean, `VMIN=1`/`VTIME=0`) so the
  tty passes bytes verbatim — AROS, not the host line discipline, owns framing. Then
  apply the negotiated speed/bits/parity (R-PARAMS) and `tcsetattr(fd, TCSANOW)`. This
  replaces the `#if 0` `cfmakeraw` at `SerialUnitClass.c:139`.
- **R-WRITE — write through, queue the remainder.** `_Write` → `Hidd_UnixIO_WriteFile`
  (`SerialUnitClass.c:323`, kept). Return the byte count actually written; the shell
  handles the short-write tail via `WBE_InterruptHandler` + the write-ready half of the
  interrupt.
- **R-READ — receive into the device ring on poll-readiness.** The `vHidd_UnixIO_Read`
  half of the registered handler does `Hidd_UnixIO_ReadFile(fd, buf, READBUFFER_SIZE)`
  then `DataReceivedCallBack(buf,len,unit,ud)` → `RBF_InterruptHandler` copies into the
  input ring (`serial_interrupthandlers.c:22`). This is serial's `serialunit_receive_data`
  (`SerialUnitClass.c:595`, currently reached only by the `#if 0` async wiring) re-driven
  by R-WAKE's `AddInterrupt`.

## AROS sub-driver binding — finish the existing HIDD (`[AROS]`, contract from design.md)

Work lands in `arch/all-unix/hidd/serial/SerialUnitClass.c` (parallel peer in
`…/parallel/ParallelUnitClass.c`), guarded `HOST_OS_darwin` where macOS diverges so the
Linux backend is byte-for-byte unchanged.

- **R-DEVNAME — macOS device names.** Replace the `unitname[]={"/dev/ttyS0".."ttyS3"}`
  (`SerialUnitClass.c:57–63`) with a darwin set. Default unit 0 = the verification PTY
  slave path; production names (`/dev/cu.usbserial-*`, `/dev/cu.debug-console`)
  supplied at runtime. **Decision:** a `SERIAL_UNITn` host env var per unit (read via
  the libc `getenv`, added to the symbol set), mirroring `AROS_HOST_VOLUME`; absent ⇒
  the PTY/default. Justification `[OURS]` (the host-volume launcher precedent) +
  `[PUB]` (`/dev/cu.*` naming). **UNVERIFIED:** exact env/UX — confirm at graft.
- **R-OPEN — `New`.** Create the `unixio.hidd` object (done, `:116`); resolve the unit
  path (R-DEVNAME); `Hidd_UnixIO_OpenFile(path, O_NONBLOCK|O_RDWR, 0, NULL)` (done,
  `:125`); on success, `tcgetattr`+R-RAW+default 9600-8N1+`tcsetattr` (un-`#if 0` the
  `:133–159` block, retargeted through `unixio.hidd`'s libc); then R-WAKE's
  `AddInterrupt(fd, vHidd_UnixIO_RW, serialunit_io)`. On any failure, `CloseFile` +
  dispose (the existing error ladder, with the `#if 0` removed).
- **R-PARAMS — `SetBaudrate` + `SetParameters` → termios.** Un-`#if 0` and retarget:
  - `SetBaudrate`: validate against `valid_baudrates[]` (`:333`, kept), then `tcgetattr`
    → `cfsetispeed`+`cfsetospeed`(speed) → `tcsetattr(TCSADRAIN)`; record
    `data->baudrate`. Return FALSE on an unsupported rate (→ shell raises
    `SerErr_BaudMismatch`). Non-`B*` rates: `ioctl(fd, IOSSIOSPEED, &speed)` `[PUB]`
    (follow-on, see Out).
  - `SetParameters`: from the tags + `io_SerFlags`, build `c_cflag`:
    `CSIZE`←`CS5/6/7/8` (from `TAG_DATALENGTH`), `CSTOPB` if 2 stop bits,
    `PARENB`(+`PARODD` for odd) from `SERF_PARTY_ON`/`SERF_PARTY_ODD`, `CRTSCTS` from
    `SERF_7WIRE`, clear `IXON|IXOFF` if `SERF_XDISABLED` else set; `tcsetattr(TCSADRAIN)`.
    This is `settermios` (un-`#if 0` `:678–723`). Map table fixed by `[PUB]` termios
    (the design.md table), not by any reference.
- **R-BREAK — `SendBreak`.** `tcsendbreak(fd, duration)` (un-`#if 0` `:513`); return 0
  on success, `SerErr_LineErr` on failure. `[PUB]`.
- **R-STATUS — `GetStatus`.** First cut returns 0 (as today). Optional: `ioctl(fd,
  TIOCMGET, &lines)` → map DSR/CTS/CD into `io_Status` `[PUB]` — flagged, not required.
- **R-RECV-HANDLER — `serialunit_io` (the receive/write-ready seam).** Adopt parallel's
  proven `parallelunit_io(fd,mode,ptr)` shape (`ParallelUnitClass.c:237–264`): on
  `mode & vHidd_UnixIO_Read` → R-READ; on `mode & vHidd_UnixIO_Write` →
  `DataWriteCallBack`. This is cleaner than serial's `#if 0` soft-int/reply-port
  scaffold (`:160–234`) and is the already-working precedent. Justification `[AROS]`
  (parallel peer) — but the *behaviour* (read-on-readiness, write-on-ready) is the
  termios/poll contract, not copied expression.
- **`HIDDSerialUnitData`** (`arch/all-unix/hidd/serial/serial_intern.h:51–82`) already
  carries `filedescriptor`, `baudrate`, `datalength`, `parity`/`paritytype`, `stopbits`,
  `stopped`, `unixio`, `orig_termios`, and the callback pointers — reuse; add a
  `struct uioInterrupt unixio_int` (as parallel has, `parallel_intern.h`-style) for
  `AddInterrupt`. Restore `orig_termios` on `Dispose` (un-`#if 0` `:263–281`).

## Parallel (secondary) — the thinner section (`[AROS]`)

`parallel.device` (`workbench/devs/parallel/`) + `arch/all-unix/hidd/parallel/
ParallelUnitClass.c`. Same shim, fewer parameters:

- **R-PAR-WIRED — parallel's async path already exists.** `ParallelUnitClass.c` already
  does `Hidd_UnixIO_AddInterrupt(&unixio_int)` with `parallelunit_io` (`:117–135,
  237–264`) and `Write` via `WriteFile` (`:185`). So parallel needs **only**
  R-DEVNAME (`/dev/lp*`→a darwin path / PTY; `:46–51`) and build bring-up — no termios
  fill-in (a parallel port has no baud/parity/stop-bits; `IOExtPar`
  (`compiler/include/devices/parallel.h:19–25`) has only `io_PExtFlags`/`io_Status`/
  `io_ParFlags`/`io_PTermArray`). `PDCMD_QUERY`=9 / `PDCMD_SETPARAMS`=10
  (`parallel.h:60–61`).
- **R-PAR-SCOPE.** Parallel is verified by the same PTY-loopback byte-exact test
  ([SR7]); modem/strobe handshake lines and printer status (`io_Status` paper-out/busy
  bits, `parallel.h:48–55`) are first-cut-optional (return clear). `CopyToPAR` /
  printer-to-parallel reach this device unchanged.

## Verification (unattended — `[OURS]` H11 two-sided discipline)

No human, no TCC, no real cable. The oracle is **a PTY pair the host process owns** +
**byte-exact / termios-fidelity asserts** on both ends. A PTY needs no entitlement (it
is ordinary file I/O by the AROS process under the launching terminal's permissions —
the host-volume stance). The shim `openpty()`s; AROS opens the device on the **slave**
path; the **master** fd is the independent oracle.

**The assertions** (every marker asserts *values*, never "it didn't crash"):
- **byte-exact** each data direction (serial is a pure byte pipe — an exact compare is
  the whole test; no DSP, no tolerance).
- **termios fidelity**: after `SDCMD_SETPARAMS`, `tcgetattr` the master and assert
  `cfgetospeed`/`cfgetispeed`, `c_cflag & CSIZE`, `CSTOPB`, `PARENB`/`PARODD` match.
- **`SDCMD_QUERY` count**: `io_Actual` == the bytes the host queued on the master.
- **readiness-via-poll**: assert the receive handler fired from the timer-poll demux
  (R-WAKE), e.g. with SIGIO disabled / unavailable — the darwin host-wake correctness.
- a **bash watchdog** (TERM-then-KILL; no GNU `timeout` on stock macOS) reaps a hung
  read into FAIL.

**Markers** (one host binary per marker, `[SR?]` PASS/FAIL, clean-exit on PASS):

- **[SR1] host PTY + termios round-trip (pure host probe, no AROS).** `openpty` →
  master raw, slave 9600-8N1 → write a known pattern slave→master and master→slave →
  assert byte-exact both ways; `tcgetattr` master → assert `B9600`/`CS8`/`!CSTOPB`.
  Grounds the harness + termios map before any AROS (the H7 `pngprobe` stance). `[SR1]`.
- **[SR2] the libc symbol set resolves.** `dlopen libSystem.dylib` (the
  `hostlib.resource` path) and resolve `tcgetattr/tcsetattr/cfsetispeed/cfsetospeed/
  cfmakeraw/tcsendbreak/tcflush/openpty/getenv` plus the existing `open/read/write/poll/
  ioctl/fcntl`. PASS = all resolve. The bsdsocket `[NABI]` discipline; catches the
  symbol-availability risk early. `[SR2]`.
- **[SR3] backend Write + poll-driven receive (bare spike).** Exercise the
  `SerialUnitClass` shape against a PTY without the full device: `OpenFile` the slave,
  `WriteFile` → master reads it; master writes → `poll(fd,0)` (the darwin demux) reports
  readable → `ReadFile` returns the bytes. PASS = both directions byte-exact, readiness
  found by **poll not SIGIO**. Proves the receive seam on the timer-poll model. `[SR3]`.
- **[SR4] SDCMD_SETPARAMS honoured.** Set 19200-7E2 → master `tcgetattr` reports
  `B19200`/`CS7`/`PARENB`(no `PARODD`)/`CSTOPB`; set 115200-8N1 → re-assert. PASS =
  every field landed. `[SR4]`.
- **[SR5] graft: real `serial.device` CMD_READ/CMD_WRITE on booted AROS.** Build the
  finished `workbench-hidd-unix-serial` + `serial.device` + `unixio.hidd` for
  darwin-aarch64; boot windowed; `OpenDevice("serial.device",0)` on the PTY slave;
  `CMD_WRITE "HELLO"` → master asserts the bytes; master writes "WORLD" → `CMD_READ`
  returns it. PASS = two-sided byte-exact through the real LVOs. Full thesis end-to-end;
  rides the crosstools graft (needs `dos.library` + the boot set). `[SR5]`.
- **[SR6] SDCMD_SETPARAMS / SDCMD_QUERY / SDCMD_BREAK on the real device.** Set baud via
  `SDCMD_SETPARAMS` (master termios asserted), `SDCMD_QUERY` after the master queues N
  bytes (assert `io_Actual==N`), `SDCMD_BREAK` (master sees a break). `[SR6]`.
- **[SR7] parallel.device byte-out (secondary).** `OpenDevice("parallel.device",0)` on
  a PTY slave, `CMD_WRITE` → master asserts the bytes; the `parallelunit_io` receive
  half delivers a master-written byte to `CMD_READ`. PASS = both directions byte-exact.
  `[SR7]`.

## Build / integration

- **No new dylib, no Cocoa.** The new code is C inside the existing `all-unix` HIDD
  files, built by the **AROS crosstools** as part of `serial.hidd`/`parallel.hidd` —
  **not** host clang. The spikes ([SR1]–[SR4]) are bare host binaries (`make
  hosted-serial`) in the `hosted/*` style.
- **The only host-side change is adding libc symbols to `unixio.hidd`.** Extend
  `arch/all-unix/hidd/unixio/`'s `struct LibCInterface` (`unixio.h:50–67`) and
  `libc_symbols[]` (`unixio_class.c:1107`) with
  `tcgetattr/tcsetattr/cfsetispeed/cfsetospeed/cfmakeraw/tcsendbreak/tcflush/openpty/
  getenv` — all `[PUB]` in `libSystem.dylib`, resolved by the existing
  `HostLib_GetInterface`. (Alternatively the serial HIDD could resolve them itself via
  `hostlib.resource`; adding to `unixio.hidd` is cleaner since it already owns the
  handle and the `HostLib_Lock` discipline. **Decision:** extend `unixio.hidd`.)
- **Build wiring (UNVERIFIED until a build).** Confirm the darwin-aarch64 mmake builds
  `workbench-hidd-unix-serial`/`-parallel` (`arch/all-unix/hidd/{serial,parallel}/
  mmakefile.src`), that `serial.device`/`parallel.device` link, and that
  `DRIVERS:serial.hidd` resolves. The shared prerequisite is `unixio.hidd` building +
  running on this target (the same gap host-volume flags) — assert at graft.
- **Where it slots in the boot.** `serial.device`/`parallel.device` are resident-pri-30
  devices (`serial.conf`/`parallel.conf`) loaded after the boot module set;
  spike-phase ([SR1]–[SR4]) needs no kickstart, exactly as `hosted/device.c` (H11).
- The AROS side pulls **no** macOS-specific tty headers it doesn't already (`termios.h`
  is included via `serial_intern.h`); host calls are opaque libc pointers through
  `unixio.hidd` + the H3 boundary.

## Open questions / UNVERIFIED

- The exact darwin device-selection UX (`SERIAL_UNITn` env var assumed) — confirm at
  graft; the PTY path keeps the loop hermetic regardless (R-DEVNAME).
- `unixio.hidd` build/run status on darwin-aarch64 (the shared prerequisite) — the
  same gap host-volume flags for the emul-handler overlay.
- Whether macOS PTYs propagate slave termios to the master `tcgetattr` for **all** the
  fields we assert (vs master-only attrs) — confirm in [SR1]; fall back to asserting the
  slave's own `tcgetattr` if not.
- Non-standard baud (`IOSSIOSPEED`) and inbound BREAK→`IO_STATF_READBREAK` /
  `GetStatus` modem lines (`TIOCMGET`) — flagged follow-ons, standard `B*` table first.
- The shell passes parity as `TAG_SKIP` (`serial_init.c:795`); the backend reads
  `io_SerFlags` directly to honour parity — confirm that path covers all callers, or
  add a `TAG_PARITY` shell change (a possible shell follow-on, out of this spec).
- `SERF_SHARED` multi-opener over one host fd — faithful to the shell, untested on a
  host tty; note, defer.
- The crosstools graft for [SR5]–[SR7] builds the device modules on top of `dos.library`
  + the boot set, which are already up (hosted AROS boots to a Wanderer desktop,
  `graft/WORKFLOW.md`).

## Provenance summary

`[PUB]` POSIX `termios` (`tcgetattr`/`tcsetattr`/`cfsetispeed`/`cfsetospeed`/
`cfmakeraw`/`tcsendbreak`/`tcflush`, `c_cflag` `CSIZE`/`CS5..8`/`CSTOPB`/`PARENB`/
`PARODD`/`CRTSCTS`, `IXON`/`IXOFF`, `VMIN`/`VTIME`), `openpty`/`posix_openpt`,
`O_NONBLOCK`, the `B*` baud constants; Apple/BSD tty model (`/dev/cu.*` call-out vs
`/dev/tty.*` dial-in), `IOSSIOSPEED`/`TIOCMGET` ioctls. ·
`[AROS]` `compiler/include/devices/serial.h` (`IOExtSer`, `SDCMD_*`, `SERF_*`,
`IO_STATF_*`, `SerErr_*`), `compiler/include/devices/parallel.h` (`IOExtPar`,
`PDCMD_*`), `compiler/include/exec/io.h` (`CMD_*` values), `workbench/devs/serial/`
(`serial_init.c` beginio/SETPARAMS→HIDD :712–818, HIDD open :77/:91/:170;
`serial_interrupthandlers.c:22`), `workbench/devs/parallel/`,
`workbench/hidds/serial/include/serial.h` (`Hidd_SerialUnit` methods :185, `TAG_*`/
`PARITY_*`), `arch/all-unix/hidd/serial/` (`SerialClass.c`, `SerialUnitClass.c` —
OpenFile :125 / Write :323 / receive :595 / the `#if 0` termios+async blocks;
`serial_intern.h` `HIDDSerialUnitData`), `arch/all-unix/hidd/parallel/
ParallelUnitClass.c` (the working `AddInterrupt`+`parallelunit_io` async path
:117/:237), `arch/all-unix/hidd/unixio/` (`unixio.conf` methods :54; `unixio.h`
`LibCInterface`/`LIBC_NAME` :35–67; `include/unixio.h` `uioInterrupt`/`vHidd_UnixIO_*`;
`unixio_class.c` darwin SIGALRM demux :855/:1177, `poll_fd`/`SigIO_IntServer` :110–162,
`AddInterrupt` :828, `WaitIntHandler` :158, `libc_symbols` :1107),
`arch/all-hosted/hostlib/` (`HostLib_Open/GetInterface/Lock`). ·
`[OURS]` H3 (`hosted/abishim.S`, host-call boundary), H11 (`hosted/device.c`,
IORequest→host I/O→reply, two-sided verify), H9/H10 (Wait/Signal, ports);
`docs/features/host-wake-pattern.md` (DARWIN-AARCH64 CAVEAT — host-context `Signal`
unsafe, poll the timer); `docs/features/bsdsocket-net/spec.md` §R-DARWIN-WAKE (the same
finding, proven live); `docs/features/host-volume/` (finish an existing all-unix overlay
for darwin); `harness`/`graft/aros-ctl` markers; `graft/WORKFLOW.md` (boot state). ·
`[DERIVED]` independently-derived points flagged for extra verification: (a) the serial
bridge inherits `unixio.hidd`'s timer-poll wake unchanged and needs **no** host thread
of its own [R-WAKE]; (b) tick-granularity receive latency is acceptable [R-LATENCY]; (c)
a host PTY-pair loopback is the right hermetic, TCC-free oracle for a tty — each restated
above from the `[AROS]` unixio darwin path + the host-wake CAVEAT `[OURS]` + POSIX/Apple
tty docs `[PUB]`. No third-party code, identifiers, or call sequence used.
