# Phase 1 — AArch64 backend on QEMU `virt`

The genuine contribution: bring AROS's lowest layer up on 64-bit ARM, on a fully
observable target, before any Apple Silicon work. Every milestone is gated by the
autonomous loop — `make run MARKER='[Mx]'` must go green with **zero manual steps**.
"Observe" names the faithful way of *seeing* for that milestone.

Status legend: ✅ done · 🔜 next · ⬜ planned

---

### A0 — The loop closes ✅
The harness builds an AArch64 ELF, boots it headless on QEMU `virt`, observes, and
emits one uniform PASS/FAIL verdict. PASS→exit 0, FAIL→non-zero, so the agent can
tell when it broke something. All text/state observation channels validated on the
Mac: serial markers, QEMU fault trace, lldb gdbstub CPU-state (with symbols).
**Observe:** the verdict block itself.

### M1 — Serial alive ✅
Bare-metal stub enters at EL1, inits the PL011 UART, prints `[M1] aarch64 serial
alive`, exits cleanly via semihosting. Confirmed entry EL = EL1.
**Observe:** serial marker. **Files:** `boot/start.S`, `boot/linker.ld`.

### M2 — C runtime ✅
`start.S` zeroes `.bss`, sets the stack, hands off to `kmain()` in C. C carries a
PL011 driver + a tiny `kprintf` (verified: `%d %u %x %lx %p %s %c`) that later
milestones reuse. Built `-mstrict-align -mgeneral-regs-only` (MMU off → Device
memory → aligned-only; FP not enabled yet). Confirmed **EL1** from C. Grounding
caught that QEMU's ELF `-kernel` enters with **x0 = 0, not the DTB** (HARDWARE.md).
**Observe:** `[M2] hello from C (EL1) x0=0x0` marker.
**Files:** `boot/start.S`, `boot/kmain.c`, `boot/linker.ld`.

### M3 — Exception vectors ✅
`boot/vectors.S` installs `VBAR_EL1` → a 16-entry × `0x80` table (2 KiB aligned);
each slot saves a trap frame and calls `exc_handler` in `boot/exc.c`. Deliberately
traps via `svc #0` (EC `0x15`, resume) and `brk #0` (EC `0x3c`, skip via `ELR+=4`).
EC values **grounded** from Linux `esr.h` and **verified live** (the handler prints
them); `SPSR=0x...3c5` independently confirms EL1h + DAIF masked. Names foreshadow
AROS `intvecs.s`/`intr.c`/`__vectorhand_*`.
**Observe:** `[M3] vectors ok` + the printed EC. **Files:** `boot/vectors.S`,
`boot/exc.c`, `boot/kern.h`.

### M4 — MMU on ✅
`boot/mmu.c` builds one L1 table (4KB granule, T0SZ=25, 1GB identity blocks:
device for the low GB incl. PL011/GIC, Normal-cacheable+exec for RAM), sets
MAIR/TCR/TTBR0, enables SCTLR.M|C|I — and *survives*. Then proves translation is
real: touching unmapped `0x8000_0000` faults with **EC=0x25, FAR=0x8000_0000,
DFSC=translation-fault-L1** (grounded ESR decode), handler recovers. Descriptor
bits grounded from Linux `pgtable-hwdef.h`. (`-mstrict-align` still set: early boot
before `mmu_init` runs MMU-off, so it stays load-bearing there.)
**Observe:** `[M4]` prints only after the deliberate fault recovers.
**Files:** `boot/mmu.c`.

### M5 — Timer interrupt ✅
`boot/irq.c` brings up **GICv2** (GICD `0x0800_0000` / GICC `0x0801_0000`, offsets
grounded from Linux `arm-gic.h`) and the **EL1 physical timer** (CNTP, INTID 30) at
100 Hz; the IRQ handler counts ticks, re-arms, EOIs. Harness now pins
`virt,gic-version=2` so the controller can't drift. Group-0 IRQ-vs-FIQ ambiguity
handled by routing both vectors to the dispatcher + unmasking both.
**Observe:** `[M5] timer IRQ ok, ticks=5`. **Files:** `boot/irq.c`.

### M6 — Physical memory ✅
`boot/pmm.c`: free-list page allocator, heap = `[page-aligned _end .. 0x6000_0000)`
(RAM-top tied to the pinned `-m 512`; DTB memory node is the eventual proper
source — x0 doesn't give it to us). Verified: `total≈130935 pages`, write/read-back
ok, LIFO free+reuse ok. **Observe:** `[M6] pmm ok: ...`. **Files:** `boot/pmm.c`.

### M7 — Context switch ✅
`boot/switch.S` (`ctx_switch`) saves/restores AAPCS callee-saved state + SP;
`boot/task.c` runs two cooperative tasks on separate pmm stacks. Verified perfect
A/B alternation with each task's loop counter preserved and distinct stack
addresses. Foreshadows AROS `cpu_Switch`/`cpu_Dispatch`.
**Observe:** `[M7] context switch ok`. **Files:** `boot/switch.S`, `boot/task.c`.

### M8 — Minimal shell ✅
UART RX (`uart_getc`, PL011 FR.RXFE) + a line shell in `kmain.c` (`ping`→`pong`,
`ticks`→count, `quit`→exit). The harness `INPUT=...` connects to the serial socket
and injects keystrokes (buffered in the RX FIFO); the shell echoes so replies land
in `serial.log`. The *drive* half of the loop, proven end-to-end.
**Observe:** injected `ping/ticks/quit` → echoed `pong`/`ticks=N`/`[M8] shell ok`.

### M9 — Framebuffer ✅
`boot/fb.c`: ramfb framebuffer via the fw_cfg DMA interface (scan `etc/ramfb` in
the file directory → config a 640×480 XRGB8888 buffer → draw 4 colored quadrants).
Protocol grounded from QEMU `qemu_fw_cfg.h` + `ramfb.c`. Harness adds `-device
ramfb`; `make shot` grabs a QMP screendump. **Visually verified**: red/green/blue/
white quadrants render correctly (the pixel "way of seeing" is live).
**Observe:** `make shot` → `run/screen.png`. **Files:** `boot/fb.c`.

### M10 — Preemptive multitasking ✅ (beyond the original plan)
The timer IRQ now *drives* scheduling. The exception path saves a **full** trap
frame (x0–x30 + ELR + SPSR) and the handler returns the frame to resume — the same
one, or another task's. `boot/sched.c` round-robins 3 contexts (main + A + B);
tasks A and B run infinite loops with **no cooperative yield**, yet both make
thousands of iterations of progress — proving the timer preempts them. Foreshadows
AROS `core_Schedule`/`cpu_Switch` driven from the system timer.
**Observe:** `[M10] preemptive multitasking ok: A ran=N B ran=M (no yields)`.
**Files:** `boot/sched.c` (+ full-context `boot/vectors.S`/`boot/exc.c`).

---

After M9–M10 the bring-up primitives exist on QEMU. **Phase 2** swaps the platform
layer from bare-metal-QEMU to hosted-on-macOS (a Mach-O bootstrap + the host-call
ABI shim — the boundary that historically killed the Darwin-PPC port), letting
macOS own every driver. That's when the MacBook Air becomes the payoff.
