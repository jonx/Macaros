# HARDWARE.md — grounded reference for the QEMU `virt` target

**Ground your work, don't dream it.** Every value here is from an authoritative
source, not from priors. Primary source is the device tree the *actual* QEMU
binary emits for the *exact* flags we boot with — regenerate it any time with
`make dtb` (→ `build/virt.dts`). Docs are secondary cross-checks.

- **Target:** `qemu-system-aarch64 -M virt -cpu cortex-a72` — QEMU **11.0.1**
- **Primary source:** the dumped DTB (`make dtb`)
- **Secondary:** [QEMU virt board docs](https://www.qemu.org/docs/master/system/arm/virt.html),
  [arm64 Linux boot protocol](https://www.kernel.org/doc/Documentation/arm64/booting.rst)

## Memory map (from the DTB)

| Region | Base | Size | DTB node / notes |
|---|---|---|---|
| RAM | `0x4000_0000` | `-m` size (≥128 MiB default) | `memory@40000000` |
| PL011 UART0 | `0x0900_0000` | `0x1000` | `pl011`, IRQ = **SPI 1**, level-high |
| GIC distributor (GICD) | `0x0800_0000` | `0x10000` | `intc`, `arm,cortex-a15-gic` = **GICv2** |
| GIC CPU interface (GICC) | `0x0801_0000` | `0x10000` | second `reg` entry |
| GICv2m MSI frame | `0x0802_0000` | `0x1000` | `v2m@8020000` |
| Generic timer (EL1 phys) | sysreg (CNTP_*) | — | `arm,armv8-timer`, **PPI 14 → INTID 30** |

We link the image at `0x4008_0000` (RAM base + `0x80000`). QEMU's ELF loader
places segments at their `p_paddr` and jumps to `e_entry`, so the 2 MiB-aligned
"Image" placement rule does not apply to us (we are an ELF, not a Linux Image).

## Boot / entry state

Verified at M1/M2 (the `-d int` trace + what `kmain` prints), cross-checked
against the arm64 boot protocol:

- Primary CPU enters at `_start` (`e_entry` = `0x4008_0000`) at **EL1**.
  (The protocol permits EL1 or EL2; QEMU virt default with no `virtualization=on`
  is EL1, and `CurrentEL` read in C confirms `EL1`.)
- **MMU off**, all interrupts **masked in `PSTATE.DAIF`**. → this is *why* C is
  built with `-mstrict-align` (RAM is Device memory with the MMU off, so
  unaligned accesses fault) and `-mgeneral-regs-only` (FP/NEON not enabled yet).
- **`x0 = 0` on entry** — NOT the DTB. ⚠️ The boot protocol's `x0 = DTB phys`
  applies to the Image-header path; QEMU's ELF `-kernel` path zeroes the GPRs.
  Proven at M2 (`x0=0x0000000000000000`). **Consequence for M6:** to read RAM
  size etc. from a device tree we must obtain it explicitly (e.g. `-dtb` at a
  known load address), not from x0.

## Interrupt controller — ⚠️ docs vs. reality

The QEMU docs say the *default* GIC is **v3**. The DTB our exact invocation emits
says **v2** (`arm,cortex-a15-gic`). The real machine wins. **Decision for M5:**
pin the version explicitly (`-machine virt,gic-version=N`) so the timer-IRQ work
isn't built against a default that can shift between QEMU versions/CPUs. Leaning
GICv2 (pure-MMIO at the addresses above, simplest first interrupt controller).

## PSCI (from the DTB)

- `compatible = "arm,psci-1.0"`, **`method = "hvc"`** — PSCI calls go via `HVC`.
- Implication: QEMU holds **secondary CPUs powered-off** until `CPU_ON`, so the
  MPIDR park in `start.S` is belt-and-suspenders (secondaries don't reach it).
- Grounded clean-shutdown alternative to semihosting: **`SYSTEM_OFF`** (function
  `0x8400_0008`) via `HVC`. We currently exit via semihosting `SYS_EXIT` (gives
  an exit code); PSCI is the "no debug feature required" option if we want it.

## Semihosting

Enabled in the harness with `-semihosting-config enable=on,target=native`. Exit
uses `SYS_EXIT` (`0x18`); on **AArch64** the parameter register points to a
`{reason, exit_code}` block — passing the reason directly (AArch32 style) is
wrong and was the cause of the early `qemu_exit=1`. Now exits 0.
