# Host media — physical disks and USB sticks inside Macaros

> Status: **working** · Target: aarch64-darwin hosted · Verified 2026-08-28
> Tool: [`graft/macaros-media`](../../../graft/macaros-media), also reachable as
> `aros-ctl media`.

Hosted AROS can mount a real macOS block device: a USB stick, an SD card, an
attached disk image. The volume shows up as an ordinary AROS device and is
handled by the ordinary AROS filesystem handler, so an exFAT stick written on a
Mac reads and writes inside AROS, and what AROS writes comes back clean on the
Mac.

macOS decides what is on offer, not AROS. The user grants a specific medium;
nothing else is reachable.

## How the medium reaches AROS

```
user grants a medium
        |
graft/macaros-media  ->  unmounts it on the macOS side
        |                writes DEVS:DOSDrivers/<NAME>
        v
AROS: Mount <NAME>  ->  exfat-handler / fat-handler
                              |
                        hostdisk.device  (opens /dev/diskN via hostlib)
                              |
                        macOS block device
```

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

```sh
aros-ctl media list                        # what macOS has, minus its own disk
aros-ctl media grant /dev/disk4s1          # read-only
aros-ctl media grant /dev/disk4s1 --rw     # AROS may write
aros-ctl media status
aros-ctl media revoke AROSEX               # give it back to macOS
```

`grant` refuses an internal disk, refuses a filesystem AROS has no handler for,
unmounts the volume on the macOS side, writes the mount description, and records
the grant. `aros-ctl run` re-resolves every grant against what is attached at
that moment, rewrites the mount description, and mounts it during startup: a
`/dev/disk` number is reassigned on every replug, so the grant is remembered by
volume identity (UUID where the medium has one, else name/size/filesystem).

Read-only is enforced on the host, by taking write permission off the device
node: `hostdisk.device` then opens it `O_RDONLY` and reports the unit as
write-protected. Confirmed live: reads work, an AROS write does not reach the
medium, and `fsck_exfat -n` stays clean.

Grants live in `aros-host.conf` as `media <name> <ro|rw> <fs> <identity>` lines,
next to the host app's other settings, so the Settings window can present the
same choice as a picker later. See [host-app-shell](../host-app-shell/README.md).

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
- The Settings window has no media picker yet; granting is CLI-only.
- The release bundle ships `hostdisk.device`, but a grant writes its mount
  description into whichever AROS tree `--aros` names. For the app that has to
  be the bundle's tree.

## Upstream state

The Darwin side of `hostdisk.device` needed three fixes, carried on the
`darwin-hostdisk` branch in `../aros-upstream`:

- the darwin build compiled the non-functional template host backend, because
  its arch mmakefile listed only `geometry`;
- `geometry.c` included `<sys/disk.h>`, which reaches host socket headers that
  do not compile here, and read a 64-bit block count into a `ULONG` field;
- the host backend defined `_DARWIN_NO_64_BIT_INODE`, a hard error on a
  64-bit-inode-only macOS, and retried read-only for `EBUSY`/`EROFS` but not for
  a node the user may read and not write.
