# Serial bridge — host character devices for AROS `serial.device` (and `parallel.device`)

> Status: planned (not started) · Target: aarch64-darwin hosted · Drafted 2026-06-28

## What & why

Give the hosted AROS a working **`serial.device`** (the AmigaOS RS-232 API —
`CMD_READ`/`CMD_WRITE`/`SDCMD_SETPARAMS`/`SDCMD_QUERY`/`SDCMD_BREAK`) backed by a
real macOS character device: a USB-serial adapter at `/dev/cu.usbserial-*`, the
debug console at `/dev/cu.debug-console`, or — for hermetic, headless verification —
a **PTY pair** the host creates with `openpty`/`posix_openpt`. AROS opens
`serial.device` unit 0, sets 9600-8N1, writes "HELLO" → the bytes appear on the host
side of the PTY; the host writes back → `CMD_READ` returns them. Baud/bits/parity set
via `SDCMD_SETPARAMS` map to host `termios`. **`parallel.device`** rides the same
shim as a thinner secondary (byte-stream out to a host fd; `CopyToPAR`/printer paths
reach it).

This is the project thesis on one more surface — *"macOS owns the drivers; AROS
reaches them via standard exec I/O."* The Mac owns the tty/termios layer; AROS reaches
it through the standard `serial.device` IORequest surface it already expects, exactly
as a classic Amiga reached a UART. It re-uses the host-call boundary de-risked by H3
(`hosted/abishim.S`) and `hostlib.resource`, and — the load-bearing reuse — it plugs
into a backend seam **AROS already defines** (the serial HIDD), against a hosted/unix
backend that **already exists** in the tree.

## Does it already exist?

**Mostly — and more than for any other feature in this folder.** AROS's serial stack
is cleanly layered into a portable device shell, an abstract HIDD interface, and a
per-arch backend; for the hosted/unix case the backend **is already written** (though
heavily stubbed). The job is bring-up + fill-in for darwin-aarch64, not greenfield.

### The three layers (grounded)

1. **`serial.device` — the portable shell** (reused verbatim).
   `/Users/user/Source/aros-upstream/workbench/devs/serial/` (`serial_init.c`,
   `serial_support.c`, `serial_interrupthandlers.c`, `serial_intern.h`, `serial.conf`).
   It speaks the classic exec **IORequest** interface (`AROS_LH1 beginio`/`abortio`,
   `serial_init.c:307,871`), owns the per-unit input ring buffer, the read/write queues
   (`su_QReadCommandPort`/`su_QWriteCommandPort`), the `SU->su_Lock` semaphore, and the
   `STATUS_READS_PENDING`/`STATUS_WRITES_PENDING` machinery. It is **host-agnostic** —
   no syscalls. It produces no I/O itself: at init it does
   `OpenLibrary("DRIVERS:serial.hidd", 0)` and `OOP_NewObject(CLID_Hidd_Serial)`
   (`serial_init.c:77,91`), and every byte goes through the HIDD.

2. **the serial HIDD — the abstract backend seam** (the plug point).
   The interface is `workbench/hidds/serial/include/serial.h`: a `Hidd_Serial` class
   with `NewUnit`/`DisposeUnit`, and a `Hidd_SerialUnit` class with the methods that
   matter — `Init` (installs the device's receive/write callbacks), `Write`,
   `SetBaudrate`, `SetParameters` (data-length / stop-bits / parity tags), `SendBreak`,
   `Start`/`Stop`, `GetCapabilities`, `GetStatus` (`serial.h:86–99`, stubs in
   `workbench/hidds/serial/serial_stubs.c`). **This is where the host backend plugs
   in** — the device shell pokes no hardware; it drives whatever class answers to
   `CLID_Hidd_Serial`. The parallel equivalent is `workbench/hidds/parallel/`.

3. **the hosted/unix serial backend — ALREADY EXISTS** (the file to finish).
   `/Users/user/Source/aros-upstream/arch/all-unix/hidd/serial/` —
   `SerialClass.c` (the `NewUnit`/`DisposeUnit` factory, complete) and
   `SerialUnitClass.c` (the per-unit class). `SerialUnitClass.c` **opens a host
   character device over libc** and routes bytes through it:
   - On `New`, it creates a `unixio.hidd` object and
     `Hidd_UnixIO_OpenFile(unitname[unit], O_NONBLOCK|O_RDWR, …)`
     (`SerialUnitClass.c:116,125`).
   - `Write` → `Hidd_UnixIO_WriteFile(fd, buf, len)` (`:323`).
   - The receive path is an `AROS_INTH1 serialunit_receive_data` that does
     `Hidd_UnixIO_ReadFile` and calls the device's `DataReceivedCallBack`
     (`:595–623`) — i.e. it feeds `RBF_InterruptHandler` in
     `serial_interrupthandlers.c:22`, which copies into the device's input ring.

   **Honest state of this file (UNVERIFIED-until-built, but visible in source):**
   it is **Linux-flavoured and heavily `#if 0`'d**. The device names are
   `/dev/ttyS0..3` (`SerialUnitClass.c:57–63`) — wrong for macOS. The whole termios
   configuration (`settermios`, `adapt_termios`, baud via `cfsetspeed`), the
   `tcsetattr` calls, and — critically — the **async receive wiring**
   (`Hidd_UnixIO_AsyncIO`, the soft-int reply ports) are inside `#if 0` blocks
   (`:133–237, 263–281, 396–413, 513–520, 680–722, 733–808`), so today `SetBaudrate`
   merely validates the rate without applying it and `SetParameters`/`SendBreak` are
   no-ops. So the backend **compiles and can open + write a host device, but does not
   yet configure the line or deliver received bytes.** That fill-in is the bulk of this
   feature.

### The parallel side (secondary)

`workbench/devs/parallel/` mirrors serial (`OpenLibrary("DRIVERS:parallel.hidd")`,
`HIDD_Parallel_NewUnit`, `CMD_READ`/`CMD_WRITE`/`PDCMD_QUERY`/`PDCMD_SETPARAMS`,
`parallel_init.c:75,162`; `struct IOExtPar` in `compiler/include/devices/parallel.h`).
Its hosted/unix backend `arch/all-unix/hidd/parallel/ParallelUnitClass.c` is **more
complete than serial's**: it actually wires `Hidd_UnixIO_AddInterrupt` with a
`parallelunit_io(fd,mode,ptr)` handler that does read-on-readiness and write-when-
ready (`ParallelUnitClass.c:117–135, 237–264`) — the very async pattern serial leaves
`#if 0`'d. Parallel has far fewer line parameters (no baud/parity/stop-bits — it's a
byte port), so it is the *thinner* spec but the *better-wired* precedent for the
receive seam.

### The host-wake problem is ALREADY solved one layer down (the key finding)

The hard part of any blocking host I/O on this port — *how does the AROS task wake
when the host fd has data, without a foreign thread `Signal`ing AROS?* — is **not new
work here**, because both backends sit on **`unixio.hidd`**
(`arch/all-unix/hidd/unixio/`), and `unixio.hidd` already carries an explicit,
in-tree **darwin** fix:

- The classic Unix readiness mechanism is `F_SETOWN`+`O_ASYNC` → `SIGIO` →
  `SigIO_IntServer` demultiplexes the fd list → `Signal`s the waiter
  (`unixio_class.c:139–162, 1173`).
- **On darwin that signal never arrives** for pipes, and `F_SETOWN`/`O_ASYNC` fails
  (`EPERM`) on a controlling tty — the source says so verbatim
  (`unixio_class.c:855–867`). So `UXIO_Init` **also installs the same
  `SigIO_IntServer` on `SIGALRM`** (`unixio_class.c:1177–1192`): the fd-readiness
  poll runs from the periodic **timer IRQ** within one scheduler tick regardless of
  SIGIO. The readiness demux is a non-blocking `poll(fd, 0)` (`poll_fd`,
  `unixio_class.c:110–137`).

That is exactly the `timer.device`-poll resolution the rest of this folder converged
on independently (`host-wake-pattern.md` DARWIN-AARCH64 CAVEAT; bsdsocket
§R-DARWIN-WAKE). And the `Signal` that *does* fire (`WaitIntHandler`,
`unixio_class.c:158–162`) runs from the **AROS SIGALRM IRQ handler**, not a foreign
pthread — the path the in-tree comment marks safe. **Consequence for this feature: the
serial bridge needs no host pump thread of its own and no foreign-thread `Signal`.** It
inherits a safe, polled wake from `unixio.hidd`. This is the single biggest reason the
serial bridge is lower-risk than CoreAudio or sockets.

### What still needs doing (the gap)

1. **macOS device names.** Replace `/dev/ttyS*` with `/dev/cu.*` (the call-out, no-
   DCD-wait device on macOS; `/dev/cu.usbserial-*`, `/dev/cu.debug-console`, or a PTY
   slave path). The set must be runtime-selectable (env var / unit→path map) since
   macOS serial device names are dynamic. **UNVERIFIED:** the exact selection
   mechanism — decide at graft (a `SERIAL_UNIT0=/dev/cu.…` env var is the likely seed,
   mirroring `AROS_HOST_VOLUME`).
2. **termios line config.** Un-`#if 0` and port `settermios`/`SetBaudrate`/
   `SetParameters` to call `tcgetattr`/`cfsetspeed`/`tcsetattr` through `unixio.hidd`
   (add the missing libc symbols — see Design). Map `SDCMD_SETPARAMS` fields.
3. **the async receive + write-ready wiring.** Un-`#if 0` serial's `AddInterrupt`/
   async path (or adopt parallel's already-working `parallelunit_io` shape) so
   received bytes reach `RBF_InterruptHandler` and queued writes drain.
4. **`SDCMD_BREAK` / `tcsendbreak`** and `GetStatus` (modem lines via `TIOCMGET`).
5. **Build wiring + bring-up.** Confirm the darwin-aarch64 mmake builds
   `workbench-hidd-unix-serial`/`-parallel` and `unixio.hidd`, that `DRIVERS:serial.hidd`
   resolves, and that the device runs once `dos.library` + the boot set are up.

**External prior art (web-grounded, not in the AROS tree, stated for honesty).** The
historical hosted-Darwin AROS ports (i386/x86_64/PPC, X11-bound) shipped this same
`all-unix` serial/parallel HIDD but, like everything else, were never brought to
arm64/Apple Silicon; no macOS-arm64 serial backend exists upstream. macOS's own serial
contract (`/dev/cu.*` vs `/dev/tty.*`, the `cu` call-out semantics, `IOSSIOSPEED` for
non-standard baud) is the standard Apple/BSD tty model — used here as `[PUB]`, not from
any third-party implementation.

## Background: the AROS serial contracts (grounded)

### The device IORequest surface — `struct IOExtSer`

`compiler/include/devices/serial.h`: `struct IOExtSer` extends `IOStdReq` with
`io_CtlChar`, `io_RBufLen`, `io_ExtFlags`, `io_Baud`, `io_BrkTime`, `io_TermArray`,
`io_ReadLen`, `io_WriteLen`, `io_StopBits`, `io_SerFlags`, `io_Status`
(`serial.h:22–36`). Commands (`serial_init.c:50–61`, values via `exec/io.h:44–53`):

| Command | value | meaning |
|---------|------:|---------|
| `CMD_READ` | 2 | read up to `io_Length` bytes (or until 0 if `-1`) into `io_Data` |
| `CMD_WRITE` | 3 | write `io_Length` bytes (or NUL-terminated string if `-1`) |
| `CMD_CLEAR` | 5 | flush the input ring |
| `CMD_RESET` | 1 | abort active + queued reqs, reset buffers (falls into FLUSH) |
| `CMD_FLUSH` | 8 | abort all queued (not active) reqs |
| `CMD_START`/`CMD_STOP` | 7/6 | resume/suspend output (`HIDD_SerialUnit_Start/Stop`) |
| `SDCMD_QUERY` | `CMD_NONSTD`=9 | report `io_Status` + bytes-in-buffer in `io_Actual` |
| `SDCMD_BREAK` | 10 | send a line break for `io_BrkTime` µs |
| `SDCMD_SETPARAMS` | 11 | apply `io_Baud`/`io_ReadLen`/`io_WriteLen`/`io_StopBits`/`io_SerFlags`/`io_RBufLen`/`io_BrkTime` |
| `NSCMD_DEVICEQUERY` | — | new-style device query (`NSDEVTYPE_SERIAL`) |

`io_SerFlags` parity/handshake bits (`serial.h:46–61`): `SERF_PARTY_ON`,
`SERF_PARTY_ODD`, `SERF_7WIRE` (RTS/CTS), `SERF_QUEUEDBRK`, `SERF_SHARED`,
`SERF_EOFMODE`, `SERF_XDISABLED` (XON/XOFF off). `io_Status` bits (`:67–76`):
`IO_STATF_OVERRUN`, `IO_STATF_READBREAK`, `IO_STATF_XOFFREAD/WRITE`. Error codes
`SerErr_*` (`:91–100`: `BaudMismatch`, `BufErr`, `InvParam`, `LineErr`, `DevBusy`).

How `SDCMD_SETPARAMS` reaches the backend (`serial_init.c:712–818`): it copies flags
to the unit, and **on baud change** calls `HIDD_SerialUnit_SetBaudrate(io_Baud)`
(`:771`); **on read/write-len or stop-bits change** builds a TagItem list
(`TAG_DATALENGTH=io_ReadLen`, `TAG_STOP_BITS=io_StopBits`, `TAG_SKIP` for parity TODO)
and calls `HIDD_SerialUnit_SetParameters(tags)` (`:792–798`). So the host termios map
is entirely inside the HIDD's `SetBaudrate`/`SetParameters`.

### The HIDD method contract (the seam to implement)

`workbench/hidds/serial/include/serial.h:185–196`:
```
OOP_Object *HIDD_Serial_NewUnit(obj, unitnum);
void        HIDD_Serial_DisposeUnit(obj, unit);
BOOL  HIDD_SerialUnit_Init(unit, DataReceived, DRUserData, WriteData, WDUserData);
ULONG HIDD_SerialUnit_Write(unit, UBYTE *data, ULONG length);   /* -> bytes written */
BOOL  HIDD_SerialUnit_SetBaudrate(unit, ULONG baud);
BOOL  HIDD_SerialUnit_SetParameters(unit, struct TagItem *tags);
BYTE  HIDD_SerialUnit_SendBreak(unit, int duration);
void  HIDD_SerialUnit_Start/Stop(unit);
void  HIDD_SerialUnit_GetCapabilities(unit, tags);   /* fills BPSRate/DataLength lists */
UWORD HIDD_SerialUnit_GetStatus(unit);
```
`Init` hands the backend two device-side callbacks (`serial_init.c:173`):
`RBF_InterruptHandler(buf,len,unit,ud)` (received bytes →
`serial_interrupthandlers.c:22`) and `WBE_InterruptHandler(unit,ud)` (write-buffer-
empty → pull next queued write). The backend invokes `DataReceivedCallBack` from its
receive path and `DataWriteCallBack` from its write-ready path. `SetParameters` tags:
`TAG_DATALENGTH` (5/6/7/8), `TAG_STOP_BITS` (1/2), `TAG_PARITY`/`TAG_PARITY_OFF`
(`PARITY_EVEN`/`PARITY_ODD`), `TAG_SET_MCR` (`serial.h:163–174`).

### The host backend it plugs into — `unixio.hidd` (already darwin-aware)

`arch/all-unix/hidd/unixio/`. Methods (`unixio.conf:54–63`):
`OpenFile(name,flags,mode,errp)`, `CloseFile`, `ReadFile`, `WriteFile`,
`Wait(fd,mode)`, `Poll(fd,mode,errp)`, `AddInterrupt(struct uioInterrupt*)`,
`RemInterrupt`. The libc symbol table it resolves through `hostlib.resource`
(`unixio_class.c:1107` `libc_symbols[]`, opened against `LIBC_NAME`) currently has
`open/close/ioctl/fcntl/poll/read/write/getpid/__error/mmap/munmap/socket/sendto/
recvfrom/bind` (`unixio.h:50–67`). `LIBC_NAME` is `"libSystem.dylib"` on darwin
(`unixio.h:35–38`) — the same handle bsdsocket and emul-handler use. **It has no
termios symbols yet** — `tcsetattr`/`tcgetattr`/`cfsetspeed`/`tcsendbreak`/`openpty`
must be added (see Design).

`struct uioInterrupt { MinNode; int fd; int mode; void(*handler)(int,int,void*);
void *handlerData; }` (`unixio.h:19–26`); modes `vHidd_UnixIO_Read=0x1`,
`vHidd_UnixIO_Write=0x2`, `vHidd_UnixIO_RW`, `vHidd_UnixIO_Error=0x10`
(`unixio.h:42–45`). **The darwin readiness path** (the host-wake): `AddInterrupt`
registers the fd; on darwin `UXIO_Init` polls every registered fd from the SIGALRM
timer IRQ via `SigIO_IntServer`→`poll_fd` and fires the handler (`unixio_class.c:
855–867, 1177–1192, 110–156`). All host calls are bracketed
`HostLib_Lock(); … AROS_HOST_BARRIER; HostLib_Unlock();` (e.g. `:839–853`) — the same
boundary H3 de-risked.

### Reference points already de-risked in this repo

- **Host-call boundary / symbol resolution** — H3 (`hosted/abishim.S`), and
  `hostlib.resource` (`HostLib_Open`/`GetInterface`/`Lock`), already used by
  `unixio.hidd`, bsdsocket, and emul-handler against `libSystem.dylib`.
- **Blocking host I/O from a device, two-sided verify** — H11 (`hosted/device.c`:
  IORequest → device task → real macOS syscall → reply, asserted on both sides). The
  serial CMD_READ/CMD_WRITE path is the same shape one layer up (device shell + HIDD).
- **Darwin host-wake = timer poll** — `host-wake-pattern.md` (DARWIN-AARCH64 CAVEAT)
  and bsdsocket §R-DARWIN-WAKE; here it is *inherited* from `unixio.hidd`'s SIGALRM
  demux rather than re-built.

## Design

### Host side (POSIX termios on a tty/PTY, via `unixio.hidd` + libc)

No new dylib and no hand-written C ABI of our own: the host calls are POSIX
`termios`/tty primitives whose signatures are fixed by the standard, reached as opaque
libc function pointers through `unixio.hidd`'s existing `hostlib.resource` mechanism
(the bsdsocket lesson — when the host side *is* libc, there is no ABI to invent). The
net-new host surface is **a handful of libc symbols added to `unixio.hidd`'s
`libc_symbols[]`**, all `[PUB]` in `libSystem.dylib`:

```
tcgetattr  tcsetattr  cfsetispeed  cfsetospeed  cfmakeraw  tcsendbreak  tcflush
openpty                         /* for the hermetic PTY verification path */
/* optional, non-standard baud / modem lines: ioctl already present (IOSSIOSPEED, TIOCMGET) */
```

`tcgetattr`/`tcsetattr`/`cfset*speed`/`cfmakeraw`/`tcsendbreak` carry the line config;
`openpty` (or `posix_openpt`+`grantpt`+`unlockpt`+`ptsname` on the existing
`open`/`ioctl`) builds the loopback for verification. The data path (`read`/`write`)
and the readiness path (`poll`+`AddInterrupt`) are already present.

**Device selection.** macOS uses `/dev/cu.<name>` for the call-out (no DCD-wait)
device — the right choice for a host bridge — and `/dev/tty.<name>` for dial-in. Unit
0..3 map to a runtime-supplied path list (env var seed `SERIAL_UNITn=…`, default the
PTY slave for the unattended loop). The data-carrier-detect / blocking-open trap of
`/dev/tty.*` is avoided by using `/dev/cu.*` + `O_NONBLOCK` (already the open flags,
`SerialUnitClass.c:125`).

**Why no host thread.** Unlike CoreAudio (RT callback thread) and bsdsocket (kqueue
pump thread), the serial bridge needs **no background host thread at all**: readiness
is delivered by `unixio.hidd`'s SIGALRM-timer poll of the registered fd
(`unixio_class.c:1177–1192`), which runs on the AROS scheduler thread, and the wake
`Signal` (`WaitIntHandler`) fires from that AROS IRQ context — the safe darwin path.
This sidesteps the entire foreign-thread hazard that dominates the other two features.

### AROS side (the hosted serial/parallel HIDD — finish the existing file)

Work lands in `arch/all-unix/hidd/serial/SerialUnitClass.c` (and the parallel peer),
guarded `HOST_OS_darwin` where behaviour diverges so the Linux backend is unchanged:

- **`New`**: open `unixio.hidd` (done), pick the darwin device path (new), `OpenFile`
  with `O_NONBLOCK|O_RDWR` (done), `tcgetattr`+`cfmakeraw`+default 9600-8N1+`tcsetattr`
  (un-`#if 0` + retarget), then `AddInterrupt` for `vHidd_UnixIO_RW` with a
  `serialunit_io(fd,mode,ptr)` handler modelled on parallel's `parallelunit_io`
  (`ParallelUnitClass.c:117,237`) — the async wiring serial leaves stubbed.
- **`Write`** → `Hidd_UnixIO_WriteFile` (done); on short write, leave the rest queued
  and let the write-ready half of the interrupt drain it (`WBE_InterruptHandler`).
- **`SetBaudrate`** → validate against the rate table (done), then
  `tcgetattr`+`cfsetispeed`+`cfsetospeed`+`tcsetattr` (un-`#if 0`); for non-POSIX
  rates use `ioctl(fd, IOSSIOSPEED, &speed)` `[PUB]` (macOS).
- **`SetParameters`** → map `TAG_DATALENGTH`→`CSIZE`/`CS5..CS8`, `TAG_STOP_BITS`→
  `CSTOPB`, parity→`PARENB`/`PARODD`, `SERF_7WIRE`→`CRTSCTS`, `SERF_XDISABLED`→
  `IXON|IXOFF` off, via `settermios` (un-`#if 0`).
- **`SendBreak`** → `tcsendbreak(fd, duration)` (un-`#if 0`, `:513`).
- **the receive handler** (`serialunit_io`, read half): `Hidd_UnixIO_ReadFile` then
  `DataReceivedCallBack(buf,len,unit,ud)` → `RBF_InterruptHandler` copies into the
  device input ring (`serial_interrupthandlers.c:22`). This is the seam that, on
  darwin, is driven by the SIGALRM timer poll — no new wake code.
- **`GetStatus`** → `ioctl(fd, TIOCMGET)` modem lines → `io_Status` (optional, first
  cut returns 0 as today).

Parallel is the same shape minus baud/parity/stop-bits — its `ParallelUnitClass.c`
already has the `AddInterrupt`+`parallelunit_io` wiring, so the parallel fill-in is
mostly device-path + build bring-up.

### The bridge (`SDCMD_SETPARAMS` → termios, and the verification loopback)

`SDCMD_SETPARAMS` field → termios mapping (the load-bearing translation):

| AROS (`IOExtSer`) | termios |
|-------------------|---------|
| `io_Baud` | `cfsetispeed`+`cfsetospeed` (or `IOSSIOSPEED` ioctl for non-standard) |
| `io_ReadLen`/`io_WriteLen` (5–8) | `c_cflag` `CSIZE` ← `CS5/CS6/CS7/CS8` |
| `io_StopBits` (1/2) | `c_cflag` `CSTOPB` (set ⇒ 2) |
| `io_SerFlags` `SERF_PARTY_ON`/`SERF_PARTY_ODD` | `PARENB` / `PARODD` |
| `io_SerFlags` `SERF_7WIRE` | `CRTSCTS` (RTS/CTS HW handshake) |
| `io_SerFlags` `SERF_XDISABLED` | clear `IXON|IXOFF` (else XON/XOFF on) |
| raw byte stream (always) | `cfmakeraw` baseline (no canon, no echo, 8-bit clean) |

**Verification loopback (the unattended core).** A PTY pair makes the whole thing
hermetic and headless: the host shim `openpty()`s → it gets a master fd and a slave
path; AROS opens `serial.device` on that **slave** path. Then:
- AROS `CMD_WRITE`s known bytes → the host reads them off the **master** fd → assert
  byte-exact.
- The host writes known bytes to the **master** → the SIGALRM poll fires the receive
  handler → `CMD_READ` returns them → assert byte-exact.
- `SDCMD_SETPARAMS` baud/bits set on the slave → host `tcgetattr`s the master (PTYs
  propagate termios) → assert the negotiated `speed`/`CSIZE`/`CSTOPB` match.

This is the H11 two-sided check (the device writes, the host independently re-reads)
applied to a tty, with no hardware, no human, and no TCC (a PTY the process owns needs
no entitlement).

## Plan — spikes in the loop

One standalone host binary per marker (H-series style, greppable), single PASS/FAIL.
Markers **[SR1]…[SR7]**. Early spikes prove the host termios+PTY mechanism in
isolation (before `dos.library`); later ones run inside booted AROS.

- **[SR1] host PTY + termios round-trip (pure host probe, no AROS).** `openpty` →
  set the master raw → `tcsetattr` the slave to 9600-8N1 → write a known pattern slave
  →master and master→slave → assert byte-exact both ways; `tcgetattr` the master and
  assert `cfgetospeed==B9600`, `CSIZE==CS8`, `CSTOPB` clear. Grounds the verification
  harness + the termios map before any AROS, like H7's `pngprobe`. `[SR1]`.
- **[SR2] the libc symbol set resolves.** `dlopen libSystem.dylib` (the
  `hostlib.resource` path) and resolve every symbol the bridge adds —
  `tcgetattr/tcsetattr/cfsetispeed/cfsetospeed/cfmakeraw/tcsendbreak/tcflush/openpty`
  — plus the existing `open/read/write/poll/ioctl/fcntl`. PASS = all resolve. Catches
  the symbol-availability risk early (the bsdsocket `[NABI]` discipline). `[SR2]`.
- **[SR3] backend Write + poll-driven receive (bare spike).** Exercise the
  `SerialUnitClass` shape against a PTY without the full device: `OpenFile` the slave,
  `WriteFile` → host master reads it; host master writes → a `poll(fd,0)` (the
  `unixio` darwin demux) reports readable → `ReadFile` returns the bytes. PASS =
  both directions byte-exact, and the readiness was found by poll (no SIGIO). Proves
  the receive seam works on the timer-poll model. `[SR3]`.
- **[SR4] SDCMD_SETPARAMS honoured.** Drive the param map: set 19200-7E2, assert the
  host master's `tcgetattr` reports `B19200`, `CS7`, `PARENB` (no `PARODD`), `CSTOPB`;
  set 115200-8N1, re-assert. PASS = every field landed. `[SR4]`.
- **[SR5] graft: real `serial.device` CMD_READ/CMD_WRITE on booted AROS.** Build the
  finished `workbench-hidd-unix-serial` + `serial.device` for darwin-aarch64, boot
  windowed, `OpenDevice("serial.device", 0)` on the PTY slave, `CMD_WRITE "HELLO"` →
  host master asserts the bytes; host master writes "WORLD" → `CMD_READ` returns it.
  PASS = two-sided byte-exact through the real device LVOs. The full thesis end-to-end.
  Rides the crosstools graft (depends on `dos.library` + boot set). `[SR5]`.
- **[SR6] SDCMD_SETPARAMS / SDCMD_QUERY / SDCMD_BREAK on the real device.** Set baud
  via `SDCMD_SETPARAMS` (assert host master termios), `SDCMD_QUERY` after the host
  queues N bytes (assert `io_Actual==N`), `SDCMD_BREAK` (assert the host master sees a
  break condition). `[SR6]`.
- **[SR7] parallel.device byte-out (secondary).** `OpenDevice("parallel.device",0)` on
  a PTY slave, `CMD_WRITE` → host master asserts the bytes; `parallelunit_io` receive
  half delivers a host-written byte to `CMD_READ`. PASS = both directions byte-exact.
  Confirms the same shim carries parallel. `[SR7]`.

Build/run in the existing harness style (`make hosted-serial` → `[SR?]` markers for
[SR1]–[SR4]; `graft/aros-ctl` for the booted [SR5]–[SR7]), clean-exit on PASS.

## How we verify it unattended

No human, no TCC, no real cable. The oracle is **a PTY pair the host process owns** +
**byte-exact assertions** on both ends (the H11 two-sided discipline). A PTY needs no
entitlement (it is ordinary file I/O by the AROS process under the launching terminal's
permissions — same stance as host-volume). Assertions per marker:

- **byte-exact** for every data direction (the truth, since serial is a pure byte pipe
  — no DSP, so an exact checksum/compare is the whole test).
- **termios fidelity**: after `SDCMD_SETPARAMS`, the host master's `tcgetattr` must
  report the exact `speed`/`CSIZE`/`CSTOPB`/`PARENB`/`PARODD`.
- **`SDCMD_QUERY` count**: `io_Actual` equals the bytes the host queued.
- **readiness-via-poll**: assert the receive path fired from the timer-poll demux, not
  SIGIO (the darwin host-wake correctness).
- a **bash watchdog** (TERM-then-KILL, no GNU `timeout` on stock macOS) reaps a hung
  read into a FAIL.

Live tests on booted AROS render verdicts in the window and are captured via
`graft/aros-ctl shot`; the host side of the PTY is the independent oracle (it logs the
bytes it saw), exactly as bsdsocket's echo server is for sockets.

## Risks & open questions

- **The existing backend is half-stubbed (the headline work, UNVERIFIED-until-built).**
  `SerialUnitClass.c`'s termios + async-receive code is `#if 0`'d and Linux-flavoured;
  it compiles+opens+writes today but does not configure the line or deliver received
  bytes. The fill-in is straightforward (the map is grounded above and parallel's peer
  already wires the async path), but it is the bulk of the feature and unproven until a
  build. Mitigation: [SR3]/[SR4] prove the mechanism in a bare spike before the graft.
- **macOS device naming is dynamic.** `/dev/cu.usbserial-*` names vary by adapter; the
  unit→path map must be runtime-supplied. **UNVERIFIED:** the exact selection UX
  (env-var seed assumed). The PTY path keeps the loop hermetic regardless.
- **`unixio.hidd` build/run on darwin-aarch64.** The whole bridge depends on it; it has
  the darwin SIGALRM-demux fix in source but **UNVERIFIED whether it builds/runs for
  this target** (the same gap host-volume flags for the emul-handler overlay). Confirm
  at graft — it is the shared prerequisite.
- **Non-standard baud.** POSIX `cfsetspeed` only covers the `B*` constants; arbitrary
  rates (e.g. 31250 MIDI) need macOS `ioctl(fd, IOSSIOSPEED)` `[PUB]`. First cut
  supports the standard table (`valid_baudrates[]`, `SerialUnitClass.c:333`); MIDI/odd
  rates are a flagged follow-on.
- **Break detection / modem status.** Inbound `BREAK` → `IO_STATF_READBREAK` and
  `GetStatus`→modem lines (`TIOCMGET`) are first-cut-optional (return 0/clear as the
  stub does today); add when a real adapter is in play.
- **PTY termios propagation.** The assumption that setting termios on the slave is
  visible via `tcgetattr` on the master holds for macOS PTYs, but the *exact* fields
  that propagate (vs. master-only attrs) is **UNVERIFIED** — confirm in [SR1] and, if
  needed, assert by reading the slave's own `tcgetattr` instead.
- **Shared vs exclusive open.** `SERF_SHARED` multi-opener semantics
  (`serial_init.c:204`) over one host fd are faithful to the device shell but untested
  on a host tty (two AROS openers, one `/dev/cu.*`); note it, defer.
- **The graft, not a spike.** [SR5]–[SR7] depend on the crosstools + `mmake` producing
  `serial.device`/`parallel.device`/`unixio.hidd` and on `dos.library` + the boot set
  (the kickstart still halts at cold-start, `graft/WORKFLOW.md`). [SR1]–[SR4] are
  session-sized and stand alone.

## References

AROS upstream (`/Users/user/Source/aros-upstream`):
- Device shell: `workbench/devs/serial/` (`serial_init.c` beginio/abortio :307/:871,
  Open/HIDD wire :77/:91/:170, SETPARAMS→HIDD :712–818; `serial_interrupthandlers.c`
  `RBF_InterruptHandler` :22; `serial_intern.h`, `serial.conf`),
  `workbench/devs/parallel/` (`parallel_init.c` :75/:162; `parallel_intern.h`).
- Device API headers: `compiler/include/devices/serial.h` (`IOExtSer`, `SDCMD_*`,
  `SERF_*`, `IO_STATF_*`, `SerErr_*`), `compiler/include/devices/parallel.h`
  (`IOExtPar`, `PDCMD_*`), `compiler/include/exec/io.h` (`CMD_*` values).
- HIDD seam: `workbench/hidds/serial/include/serial.h` (`Hidd_Serial`/`Hidd_SerialUnit`
  methods, `TAG_*`/`PARITY_*`, stub protos :185), `workbench/hidds/serial/serial_stubs.c`,
  `workbench/hidds/parallel/include/parallel.h`.
- Hosted/unix backend (exists): `arch/all-unix/hidd/serial/` (`SerialClass.c` factory,
  `SerialUnitClass.c` host-device class — OpenFile :125, Write :323, receive
  :595–623, the `#if 0` termios/async blocks; `serial_intern.h`, `serial.conf`,
  `unix_funcs.c`), `arch/all-unix/hidd/parallel/` (`ParallelUnitClass.c` —
  `AddInterrupt`+`parallelunit_io` async :117/:237, the working receive precedent).
- Host backend + darwin wake: `arch/all-unix/hidd/unixio/` (`unixio.conf` methods
  :54–63; `unixio.h` `LibCInterface`/`LIBC_NAME=libSystem.dylib` :35–67; public
  `include/unixio.h` `uioInterrupt`/`vHidd_UnixIO_*`; `unixio_class.c` darwin SIGALRM
  demux :855–867/:1177–1192, `SigIO_IntServer`/`poll_fd` :110–162, `AddInterrupt`
  :828, `WaitIntHandler` :158, libc_symbols :1107).
- Host-symbol mechanism: `arch/all-hosted/hostlib/`, `arch/all-unix/bootstrap/`.

This repo (`/Users/user/Source/aros-aarch64`):
- `docs/features/host-wake-pattern.md` (DARWIN-AARCH64 CAVEAT — host-context `Signal`
  unsafe; poll `timer.device`), `docs/features/bsdsocket-net/{design.md,spec.md}`
  (§R-DARWIN-WAKE — the same finding, proven live), `docs/features/host-volume/`
  (the "finish an existing all-unix overlay for darwin" precedent).
- `NOTES.md` H3 (`hosted/abishim.S` host-call shim), H11 (`hosted/device.c` —
  IORequest→device task→host syscall→reply, two-sided verify), H9/H10 (Wait/Signal,
  ports). `hosted/host.c`, `hosted/signal.c`, `hosted/msgport.c`.

External / web (prior art + macOS contract, not in the AROS tree):
- macOS tty model: `/dev/cu.*` (call-out, no DCD-wait) vs `/dev/tty.*` (dial-in);
  `termios(4)`, `tcsetattr(3)`, `cfsetspeed(3)`, `tcsendbreak(3)`, `openpty(3)`;
  `IOSSIOSPEED` ioctl for non-standard baud (Apple `IOKit`-serial / `man` pages).
  Used as `[PUB]` only.
- Historical hosted-Darwin AROS (i386/x86_64/PPC, X11) shipped this same `all-unix`
  serial/parallel HIDD but never on arm64 — confirms the design is the proven Unix path
  brought to a new target, not new invention.
