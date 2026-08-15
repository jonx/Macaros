# Initial platform bring-up archive

This directory contains the work that established Macaros. It is no longer the
day-to-day implementation plan; current work is indexed in
[`docs/features`](../../features/README.md).

It is kept because the sequence is useful when bringing AROS to another
architecture or hosted platform:

1. [Roadmap](ROADMAP.md) — why the CPU backend and host integration were split
   into independently testable phases.
2. [QEMU AArch64 bring-up](qemu-virt/PHASE1.md) — exception vectors, MMU, timer,
   memory, context switching, input, framebuffer, and preemption.
3. [Hosted macOS spikes](macos-hosted/PHASE2.md) — scheduling, host ABI,
   libraries, devices, and display inside an arm64 process.
4. [Graft notes](GRAFT.md) — how those results mapped into the real AROS tree.

The small QEMU kernel is retained in [`qemu-virt/boot`](qemu-virt/boot/). Its
original root Make targets still work:

```sh
make image
make test
make shot
```

These targets are architecture evidence and a porting scaffold. They are not a
bare-metal Apple Silicon roadmap; Macaros remains hosted under macOS.
