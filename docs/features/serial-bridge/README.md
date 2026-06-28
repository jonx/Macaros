# Serial bridge — host tty for AROS `serial.device` (+ `parallel.device`)

> Status: **planned (not started)** · Target: aarch64-darwin hosted · Drafted 2026-06-28
> Design rationale: [design.md](design.md) · Implementation spec: [spec.md](spec.md).

## What & why

Give the hosted AROS a working **`serial.device`** (the AmigaOS RS-232 API —
`CMD_READ`/`CMD_WRITE`/`SDCMD_SETPARAMS`/`SDCMD_QUERY`/`SDCMD_BREAK`) backed by a real
macOS character device: a USB-serial adapter (`/dev/cu.usbserial-*`), the debug console,
or — for hermetic verification — a host **PTY pair**. AROS sets 9600-8N1 and writes
bytes; they appear on the host tty. The host writes back; `CMD_READ` returns them.
Baud/bits/parity from `SDCMD_SETPARAMS` map to host `termios`. **`parallel.device`** rides
the same shim as a thinner secondary (byte-out for `CopyToPAR` / printer paths).

The project thesis on one more surface — *macOS owns the drivers; AROS reaches them via
standard exec I/O.* The Mac owns the tty/termios layer; AROS reaches it through the
`serial.device` IORequest surface it already expects, exactly as a classic Amiga reached
a UART.

## Why this one is lower-risk

This feature is **mostly bring-up, not greenfield** — more so than anything else in this
folder:

- **AROS already defines the backend seam.** `serial.device` is a host-agnostic shell
  that opens `DRIVERS:serial.hidd` and drives whatever class answers it
  (`workbench/devs/serial/serial_init.c:77`). We plug in at the HIDD, touching no shell.
- **The hosted/unix backend already exists.** `arch/all-unix/hidd/serial/
  SerialUnitClass.c` already opens + writes a host device over `unixio.hidd`. It is
  Linux-flavoured (`/dev/ttyS*`) and its termios + async-receive code is `#if 0`'d, so
  the work is **finish + retarget to macOS**, not write-from-scratch. Parallel's peer
  (`ParallelUnitClass.c`) already wires the async receive path — the precedent to copy.
- **The hard part — the darwin host-wake — is already solved one layer down.** Both
  backends sit on `unixio.hidd`, which **already** drives its fd-readiness poll from the
  SIGALRM timer IRQ on darwin (SIGIO never arrives here;
  `unixio_class.c:855–867,1177–1192`) and wakes from AROS IRQ context — the safe path the
  rest of this folder converged on independently
  ([host-wake-pattern.md](../host-wake-pattern.md), bsdsocket §R-DARWIN-WAKE). So the
  serial bridge needs **no host thread of its own and no foreign-thread `Signal`** — the
  hazard that dominates CoreAudio and sockets simply doesn't arise.

## The shape

```
serial.device  (exec IORequest; reused verbatim)            [AROS]
   │ HIDD_SerialUnit_Write / SetBaudrate / SetParameters
   ▼
all-unix serial HIDD  (finish termios + async receive)      [AROS, the work]
   │ Hidd_UnixIO_OpenFile/Write/Read + AddInterrupt(fd, RW)
   ▼
unixio.hidd  (darwin-aware: SIGALRM-timer fd-poll wake)      [AROS, reused]
   │ hostlib.resource + HostLib_Lock  →  libSystem.dylib
   ▼
a /dev/cu.* tty   OR   a host PTY slave (master = verify oracle)   [PUB]
```

`SDCMD_SETPARAMS` → termios: `io_Baud`→`cfset*speed`, `io_ReadLen`→`CSIZE`,
`io_StopBits`→`CSTOPB`, `SERF_PARTY_ON/ODD`→`PARENB`/`PARODD`, `SERF_7WIRE`→`CRTSCTS`.

## Verification — unattended, no TCC

Hermetic and headless via a **host PTY pair** the process owns (no entitlement needed):
the shim `openpty()`s, AROS opens the slave, the master is the independent oracle. Every
test asserts **byte-exact** transport (serial is a pure byte pipe) and **termios
fidelity** (the master's `tcgetattr` must match what `SDCMD_SETPARAMS` set) — the H11
two-sided check applied to a tty.

Markers **[SR1]–[SR7]** (one host binary each, single PASS/FAIL):

| Marker | Proves |
|--------|--------|
| `[SR1]` | host PTY + termios round-trip (pure host probe) |
| `[SR2]` | the libc symbol set resolves through `hostlib.resource` |
| `[SR3]` | backend Write + poll-driven receive (bare spike, no SIGIO) |
| `[SR4]` | `SDCMD_SETPARAMS` baud/bits/parity honoured by termios |
| `[SR5]` | graft — real `serial.device` CMD_READ/CMD_WRITE on booted AROS |
| `[SR6]` | `SDCMD_SETPARAMS` / `SDCMD_QUERY` / `SDCMD_BREAK` on the real device |
| `[SR7]` | `parallel.device` byte-out (secondary) |

[SR1]–[SR4] are session-sized bare host spikes; [SR5]–[SR7] ride the crosstools graft
(need `dos.library` + the boot set).

## Biggest unknowns

- The existing backend is half-stubbed (the bulk of the work — termios + async receive);
  grounded but **UNVERIFIED until built**.
- `unixio.hidd` building/running on darwin-aarch64 — the shared prerequisite (the same
  gap [host-volume](../host-volume/) flags).
- macOS serial device names are dynamic; the PTY path keeps the loop hermetic regardless.

## Provenance

Independent work: no third-party implementation source — emulator, agent, driver, or
otherwise — was read, searched, or consulted; built from POSIX/Apple `termios`+tty docs,
the in-tree AROS serial/parallel/unixio modules, and this project's H-series spikes. See
[spec.md](spec.md)'s provenance banner and [../CLEANROOM.md](../CLEANROOM.md).
