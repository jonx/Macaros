# Host media — physical disks and USB sticks inside Macaros

> Status: **working** · Target: aarch64-darwin hosted · Verified 2026-08-28
> In the app: Settings ▸ Media. On the command line:
> [`graft/macaros-media`](../../../graft/macaros-media), also `aros-ctl media`.

Hosted AROS can mount a real macOS block device: a USB stick, an SD card, an
attached disk image. The volume shows up as an ordinary AROS device and is
handled by the ordinary AROS filesystem handler, so an exFAT stick written on a
Mac reads and writes inside AROS, and what AROS writes comes back clean on the
Mac.

macOS decides what is on offer, not AROS. The user grants a specific medium;
nothing else is reachable.

## How the medium reaches AROS

```
Settings ▸ Media  (or aros-ctl media grant)
        |
        |  unmounts it on the macOS side, sets the node's permissions
        |  to the access granted, writes the mount description into
        v  the shared folder, records the grant in aros-host.conf
   <share>/.macaros-media/<NAME>
        |
        v  (MacRW:.macaros-media, polled by C:MediaWatch in AROS)
AROS: Mount <NAME>  ->  exfat-handler / fat-handler
                              |
                        hostdisk.device  (opens /dev/diskN via hostlib)
                              |
                        macOS block device
```

The descriptions live in the shared folder rather than in `DEVS:DOSDrivers`
because a released Macaros.app is signed and sealed: nothing may be written
inside the bundle's own AROS tree. `MediaWatch` mounts what appears there and
dismounts what is withdrawn, so a stick granted while the desktop is up shows
up without a reboot, and one taken back disappears within a second.

`hostdisk.device` is upstream AROS: a hosted device whose units are host device
nodes or image files, addressed either by unit number (`/dev/disk<N>` on Darwin)
or by a Mountlist `Unit` naming the node directly. It speaks the 64-bit
trackdisk commands and the media-change interrupt the exFAT handler expects.

## The route that does not work

Pointing `fdsk.device` at a device node through a host volume fails by design.
`fdsk.device` opens `FDSK:Unit<N>` as an ordinary DOS file, and emul-handler's
open path accepts only regular files and directories: a device node is
`ERROR_OBJECT_WRONG_TYPE` and a symlink to one is `ERROR_IS_SOFT_LINK`. Verified
on 2026-08-28 (the mount succeeds, the volume never appears). Relaxing that would
put every host device node behind any host volume, which is the opposite of what
the broker is for.

Poseidon over libusb is separately blocked on this host: macOS refuses ownership
of the mass-storage interface. See [exfat](../exfat/README.md).

## Using it

**In the app**: Settings ▸ Media lists the removable media the Mac has, with the
volume name, size, format, where it currently is, and a per-medium choice of
*Not shared* / *Read only* / *Read & write*. The list follows the hardware:
plug a stick in or pull it out and the table updates while the window is open.
A medium whose filesystem AROS has no handler for is listed but cannot be
shared.

**On the command line**:

```sh
aros-ctl media list                        # what macOS has, minus its own disk
aros-ctl media grant /dev/disk4s1          # read-only
aros-ctl media grant /dev/disk4s1 --rw     # AROS may write
aros-ctl media status
aros-ctl media revoke AROSEX               # give it back to macOS
```

Both halves do the same three things: refuse an internal disk and a filesystem
AROS has no handler for, take the volume from macOS, and write the description
plus the grant. A grant is remembered by volume identity (UUID where the medium
has one, else name/size/filesystem), never by device node, because a
`/dev/disk` number is reassigned on every replug. Every grant is re-resolved and
its description re-authored as the display comes up (`cm_media_prepare`, and
`macaros-media prepare` for the harness), so what AROS mounts always names the
node the medium has right now.

The size in a mount description is the **device's**, never the mounted
filesystem's: `diskutil`'s `TotalSize` is the volume's usable capacity, which is
short of the partition, and the exFAT handler correctly refuses a partition that
ends before the volume does.

Read-only is enforced on the host, by taking write permission off the device
node: `hostdisk.device` then opens it `O_RDONLY` and reports the unit as
write-protected. Confirmed live: reads work, an AROS write does not reach the
medium, and `fsck_exfat -n` stays clean.

Grants live in `aros-host.conf` as `media <name> <ro|rw> <fs> <identity>` lines,
next to the host app's other settings, which is why the window and the command
line see each other's decisions. See
[host-app-shell](../host-app-shell/README.md).

## What this boundary is, and is not

It is a place for a person to decide what to expose, and a guard against the two
ways this goes wrong in practice: handing over the wrong disk, and letting macOS
and AROS write the same filesystem at once.

It is not containment. Guest code can already reach the host directly through
`hostlib.resource` (see [host-bridge](../host-bridge/README.md)), so a program
determined to open a device node does not need `hostdisk.device` to do it. The
security boundary is the macOS process, not the guest.

## Known gaps

- The exFAT handler does not query `TD_PROTSTATUS`, so on a read-only grant a
  write fails at the device layer rather than returning
  `ERROR_DISK_WRITE_PROTECTED` with `ID_WRITE_PROTECTED`. The medium stays
  correct either way. The same gap applies to a stick with a physical
  write-protect switch.
- A medium withdrawn while AROS holds files open on it is dismounted anyway;
  AROS reports the device as gone rather than warning first.

## Tests

| Gate | What it proves |
|---|---|
| `make cocoametal-media` | The broker itself, on a disposable exFAT image attached as a real `/dev/disk` node: listed, granted read-only then read/write, withdrawn; the mount description, the recorded grant, the node's permissions and the macOS mount state after each step. |
| `make cocoametal-shell` | The Settings window really generates the Media tab and its device list, against the production dylib. |

The live path (grant while the desktop is up, `MediaWatch` mounts it, withdraw
it and watch it go) was verified by hand on 2026-08-28.

## Upstream state

Carried on the `darwin-hostdisk` branch in `../aros-upstream`: `C:MediaWatch`
is new, and the Darwin side of `hostdisk.device` needed three fixes:

- the darwin build compiled the non-functional template host backend, because
  its arch mmakefile listed only `geometry`;
- `geometry.c` included `<sys/disk.h>`, which reaches host socket headers that
  do not compile here, and read a 64-bit block count into a `ULONG` field;
- the host backend defined `_DARWIN_NO_64_BIT_INODE`, a hard error on a
  64-bit-inode-only macOS, and retried read-only for `EBUSY`/`EROFS` but not for
  a node the user may read and not write.
