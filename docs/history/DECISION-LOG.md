# AROS AArch64 — Architecture and Decision Log

This is the chronological record of the work that led to Macaros. It preserves
design rationale and debugging evidence; current instructions live in the
[feature index](../features/README.md) and root
[getting-started guide](../../GETTING-STARTED.md).

This is the project from the long conversation: give AROS a 64-bit ARM (AArch64)
backend, brought up on QEMU `virt` first, with my Apple Silicon MacBook Air as
the eventual hosted target. The non-negotiable constraint is that an AI agent
must be able to run the *whole* loop unattended — build, boot, observe, judge —
with no manual step in the middle. If a step needs a human, it doesn't scale.

## Finding: `x18` must be reserved on the hosted-Darwin aarch64 target (2026-06-30)

`x18` is the AAPCS64 **platform register, reserved on Darwin**. On Apple Silicon
the macOS kernel owns it and **zeroes `x18` in the signal context** it hands a
handler. Proven with a 30-line host probe
([`hosted/x18probe/`](../../hosted/x18probe/README.md)): put a sentinel in `x18`,
let a timer signal fire while the program holds it, read `x18` back from the signal
frame → `0x0000000000000000`, every run. AROS-hosted preempts tasks via signals,
so any value left live in `x18` is gone the moment a preemption fires.

The AROS side is **not** at fault: the darwin `SAVEREGS`/`RESTOREREGS` macros
(`arch/all-darwin/kernel/cpu_aarch64.h`) already copy the full `x0..x28` block,
`x18` included — they just faithfully copy the zero macOS already wrote. Because
the switch path runs the register set through that frame, even a same-task
preemption writes the zero back into `x18`. So the hosted "save and restore" that
works on Linux/bare-metal (where `x18` is an ordinary, honestly-preserved
register) **cannot** work on macOS: the value is destroyed at the kernel boundary,
before any AROS code runs.

This surfaced as the **ffmpeg h264 crash**: clang put the h264 GetBitContext
buffer pointer in `x18`; a preemption mid-decode zeroed it → SIGSEGV reading
`[x18 + idx]`. Found with the fault-address trap dump (`fault addr=0x268`,
`ldr w2,[x18,x2]`, `x18=0`). Fixed with `-ffixed-x18` in `aros-cc.sh`.

**Conclusion.** The fix is to **reserve `x18` across the aarch64-AROS ABI**
(toolchain + build), not to "fix the host" — macOS makes host preservation
impossible. Reserving it costs one register on Linux/bare-metal (harmless) and is
required on darwin, and it keeps a single portable aarch64-AROS binary set that
also runs on the macOS host. `-ffixed-x18` is now in `config/make.cfg.in` (OS
build) and `aros-cc.sh` (ffmpeg); the crosstools default and any objects built
before the flag still need a rebuild to be fully covered.

**Rebuild done for the Rust-runtime path (2026-07-01).** The flag was committed but
the deployed modules predated it (`posixc` alone had 108 x18 uses), so `time`/threads
SIGBUS'd from Rust. Force-recompiling `posixc`/`stdc`/`exec`/`kernel`/`timer`/`dos`/
`emul-handler` with the flag dropped each to ~2 x18 uses (compiler-rt soft-float
residual, not in hot integer paths). After that, Rust `std::time` and `std::thread`
both work, verified live. The rest of the desktop (Wanderer, C: commands, ffmpeg
datatypes) still has x18-dirty objects and can be recompiled the same way when its
latent faults matter — mixing x18/non-x18 modules is safe (no ABI change). To force a
module's recompile: delete its `gen/**/*.o` (make.cfg alone doesn't invalidate them),
then build its metatarget.

> Raised with kalamatee, who noted the hosted path saves/restores
> `x18` and that making it usable is the host's job, not the binary's. Both true;
> the probe shows macOS removes that option on Apple Silicon. Re-run `x18probe.c`
> to re-check on a future macOS.

## Architecture at a glance

- **Two independent hard problems, never worked at once.** (1) The AArch64 CPU
  backend (vectors, MMU, context switch, exception model) — greenfield, the real
  contribution. (2) Apple Silicon specifics — deferred. Phase 1 does #1 entirely
  on QEMU `virt`, where the machine is observable; Apple hardware is undocumented
  and gives the agent nothing to see, so it comes last.
- **The harness is the product right now.** `make run` → builds an AArch64 ELF →
  boots it headless on QEMU `virt` → observes → prints one uniform verdict block.
  Everything else hangs off this loop.
- **"Seeing" is generalized past screenshots.** The agent observes through
  whichever channel is faithful for the milestone — serial markers, QEMU's fault
  trace, lldb CPU-state, or (from M9) a framebuffer screendump — and always gets
  the same PASS/FAIL block back. It never needs to know which channel mattered.
- **Layout:** the standalone bring-up now lives in
  [the standalone QEMU kernel](../hosted/initial-platform-bringup/qemu-virt/boot/),
  with its [Phase 1 notes](../hosted/initial-platform-bringup/qemu-virt/PHASE1.md);
  `harness/` contains the loop and observation channels, and `Makefile` provides
  the hosted and integration entry points.

## Key decisions

### Bring-up toolchain: LLVM, not GNU
clang cross-compiles to `aarch64-none-elf` out of the box and lldb is the native
macOS debugger (no gdb codesigning dance, which would have put a manual step in
the loop). The eventual *real* AROS port will use AROS's own GCC crosstools
regardless — so this choice only governs the bring-up phase, and convenience won.
Packages: `qemu`, `llvm` (clang + llvm-objcopy + lldb), and `lld` (separate brew
formula — `llvm` does not ship the linker).

### QEMU `virt` is the Phase-1 target, on purpose
It's the only target that exposes the machine programmatically — PL011 serial to
a logfile-backed socket, QMP for screendumps, a gdbstub for lldb. That's exactly
what makes the unattended loop possible and exactly what a real MacBook denies us.
So the "no manual steps" rule is itself an argument for doing the CPU work on
QEMU first.

### Clean exit via semihosting
A finished program calls semihosting `SYS_EXIT`, so a PASS exits in <1s instead
of waiting out the timeout. A hung boot is reaped by a watchdog. Without both,
the "unattended" claim is false — the loop would block forever on a bad boot.

### Marker discipline
Each milestone prints a unique serial marker (`[M1] …`, `[M2] …`). On an OS
bring-up there's no stack trace — markers are the agent's ground-truth "did it
get further than last time," and one-per-milestone lets it localize a regression.

### Grounded: the state of AArch64 in AROS upstream (and what we mirror)
Checked the AROS tree (`../aros-upstream`, shallow clone) before
writing native code. Findings: `arch/aarch64-all` is **header-only scaffolding**
(8 files / 452 lines) configured as a *hosted-Linux* flavour
(`aros_flavour="emulation"`, `aarch64-linux-gnueabihf-`) that was never finished —
real ARMv8 atomics (`atomic_v8.h`), an incomplete/buggy `struct ExceptionContext`
(`r[29]; fp; sp; pc`, mislabels x30, no ELR/SPSR), and an empty `asm/mmu.h`. There
is **no native AArch64 kernel** — no boot, vectors, MMU, context switch, timer, or
`kernel.resource`. So "AROS's first *native* AArch64 backend" is accurate; we are
not duplicating finished work. At the graft we *reuse and fix* the existing
`aarch64-all` headers rather than inventing parallel ones.

What we mirror from the real `arm-native` port (20k lines) so Phase-1 code grafts
cleanly later: boot `start`→`kernel_cstart`; vectors `intvecs.s` + `intr.c`
(`__vectorhand_*`) installed by `core_SetupIntr()`; trap frame `struct
ExceptionContext`; MMU `mmu.c`/`core_SetupMMU`; switch `cpu_Switch`/`cpu_Dispatch`;
naming `cpu_*` / `core_*` / `Krn*`; split `rom/kernel` ↔ `arch/<cpu>-all` ↔
`arch/<cpu>-native`. We adopt the *names and shapes* now, not the whole machinery.

### Ground against the real machine, not priors
Every hardware fact is verified against an authoritative source before code is
written against it — the DTB the actual QEMU binary emits (`make dtb`), the QEMU
board docs, the arm64 boot protocol - and recorded in the
[QEMU hardware map](../hosted/initial-platform-bringup/qemu-virt/HARDWARE.md) with its
citation. This already paid for itself at M2: (1) the docs say the default GIC is
v3, but our exact invocation emits **v2** — the machine wins; (2) the boot
protocol says x0 = DTB, but QEMU's ELF `-kernel` path actually enters with
**x0 = 0**, which would have made M6's "read RAM from the DTB" silently dereference
null. Assumptions get *proven in the loop* (kmain prints the value) rather than
assumed. This is the project's standing rule: ground it, don't dream it.

### H3 grounding: Apple's arm64 ABI divergences (the host-call boundary)
The hosted port's scariest layer is the host-call shim — AROS code (built to
generic AAPCS64 by its own crosstools) calling macOS libc, which follows Apple's
arm64 ABI. I grounded the divergences not against Apple's HTML doc (JS-rendered,
unfetchable as text) but against the **stronger** source: the exact instructions
this machine's own `clang` emits (`-O0 -S` on snprintf/9-arg calls — see the H3
commit). The four divergences from AAPCS64, all confirmed empirically:

1. **Variadic args go on the STACK, one 8-byte slot each** — even when arg
   registers are free. `snprintf("%d %d %d",11,22,33)` `str`s the three ints to
   `[sp],[sp+8],[sp+16]` while `x5..x7` sit unused. *This is the printf killer:*
   a hosted AROS cannot `bl _printf` with varargs in registers.
2. **Variadic FP rides the same integer stack slots** — a `double` is `str d0,
   [sp+8]`, no separate FP save area. So one integer marshalling path carries
   `%d`, `%p` and `%f` alike (pass the double's bit pattern).
3. **Non-variadic stack args pack to natural size** (an `int` spilled past x7 is
   4 bytes at `[sp]`, the next at `[sp+4]`; `char`@0/`short`@2/`char`@4) — not
   the 8-byte slots AAPCS64 uses. Bites only at >8 args; documented, not shimmed.
4. **The caller sign/zero-extends sub-word args to 32 bits** (`ldursb` before the
   call for a `signed char`).

The shim (`hosted/abishim.S`) is the hand-written marshaller for #1/#2 — exactly
the code AROS's host-printf bridge must contain. Proven in the loop (`make
hosted-abi` → `[H3]`): the correct path prints `11 22 33` / `7 3.5 Z` / `<AROS>`;
a deliberately-naive register-passing control prints `0 0 0`, so the divergence is
shown to be real *and* bridged. This is the boundary that killed Darwin-PPC; it's
now de-risked the same cheap way as H1/H2 — a spike, grounded, verified live.

### H4: the AROS exec scheduler, hosted (grounded against the real tree)
After H2 proved hosted preemption, I refused to leave it an ad-hoc round-robin.
Read the actual AROS scheduler — `arch/arm-native/kernel/kernel_scheduler.c`
(`core_Schedule`/`core_Switch`/`core_Dispatch`), `kernel_cpu.c`
(`cpu_Switch`/`cpu_Dispatch` + `STORE_/RESTORE_TASKSTATE`), the exec stubs in
`rom/exec/{dispatch,schedule,reschedule,switch}.c`, and `struct Task` +
`TS_*` in `include/exec/tasks.h` — then rebuilt the hosted scheduler to those
exact names, signatures and contracts (`hosted/exec.c`). The macOS main thread is
modelled as a low-priority "boot" anchor task; the SIGALRM handler is
`core_ExitInterrupt`. Stacks come from `mmap` with real `tc_SPLower/SPUpper`
bounds (the stack-probe check in `core_Switch` is live, not decoration). Proven in
the loop (`make hosted-exec` → `[H4]`): two pri-1 tasks round-robin within ~2%,
two pri-0 tasks starve — i.e. AROS's strict-priority + equal-priority-FIFO
semantics, reproduced exactly. This is the scheduler *spine* both the native and
hosted ports share, now exercised on the MacBook before the graft.

**Grounding caught a real upstream bug to fix at the graft:** AROS's
`arch/aarch64-all/include/aros/cpucontext.h` defines `struct ExceptionContext` as
`{ IPTR r[29]; IPTR fp; IPTR sp; IPTR pc; }` — it mislabels x30 as `fp`, omits
SPSR_EL1 entirely, and has no FP/NEON pointer. Our Phase-1 trap frame
(`docs/hosted/initial-platform-bringup/qemu-virt/boot/kern.h`:
`x[31]` + `elr` + `spsr`) is the correct shape. When we graft, we
*fix* that header rather than inherit it — a concrete, grounded AROS contribution.

### H5: the AROS exec memory model, hosted (grounded against the real tree)
Same discipline as H4, applied to memory. Read the actual allocator —
`rom/exec/{allocate,deallocate}.c` and the `stdAlloc`/`stdDealloc` core in
`rom/exec/memory.c`, plus `struct MemHeader`/`MemChunk` in
`include/exec/memory.h` — and reproduced it faithfully in `hosted/mem.c`:
first-fit walk of a single-linked free list, split-and-return-first on a partial
fit, address-ordered coalescing insert on free (merge prev if `p1+bytes==p3`,
merge next if `p4==p2`), `MEMCHUNK_TOTAL`=16 rounding, `MEMF_CLEAR`/`MEMF_REVERSE`,
`FreeTwice` overlap detection. The region is one `mmap` — macOS owns the pages,
exec owns the policy (the hosted memory story). Verified in the loop with a
stress battery and hard invariants (free-list ordered/non-overlapping/in-bounds,
`sum(chunks)==mh_Free`, full coalesce back to a single chunk after free-all).
Deliberately dropped from the spike: the managed-mem (`MemHeaderExt`) path and the
`mhac` index cache — a lookup accelerator, not a correctness feature. With H4
(scheduler) this gives the two load-bearing `exec` subsystems, hosted and proven,
before the graft.

### H6: composition (H4+H5) caught a real bug isolated spikes couldn't
Tied the scheduler (H4) and allocator (H5) into one process (`hosted/kern.c`):
tasks are `AllocMem`'d from the heap and scheduled preemptively, with
`Forbid()`/`Permit()` (AROS dispatch-disable) guarding the allocator. The first
cut *failed* — the free list ended 512 bytes inconsistent (`free_sum < mh_Free`).
Root cause, and the lesson: **`forbid_cnt` is `volatile` but `struct MemHeader`
is not, and C only orders volatile-to-volatile accesses.** At `-O2` the compiler
hoisted/sank the non-volatile free-list writes *outside* the Forbid window, where
a SIGALRM caught the list half-updated. The fix is a compiler barrier
(`asm volatile("" ::: "memory")`) in Forbid/Permit, tying the allocator's memory
ops to the window; a single underlying thread means no CPU fence is needed for
same-core signal delivery. This is precisely the class of bug a hosted port must
get right, and it only appears under composition — neither the scheduler nor the
allocator spike could surface it alone. Also fixed: shutdown must respect Forbid
too (don't snapshot the system mid-critical-section). Verified stable across
repeated runs (`make hosted-kern` → `[H6]`).

**Size note (slow-ai):** `hosted/kern.c` is ~330 lines — over the 200-line flag,
but it's a deliberate integration milestone (two subsystems + the glue) kept in
one runnable file to match the harness's one-binary-per-marker design. Each
subsystem is clearly sectioned; at the real graft these become separate AROS
modules anyway.

### H7: the host display, observed unattended (the M9 trick, hosted)
Applied "macOS owns the drivers" to the display. AROS draws into a framebuffer
`AllocMem`'d from its own heap; the host presents it by encoding to PNG via
macOS ImageIO/CoreGraphics. The ImageIO call sequence was grounded against the
live toolchain first (a scratch `pngprobe` that I compiled, ran, and *read back*
as an image) before writing the driver — same ground-it-don't-dream rule. Crucial
for the project's constraint: the agent observes the screen through a PNG file it
can read, so "seeing pixels" stays unattended, exactly like M9's QMP screendump.
The scene is pixel-asserted (four-colour squares exact, title bar present, a white
'A'-crossbar pixel, bluish sky) so there's a real PASS/FAIL behind the image, not
just "a file exists". An actual on-screen Cocoa window is deferred on purpose: a
live window can only be verified unattended via `screencapture`, which needs macOS
Screen-Recording (TCC) permission — a manual approval that violates the no-manual-
steps rule. Render-to-PNG sidesteps that entirely; the window is a later
human-facing nicety, not a loop dependency.

### H8: the library/LVO mechanism — and a host-risk that ISN'T one
Before spiking the library system I asked the host-specific question: does AROS
build its library jump vectors as *runtime-generated code*? If so, doing that
hosted hits Apple Silicon's W^X / MAP_JIT wall (you can't just write executable
memory). Grounded the answer in the AROS tree: 32-bit ARM and m68k DO use
executable `FULLJMP` vectors — but 64-bit native arches do not.
`arch/x86_64-all/include/aros/cpu.h` says it outright: *"On x86-64 we use vector
table consisting only of pointers. We do not include jump code in them."*
(`__AROS_USE_FULLJMP` is off; `FullJumpVec` is only for `LoadSeg` headers.)
AArch64 follows the same 64-bit convention. **So the library system is plain
function pointers + indirect calls — no codegen, no W^X wall.** H8
(`hosted/library.c`) proves it live: `MakeLibrary` builds the `JumpVec` table
below the base, calls dispatch through `__AROS_GETVECADDR`, and `SetFunction`
hot-patches a vector (contract from `rom/exec/setfunction.c`: negative byte
offset `-LVO*LIB_VECTSIZE`, `Forbid()`, return old vector). This is the grounding
discipline catching a *non*-problem before I built a workaround for it — as
valuable as catching a real one.

### H9: Wait()/Signal() — making the scheduler a real exec
H4/H6 round-robin tasks but they never block; a real exec is built on tasks that
*wait* (for a message, a device, a semaphore) and get *signalled*. H9 adds the
genuine AROS state machine (`hosted/signal.c`), grounded against
`rom/exec/{wait,signal}.c`: `Wait` sets `TS_WAIT` + `tc_SigWait`, enqueues on
`TaskWait`, and yields; `Signal` ORs into `tc_SigRecvd` and, if the target waited
on those bits, moves it back to `TaskReady`. The hosted realisation of "yield":
once a task is `TS_WAIT`, `core_Switch` won't re-queue it (it only re-queues
`TS_RUN`), so the next timer tick parks it off the ready list — that *is* blocking.
A volatile re-read of `tc_State` in the wait loop defeats the compiler caching the
state in a register (same family of bug as the H6 barrier). Verified with a
lock-step producer↔consumer ping-pong (100=100, each blocking 100×) and a
free-runner that does ~1000× the work — proving the pair really yield. No lost
wakeups: `Wait` checks `tc_SigRecvd` before blocking, so a `Signal` that races
ahead of the `Wait` is still seen.

### H10: message ports — the exec IPC layer (and device-I/O shape)
Layered the real AROS port mechanism on H9's Wait/Signal (`hosted/msgport.c`),
grounded against `rom/exec/{putmsg,getmsg,waitport}.c`. `PutMsg` = `AddTail` then
`Signal(mp_SigTask, 1<<mp_SigBit)`; `WaitPort` blocks on the port's signal bit
until the queue is non-empty; `GetMsg` = `RemHead`; `ReplyMsg` = `PutMsg` back to
`mn_ReplyPort`. The point: this is the exact shape of AROS device I/O (send an
IORequest to a port, block for the reply), so proving it hosted proves the I/O
path a hosted AROS uses to reach host resources. Verified with a client↔server
request/reply loop (server squares each request): ~124 round-trips, server
processed exactly as many (no message loss), zero wrong replies, both tasks
blocking on their ports under preemption. With H4/H5/H6/H8/H9 this rounds out the
essential exec: scheduling, memory, libraries, signals, messages.

### H11: a device backed by a real macOS file — the thesis, end to end
This is the Phase-2 mission statement made concrete: *macOS owns the drivers,
AROS reaches them via standard exec I/O.* `hosted/device.c` runs the real exec
I/O path — a client builds an `IOStdReq`, `DoIO()` does `BeginIO` (`PutMsg` to the
device's port) then `WaitIO` (block for the reply), and a device task performs the
actual `pread`/`pwrite` on a real file — grounded against `exec/io.h` and
`rom/exec/doio.c`. It composes the whole stack: scheduler (H6) + Wait/Signal (H9)
+ message ports (H10), and the host syscalls run on a switched task stack under
preemption (the H1 property, now at the device layer). Verified two independent
ways: the client checks each read == its write, and main then re-reads the file
*through the host* to confirm the bytes physically landed (128 bytes all equal to
the last round's pattern). 62 round-trips, zero errors. The `(struct Message *)io
== &io->io_Message` identity (io_Message first in IOStdReq) is what lets a replied
message be cast straight back to its IORequest — faithful to AROS.

### H12: exec.library boot — the capstone that ties it all together
The 11 spikes each proved one mechanism; H12 (`hosted/execboot.c`) assembles them
into a single coherent miniature exec, the AROS way: `exec.library` is the hub,
and its services (`AllocMem`/`FreeMem`, `Signal`/`Wait`, `AddTask` + the scheduler)
live in the negative-offset LVO jump-vector table below `SysBase`, reached by the
tasks through the library base — exactly the `__AROS_GETVECADDR(SysBase, lvo)`
dispatch that defines AROS. The faithful detail that matters: this is not "call the
functions directly", it's "call them through the vector table", which I *prove* by
`SetFunction`-instrumenting `LVO_Signal` and showing its counter (~398) tracks the
ping-pong handshakes. Two tasks AddTask'd via LVO run a lock-step Signal/Wait
exchange, allocating buffers via `AllocMem` LVO, scheduled preemptively. It is the
"a tiny AROS exec actually runs on the MacBook, every call going through the real
exec hub" demonstration — the satisfying close of the spike phase.

## Trade-offs made under time pressure

- `start.S` parks secondary CPUs in a `wfe` spin rather than implementing PSCI
  CPU bring-up. Fine until we need SMP; revisit around the scheduler milestones.
- Fault detection in the harness is a grep over QEMU's `-d int` trace, with the
  benign semihosting pseudo-exception filtered out. Good enough; a determined
  bad path could still slip a fault past the regex. Tighten if it ever lies.
- The portable timeout is a bash watchdog because stock macOS has no GNU
  `timeout`. Works, but it's a TERM-then-KILL hack, not airtight.

## With more time, I would

- Add a `make test` that runs the whole milestone suite and reports a matrix, so
  a regression in M3 is caught while working on M7.
- Wire the framebuffer/screendump path (M9) earlier as dead code, so the "pixel"
  way of seeing is exercised before Wanderer actually needs it.

## Phase 1 retrospective (stepping back)

**What we built:** a complete *native* AArch64 bring-up on QEMU virt — boot → C
runtime → exception vectors → MMU → timer IRQ → page allocator → cooperative
scheduler → shell → framebuffer. ~840 lines across 12 files, each milestone
grounded against an authoritative source and verified live in the loop.

**What's genuinely solid:** the autonomous loop (build→boot→observe/drive→verdict,
all channels: serial, fault trace, lldb, framebuffer, plus injected input + a
regression matrix); the grounding discipline, which caught three real errors I'd
otherwise have shipped (x0≠DTB, GICv2-not-v3, AArch32-vs-A64 semihosting); and a
clean file-per-concern layout that mirrors AROS's `arch/<cpu>-native`.

**Known simplifications (honest debt, not bugs):**
- Single core — secondaries parked, no PSCI `CPU_ON`/SMP.
- Identity map only — no virtual address spaces, VA==PA, one L1 table.
- ~~Cooperative scheduler~~ → **M10 made it preemptive**: the timer IRQ saves a
  full trap frame and round-robins tasks (no yields). Still single-core, no task
  priorities/blocking, no per-task address space — a real scheduler is more.
- `pmm` is a flat free-list; no contiguous alloc; RAM size hardcoded to `-m 512`
  (no DTB parse yet).
- Framebuffer in `.bss`, no cache maintenance (fine under QEMU's no-cache model;
  real hardware would need a clean-to-PoC).
- Everything at EL1 — no EL0/userspace, no FP/NEON.

**Strategic insight worth being honest about:** Phase 1 is the *native* CPU
backend. For the original "AROS on my MacBook" **hosted** dream, the directly
reusable pieces are the toolchain, the AArch64 ABI/calling-convention knowledge,
and the context switch (`switch.S`) — *not* the bare-metal drivers (MMU, GIC,
vectors, ramfb), because macOS provides those. So the native track (what we have)
and the hosted track (the MacBook payoff) share a spine — the AArch64 `exec` /
`kernel.resource` logic — but diverge on the driver layer. Both still require the
**graft**: turning these primitives into actual AROS `cpu_Switch`/`core_*`/`Krn*`
functions wired into `rom/kernel` + `exec.library`. That graft is the real Phase-2
mountain, native or hosted.

## Phase 2 retrospective (stepping back)

**What we built:** eight standalone spikes (`hosted/`, `make hosted-test` all
green) that de-risk the entire *host-facing* surface of a hosted AROS on Apple
Silicon, each grounded against an authoritative source and verified live:
foundation + preemption (H1/H2), the host-call ABI boundary (H3), the real `exec`
scheduler and memory models and their composition (H4/H5/H6), the host display
(H7), and the library/LVO mechanism (H8). The scariest historical risk — the
cross-ABI host call that killed Darwin-PPC — is retired.

**What's genuinely solid:** the hosted loop is as unattended as Phase 1 (run the
Mach-O binary, read stdout or a PNG, get one verdict), and the grounding
discipline kept paying:
- it *retired* real risks (Apple's variadic-args-on-stack ABI — H3);
- it *caught a real composition bug* nothing else could (the `Forbid` compiler
  barrier — `volatile` orders only volatile-to-volatile, so `-O2` sank allocator
  writes outside the critical section — H6);
- it *cleared a non-risk before I built a workaround* (64-bit AROS library vectors
  are data pointers, so no Apple-Silicon W^X / MAP_JIT wall — H8).

**Honest scope (what these spikes are and aren't):** they are faithful
reproductions of AROS's *shapes and contracts*, hand-written in standalone files
(one binary per marker, to fit the harness). They are NOT the AROS tree compiled
and running. Deliberate simplifications, noted not faked: single underlying thread
(so `Forbid` needs only a compiler barrier, no CPU fence); allocator drops the
managed-mem path + index cache; scheduler omits signals/`Wait`, SMP, ETask CPU
accounting; the display is render-to-PNG, not an on-screen window; LVO numbers are
illustrative. Each note says what was dropped and why.

**The mountain, named honestly:** every *hosted* unknown is now answered, so the
remaining work is no longer spike-able — it's the **graft**: build AROS's own
crosstools for `aarch64-darwin`, drive its `configure`/`mmake` to emit a hosted
binary, fix the unfinished `arch/aarch64-all` (e.g. the wrong `ExceptionContext`),
and bootstrap the real `exec.library` on these primitives. That's large-scale
integration in the AROS tree, not a session-sized spike — and it's the next thing
to ground rather than reimplement.

## 2026-06-24 — Hosted AROS BOOTS on Apple Silicon

The graft is no longer "the next thing to ground": **real AROS now loads, relocates
and runs on this Mac.** `exec.library` (175KB) + `kernel.resource` (70KB) +
`hostlib.resource` come up with valid `SysBase`/`KernelBase`; a SIGSEGV is caught via
the `cpu_aarch64.h` host-signal→AROS-trap bridge and AROS prints its AArch64 register
dump + native Guru alert. It halts at a cold-start trap (no `dos.library` in a
3-module kickstart). Run it: `~/aros-darwin/run.sh`. Full map: `graft/WORKFLOW.md`;
upstream friction (25 items): `graft/UPSTREAM-NOTES.md`.

### Key decisions / trade-offs under the "make it boot" push

- **`-mcmodel=large` for the kickstart modules, instead of a GOT in the loader.**
  AArch64 clang routes *weak* symbols (`__aros_libreq_SysBase`) through the GOT even
  with `-fno-pic`, and the simple bootstrap loader has none. Large model emits
  absolute `movz/movk` (only `MOVW_UABS`/`ABS64` relocs). Trade-off: bigger modules
  + a full recompile, for a much simpler, self-contained loader (no near-code GOT).
- **Map the AROS RAM pool `RW`, not `RWX`** (and drop `MAP_32BIT`) on Apple Silicon.
  W^X refuses a one-shot RWX anon `mmap` even with JIT entitlements; early boot runs
  from the kickstart RO region anyway. Consciously deferred: executing code *loaded
  into* the pool (LoadSeg) needs the W^X-aware path later.
- **Force-load a hand-built static C runtime (`libkrnmem.a`).** The proper fix is for
  the freestanding modules to link `stdc.static` so its *strong* `memset`/`strcmp`/…
  win over the weak StdC stubs; archive semantics wouldn't pull them, so I carved the
  mem/str/format dependency-closure into `libkrnmem.a` and `--whole-archive`'d it.
  Trade-off: assembled by hand in the build dir — needs a real mmake rule.
- **A deliberately minimal 3-module kickstart** to *get a boot* and see AROS's own
  output, accepting the cold-start halt. The rest of the OS is the next body of work.

### With more time, I would (now)

- Give `libkrnmem.a` a real mmake rule (or make the kernel/exec/hostlib links pull
  `stdc.static` strong-over-weak cleanly) and find the proper `configure` home for
  `-mcmodel=large` / `-D__arm64__`, so a clean checkout reproduces the boot.
- Implement `arch/aarch64-all/stdc/setjmp.s` (still a weak stub).
- Walk the cold-start: add `dos.library` + the boot module set, chase the next traps.

## 2026-06-25 — [J5b] control flow in the 68k JIT (a real loop, real condition codes)

Scoped increment of the `[J5]` decoder mountain. Translated a self-contained 68k loop
`moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ; subq.l #1,d1 ; bne.s L ; rts` (→ d0=15,
5 iterations) in ONE `jit_region` with an **internal backward branch**, real condition
codes, verified byte-exact against an independent from-scratch interpreter.

### Key decisions / trade-offs

- **The branch reads real NZCV straight from the `subs`.** `subq.l #1,d1` is emitted as
  `subs w_d1,w_d1,#1` (the flag-setting subtract, `A64.h:320`); the conditional
  `bne.s` is an AArch64 `b.ne` (`A64_CC_NE`, Z=0) that consumes those flags directly —
  the AArch64 NZCV *is* the live 68k branch condition. No CCR=0 shortcut.
- **68k CCR recomputed with NON-flag-setting ops to preserve `subs`→`b.ne` adjacency.**
  Right after the `subs`, before the branch, the block derives the 68k CCR bits
  (N from result bit31, Z from result==0, C/X from the subtract borrow rule, V from
  signed overflow) using `cset`/`orr`/`str` etc. — none of which touch NZCV — so the
  branch still sees the `subs` flags. The asserted `state->ccr` is therefore the real,
  full N/Z/V/C/X, matching the interpreter's subtract flag rule. (68k subtract C = NOT
  AArch64 C, since ARM sets C on no-borrow; handled explicitly.)
- **Single region, internal backward branch.** The whole loop is emitted once; the
  loop-top is a label (a word index) inside the emitted code and the `b.ne` offset is
  `loop_top_idx − bne_idx` (negative). `b_cc`/`b` take signed word offsets and
  `ASSERT_OFFSET` is a no-op at `-O2`, so two's-complement backward branches encode
  cleanly. No cross-region chaining (deferred to `[J5c]`).
- **Watchdog turns an infinite loop into FAIL.** 10s SIGALRM. The negative control
  breaks the Z computation (emit `b.al` / wrong condition) to force divergence or a
  caught hang, proving the iteration/termination asserts bite.
- **No new Emu68 file** — only existing `A64.h` encoders (`add_reg`, `subs_immed`,
  `b_cc`, `b`, `cset`, ldr/str, `mov_immed_u16`). Exhibit-B grep unchanged.

### With more time (→ [J5c])

Cross-region block chaining + an instruction cache, full `Bcc`/`DBcc` condition
coverage, forward branches across blocks, real `jsr`-through-vector decode from a
stream, library calls from the running program, and OUR register allocator (still
memory-backed here).

## 2026-06-25 — [J5n] PLAN: the DIAGNOSTICS subsystem (crash bundles) — gates, not yet code

A faults-are-never-silent subsystem for the 68k JIT: every fault produces one
self-contained, shareable **crash bundle** (`tar.gz`) with everything to reproduce
and diagnose. INTERNAL/host-side; the frozen seam (jit_region API, struct M68KState
layout, the [J3] LVO contract, the [J5i] exception model) is NOT touched.

### Gate 1 — reference summary (what the existing code establishes)

- **The fault funnel already half-exists.** `j5d_engine.c` routes every fault through
  `raise_exception(sb, st, vnum, frame_pc, &handler, errbuf, errlen)` (vectors 2/3/4/5/
  32+n) and through `RFAIL(msg)` (clean dispatcher errors). The dispatcher main loop
  (`j5d_run`, lines 821-1054) decodes each terminator and computes the next PC — this is
  the one place a global instruction counter, the diff-lockstep step, and replay-to-N
  all belong. `g_stats` already tracks per-block/insn counts.
- **The oracle is `j5d_interp_run`** (j5d_interp.c) — same signature as `j5d_run`, its own
  state + sandbox. It is the basis for the differential mode (run both in lockstep, trap on
  first divergence). It already supports the exception log via `j5d_interp_set_exc_log`.
- **The host-signal bridge shape is `graft/cpu_aarch64.h`** — `Xn(ctx,n)`, `FP/LR/SP/PC/
  CPSR(ctx)`, `GPSTATE`/`FPSTATE`, and `struct ExceptionContext`. On Darwin arm64 the
  mcontext exposes `__ss.__x[0..28], __fp, __lr, __sp, __pc, __cpsr` (non-opaque). I mirror
  exactly this to dump host x0–x30/sp/pc from a signal `ucontext`.
- **The loader skips `HUNK_SYMBOL`/`HUNK_DEBUG`** (j4_loader.c:223-249). The symbol hunk
  format is verified: `[BE32 name-len-in-longs][name padded to longs][BE32 value]`, zero
  length terminates; value is hunk-relative (add the hunk base, like a reloc). vasm WITHOUT
  `-nosym` emits a real SYMBOL hunk (confirmed: `mul.s` → symbol `loop` value 0x6).
- **Determinism holds:** single 68k execution, sandbox, deterministic stub OS, no host
  threads. So a global instruction number #N is reproducible and replay-to-N is sound.
- **Test/build convention:** one standalone `jXX_test.c` per spike, a Makefile target that
  darwinizes the emu68 decoders into `build/emu68-darwin/` then clang-links the OURS files +
  the test, run via `harness/run-hosted.sh '[J5n] PASS'` with a SIGALRM watchdog.

### Gate 2 — change-set (one line per file)

NEW (all OURS, AROS-licensed, no Emu68 source copied):
- `hosted/jit68k/j5n_diag.h` — the diagnostics API: `j5d_fault(kind,detail,ctx)` funnel,
  bundle writer, flight-recorder ring, symbol map, diff/replay config, host-signal net.
- `hosted/jit68k/j5n_diag.c` — implements the funnel, REPORT.txt (two-level), bundle dir +
  `tar -czf`, the LOUD banner, core.snapshot, program.exe+sha256, MANIFEST/REPRODUCE,
  flight recorder, 68k+host register dumps, both stacks (68k return-stack walk + native
  `backtrace()`), the SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers + `sigaltstack`.
- `hosted/jit68k/j5n_symbols.h/.c` — parse HUNK_SYMBOL (+ HUNK_DEBUG if usable) into a
  PC→symbol map; `j5n_sym_lookup(pc)` for the call-stack/report. Loader stays unchanged
  (I parse the file buffer independently, so j4_loader.c is not modified).
- `hosted/jit68k/j5n_test.c` — the `[J5n]` driver: trigger each fault kind, assert a bundle
  with REPORT.txt+core.snapshot+program.exe+MANIFEST.txt+REPRODUCE.txt + banner + REPORT
  contents; differential trap with diverge.txt; replay-to-N lands at #N; host-signal net;
  regression note; bundle cleanup.
- `apps68k/diagfault.s` (small) — a labeled program that deliberately faults (div0 / illegal
  / OOB) at a known instruction, assembled WITH symbols, to exercise symbol mapping +
  replay-to-N on a real hunk. Built by a tools step; `.exe` committed.

EDIT (engine — additive hooks only, behind a config struct; off the hot path):
- `hosted/jit68k/j5d_engine.c` — (a) a global instruction counter `++` per dispatched 68k
  instruction; (b) call the diag funnel at each `raise_exception`/`RFAIL` site (one wrapper
  macro, so all paths route through `j5d_fault`); (c) per-instruction hook for diff-lockstep
  + replay-to-N break, gated by a config pointer that is NULL unless a mode is enabled; (d)
  push (PC,opcode) into the flight-recorder ring. NO struct/seam change — `j5d_run`'s
  signature is unchanged; the diag config is set via a separate `j5d_set_diag()` like the
  existing `j5d_set_exc_log()`.
- `Makefile` — add `hosted-jit68k-j5n` target (darwinize + link OURS + j5n_*), and a tools
  step to assemble `diagfault.exe` with symbols.

### Gate 3 — data-flow paragraph

The engine runs deterministically; a file-scope `g_insn_number` increments once per
dispatched 68k instruction in `j5d_run`. A diag config (set by `j5d_set_diag`, NULL = the
fast path the whole corpus uses) carries: the loaded program bytes + sha + sandbox handle,
the symbol map, the flight-recorder ring, and optional diff/replay parameters. On ANY fault,
every `raise_exception`/`RFAIL` site (and the host-signal handlers) calls `j5d_fault(kind,
detail, host_ctx)`. The funnel snapshots M68KState + sandbox image, walks the 68k return
stack (A7 + saved return addresses → symbols), captures the native `backtrace()`, dumps host
registers from the signal `ucontext` (or "no host ctx" for a clean in-band fault), drains the
flight recorder, writes the bundle dir, `tar -czf`s it, prints the banner, and returns. In
diff mode, before each instruction the oracle steps one instruction over its mirror state and
the engine compares register/CCR/relevant memory; first divergence writes `diverge.txt` then
faults. In replay mode, the run breaks exactly when `g_insn_number == N`. The bundle's
`REPRODUCE.txt` records `run-to #N` + `git rev-parse HEAD` + build config, so another machine
re-runs the same program to the same fault.

### Gate 4 — trade-offs, uncertainties, mitigations

- **Lockstep granularity = per 68k INSTRUCTION, but the JIT executes per BLOCK.** The JIT
  doesn't expose mid-block state. Mitigation: run the oracle instruction-by-instruction and
  compare at BLOCK boundaries (the JIT's natural observation points — exactly where state is
  flushed to M68KState, per the seam). I locate the first diverging instruction by, on a
  block-level mismatch, re-running the oracle instruction-stepped through that block to name
  the exact insn. This is honest: I'll document "block-boundary detection, instruction-
  precise localization." For the injected-divergence test I can force a known mismatch.
- **`j5d_fault` from a signal handler must be async-signal-safe-ish.** Mitigation: use
  `sigaltstack`; do the heavy bundle writing with low-level `write`/`open` where practical,
  accept that this is a crash path (we are already dying) and a best-effort bundle is the
  goal; never re-enter the JIT. Document the async-signal caveat.
- **Symbol/line info:** vasm emits HUNK_SYMBOL (label→offset) reliably; HUNK_DEBUG line info
  is toolchain-dependent and may be absent. Plan: symbols-always, line-info-if-present,
  report honestly what was obtained. The corpus is stripped, so the symbol demo uses a
  purpose-built labeled `diagfault.exe`.
- **Real host-signal net (E in INTERFACE.md) is an AROS-integration row.** I install the
  handlers around the host-side test execution so a genuine out-of-sandbox HOST access is
  caught and bundled (the spec's explicit ask) — but the production SIGSEGV→`j5d_raise_
  exception` wiring inside AROS stays the owner's track; I do not fake it.
- **Seam unchanged:** I add a side-channel `j5d_set_diag()` (mirrors `j5d_set_exc_log()`),
  never touch struct M68KState/j5d_m68k_state layout, jit_region.h, the [J3] contract, or the
  [J5i] model. The engine edits are additive and NULL-gated.

### Bundle root README.txt (added at coordinator/user request)
A plain-language `README.txt` at the bundle ROOT for a non-expert who just hit the crash:
one line on what it is, the ONE action (send the whole .tar.gz), a one-line plain gloss of
each file, and a short "For the developer" section (diff pins the wrong instruction; the
coordinate+snapshot reproduce it via `run-to #N`; the two-level 68k+host dump says program-
bug vs JIT-bug). Complements MANIFEST.txt (the precise index); README.txt is the friendly
overview. The crash banner points at the archive AND says "open README.txt inside if unsure."

### Deferred (honest, stated up front)
- Snapshot-LOAD/inspect mode (load `core.snapshot` and dump): include if tractable, else
  documented as deferred (the snapshot is still WRITTEN + reload-described in MANIFEST).
- True per-instruction JIT lockstep (would need block-splitting); using block-boundary
  detection + oracle instruction-localization instead.

### [J5n] DONE — what shipped, and the decisions that landed during the build

Built + green: `make hosted-jit68k-j5n` prints `[J5n] PASS` (harness verdict PASS). The
binary actually printed the LOUD banner with the absolute bundle path, the differential trap
at the exact instruction, and the replay-to-N landing on #N. A sample bundle's
README.txt/REPORT.txt were cat'd and the program SHA-256 round-trips (bundle digest == the
committed diagfault.exe). The whole corpus + `[J1]`–`[J5m]` re-ran green; the frozen seam is
git-confirmed unchanged (no diff to `jit_region.h` / `j5d_jit68k.h` / the `j3_*` contract).

Decisions that landed:
- **The funnel routes through `j5d_fault`; the engine stays NULL-gated.** All fault sites
  (`raise_exception` failures, `RFAIL`, and a NEW `J5N_UNHANDLED` check) funnel only when a
  diag config is registered (`j5d_set_diag`, mirroring `j5d_set_exc_log`). With no diag the
  engine is byte-for-byte as before — that's why every existing marker stayed green.
- **`J5N_UNHANDLED` was the key insight for correct fault kinds.** The `[J5i]` model
  "succeeds" raising an exception even when the vector is 0 (uninstalled) — it dispatches to
  PC 0 and cascades to a bus error. I must NOT change `[J5i]`. So a diag-only check fires the
  funnel at the ORIGIN (div0/illegal/bus with the right kind) BEFORE the cascade masks it.
  No-op when diag is NULL, so `[J5i]` is untouched.
- **Optional dependency via weak symbols.** The other `[J5*]` tests link `j5d_engine.c` but
  NOT `j5n_diag.c`. I gave the engine WEAK stub definitions of the four diag hooks it calls;
  `j5n_diag.c`'s strong defs win when linked, the weak no-ops satisfy the linker otherwise.
  This is what keeps the diagnostics genuinely optional + the existing targets unchanged.
- **The differential is block-boundary + oracle-localized, as planned.** `j5d_run_diff` runs
  the JIT block-by-block and advances the instruction-precise interp oracle to each boundary
  (a `stop-at-PC` side-channel I added to the interp), comparing there. The injected
  divergence (JIT bridge returns a different d0 than the oracle bridge — a separate
  `ref_lvo`) traps deterministically at 0x21000A.
- **#N alignment was the subtle bug.** The engine counts per-block; the oracle per-
  instruction. I made the engine count `body_insns` + 1-per-terminator so its #N equals the
  oracle's instruction index, so the crash #N and `JIT68K_RUNTO=N` land on the same PC.
- **Chaining gated off under diag** (it's a hot-path optimization; diagnostics are off the
  hot path) so per-block accounting + fault localization are exact. The chained corpus
  (`[J5k]`/`[J5j]`) registers no diag, so it keeps full chaining — re-confirmed green.

### Things to discuss in the [J5n] walkthrough
- The weak-symbol trick for the optional diag dependency — clean, or would a separate
  `j5d_engine` compile flag be clearer?
- Block-boundary vs true per-instruction lockstep: the honest trade, and when block-splitting
  would be worth it.
- The signal-handler bundle write is best-effort (we're already dying); the one real guard I
  added was clamping the 68k stack walk to the sandbox so a corrupt a7 can't fault the handler.

## bsdsocket.library (host networking) — implementation log

Building the `bsdsocket-net` feature (real TCP/IP by forwarding AROS's
`bsdsocket.library` to the Mac's BSD sockets). Design/spec:
`docs/features/bsdsocket-net/`. Working the chunks host-first (fast loop), AROS
graft last (ephemeral cross-build).

### Key decisions
- **Host pump packaged as a dylib, peer of cocoametal/pasteboard.**
  `build/libbsdsockhost.dylib` (`make bsdsock-dylib`) exports exactly
  `hosted/bsdsocket/bsdsock.exports`; the AROS side reaches it via
  `hostlib.resource` (`HostLib_Open`+`HostLib_GetPointer`), the proven pattern.
  ABI verified by `make bsdsock-abi` ([NABI]) — dlopen + resolve all 19 symbols.
- **The readiness→wake seam is `ps_create_cb`.** The standalone proof wakes a
  self-pipe; the graft installs `Signal(task, readySig)` via `ps_create_cb(wake,
  cookie)`. The pump code is unchanged — only the callback differs (the single
  swap point the spec promised). Callback runs on the pump (host) thread →
  host-wake discipline is the AROS side's job.
- **Module location: `arch/all-unix/bsdsocket/`, not `all-darwin/`.** Confirmed
  tree-wide there is no unix host-socket bsdsocket (only Windows mingw32); the
  host-neutral core benefits every hosted AROS, with kqueue isolated in one file
  (`readiness_*`). Linux `epoll` backend is a bounded follow-on, not built here.
- **errno table built explicitly (`errno_xlate.c`), and it mattered.** The spec's
  "don't assume identity" caught a real one: macOS `EOPNOTSUPP==102` vs AmiTCP
  `EOPNOTSUPP==45` — a genuine non-identity map. Rest of the BSD socket range is
  identity but asserted entry-by-entry. `make bsdsock-errno` ([NERR]) = 25/25.

### Grounding finding — host-thread Signal is unsafe on darwin (reshapes the park)
Before writing the AROS-side park I checked the proven darwin drivers, and the
spec's core assumption (the kqueue pump thread raises `Signal(task, readySig)`) is
**wrong for this port**. `cocoa_input.c:546` is explicit: a task woken from host
interrupt/thread context runs in "supervisor mode" under the threaded scheduler and
**trips every semaphore op**; both proven drivers (input ~50 Hz, the working
clipboard ~5 Hz) **poll `timer.device` (`Delay()`)** and never `Signal` from a host
thread. So the design changes: keep the kqueue pump (efficient in-kernel readiness +
`pump_drain` stash), but the AROS-side WaitSelect/recv park is a **`Delay()` poll of
`pump_drain`**, not `Wait` on a host-raised signal; the pump callback just sets an
atomic flag. Documented in spec §R-DARWIN-WAKE, design.md "The bridge", and
host-wake-pattern.md R-W2. This is "ground it, don't dream it" catching a
load-bearing error before code — the host dylib already built is unaffected (the
pump + drain are exactly what the poll needs).

### Status
- [N1]–[N3] (pump round-trip / WaitSelect-style readiness / non-blocking park):
  PASS (pre-existing spike, re-verified 9/9).
- [NABI] dlopen ABI + the `ps_create_cb` wake seam through the boundary: PASS.
- [N4]/[NERR] errno translation table: PASS 25/25.
- **[N5] library graft — BUILDS.** `arch/all-unix/bsdsocket/` (14 files: genmodule
  conf, per-task SocketBase, hostlib wiring under Disable(), the data-path LVOs with
  the timer-poll park, sockopt/misc, stubs) compiles + links end-to-end against the
  darwin-aarch64 crosstools → a 34KB `AROS/Libs/bsdsocket.library`. Committed on
  `aarch64-darwin-graft` in ../aros-upstream. Build-bugs found+fixed along the way:
  `HostLibBase` field-vs-`#define` clash (→ field `hostlib`), missing
  `<sys/errno.h>`/`TICKS_PER_SEC`, two `*/`-in-comment terminations, and the
  AmiTCP-vs-libc `fd_set` collision (forward-declared in the stubbed WaitSelect).
- **[N5a] library LOADS + INITIALISES live — PROVEN on booted AROS.** Deployed
  libbsdsockhost.dylib to ~/lib, booted via aros-ctl, typed `version
  bsdsocket.library full` at the shell → **`bsdsocket.library 3.0 (28-06-26)`**, no
  trap. That single line proves the whole high-risk integration: OpenLibrary loaded
  the genmodule module, `bsdsocket_Init` ran (hostlib → libSystem **and**
  libbsdsockhost both HostLib_Open'd + interfaces resolved), **pump_start (the
  kqueue pthread) came up under the threaded scheduler without crashing**, and
  BSDSocket_OpenLib made a per-task base. Proof image:
  docs/features/bsdsocket-net/library-load-proof.png.
- **[N5b] LIVE TCP ROUND-TRIP — PROVEN, both ends.** Built an AROS client
  `socktest` (C: command using the inline defines/bsdsocket.h LVO stubs) + a host
  localhost echo server. Booted AROS, ran `socktest` at the shell →
  **`[N5B] PASS: round-trip echoed 'PING42' (6 bytes)`**, and the host echo server
  logged `client connected / bounced 6 bytes / client closed`. So the real
  bsdsocket.library data path works live on Apple Silicon: `socket()` → `connect()`
  (non-blocking + the darwin **timer-poll park** + `SO_ERROR`) → `send` → `recv`
  echoed-equal → `CloseSocket`, errno-translated, per-task SocketBase, kqueue pump —
  no trap. The H11 two-sided check. Proof: docs/features/bsdsocket-net/roundtrip-proof.png;
  test programs stashed in hosted/bsdsocket/n5b/.
  - Build notes: the client uses `defines/bsdsocket.h` directly (clib protos pull
    sys/types.h, uninstalled in -quick) and links posixc (AmiTCP sys/socket.h pulls
    sys/types.h, which lives in the posixc include tree — so NOT -noposixc).
  - The headless emergency-CLI boot currently crashes in dos/LoadSeg (a pre-existing
    boot fragility, unrelated to bsdsocket); the windowed-boot CLI was used instead.
- **[WS] WaitSelect (LVO 21) + [N6] outbound internet — PROVEN LIVE.** Implemented
  the real timer-poll WaitSelect (register fds with the pump, Delay-poll pump_drain +
  the *sigmask bits, rebuild the fd_sets, rewrite *sigmask; AmiTCP fd_set/timeval
  mirrored locally to dodge the net_types collision). Ran `nettest` on booted AROS:
  - **`[WS] PASS: WaitSelect woke on read-ready, recv echoed the byte`**
  - **`[N6] PASS: AROS fetched over the internet from 1.1.1.1 -> 'HTTP/1.1 301 Moved Permanently'`**
  AROS made a real outbound TCP connection to 1.1.1.1:80 and did an HTTP/1.0 GET — on
  Apple Silicon, through this bsdsocket.library. Proof:
  docs/features/bsdsocket-net/waitselect-internet-proof.png.
- **The feature is functionally complete** (per the spec's [N1]–[N6] scope): host
  pump + errno + the AROS library; socket/bind/listen/accept/connect/send/recv/
  WaitSelect all live; localhost round-trip AND real-internet fetch proven.
- **[DNS] resolver — DONE & PROVEN LIVE.** Implemented `gethostbyname` (LVO 35) the
  darwin-safe way: host `getaddrinfo` on a detached pthread (libbsdsockhost.dylib
  `hs_resolve_start/poll/free`), the AROS LVO **timer-polls** the result (only the
  calling task parks), then builds a per-task `struct hostent`. `nettest` on booted
  AROS: **`[DNS] PASS: resolved one.one.one.one -> 1.1.1.1, fetched 'HTTP/1.1 301'`**.
  Proof: docs/features/bsdsocket-net/dns-proof.png.
- **FEATURE COMPLETE** — the bsdsocket.library does sockets + WaitSelect + DNS, all
  proven live on Apple Silicon (AROS genuinely on the internet). Remaining LVOs are
  secondary/rarely-used stubs outside the [N1]–[N6] scope (gethostbyaddr,
  get{net,serv,proto}by*, inet_*, Obtain/ReleaseSocket, Dup2Socket, SocketBaseTagList,
  send/recvmsg, GetSocketEvents) — fillable as needs arise.

### Trade-off / to discuss in the walkthrough
- `errno_xlate.{c,h}` is authored + host-tested in `hosted/bsdsocket/`; the AROS
  module needs the same file. Kept pure int→int (no errno.h dependency) so one
  copy compiles in both worlds — but the AROS module currently gets its own copy
  (upstreamable/self-contained) at the cost of a hand-sync. Worth a check rule.

## 2026-06-28 — [LED+THEME] PLAN: Amiga status-bar LEDs + a Dark/Light/System theme

Host-layer feature for the Macaros window: turn the footer into a proper **status
bar** with Amiga-style **Power + Activity LEDs**, and add a **theme switch
(Dark / Light / System)** so the whole app — title bar, menus, Settings, and the
footer — reads as one coherent appearance. Worked under /slow-ai (gates first).
All host-side in `hosted/cocoametal/`; **no `../aros-upstream` change, ABI stays 2.**

### Decisions (answered by John before code)
- **Second LED = Power + Activity, host-only.** Power is real (running + the
  `CM_OPT_POWER` lifecycle). The host can't see real AROS disk I/O without an OS-tree
  hook + an ABI bump, so the second LED is an honest **Activity** light driven by
  `cm_present` cadence — labeled Activity, not DF0. A true DF0 disk LED is a documented
  follow-up (needs `arch/*/trackdisk`/emul hook + ABI v3 + HIDD rebuild).
- **Theme via `NSApp.appearance`, three-way:** System (nil) / Light (Aqua) / Dark
  (DarkAqua). New host-acted key `CM_OPT_THEME = 0x05` in the reserved `0x05..0x0F`
  range — exactly the `CM_OPT_RETINA = 0x04` precedent, so **CM_ABI_VERSION stays 2**.
  Default **System**.
- **Native material status bar:** an `NSVisualEffectView` (titlebar/window material)
  for the footer background — auto-tracks the theme, native vibrancy, least code.

### Change-set (one line per file)
- `cocoametal.h` — add `CMTheme {SYSTEM,LIGHT,DARK}` + `CM_OPT_THEME = 0x05` (host-acted).
- `cocoametal.m` — `CMContext.theme`; set/get cases; persist via `cm__apply_persisted_
  options`; weak no-op `cm__apply_theme_appkit` (mirrors `cm__set_fullscreen_appkit`);
  a cheap `cm__note_present()` activity tick in `cm_present`.
- NEW `cocoametal_statusbar.m` — the status bar view (brand + LED cluster), the LED
  model, strong `cm__apply_theme_appkit` (sets `NSApp.appearance`), the present-driven
  activity sampler. Keeps `window.m` from bloating.
- `cocoametal_window.m` — replace the inline footer (lines ~253-263) with the new
  builder; `FOOTER_H` unchanged.
- `settings.json` + `cocoametal_settings_schema.m` — a `display.theme` popup →
  `hostOption CM_OPT_THEME`; one line in the `CM_OPT_*` const map.
- `cocoametal_shell.m` — a `View ▸ Theme` submenu (System/Light/Dark) with checkmarks.
- `Makefile` — add `cocoametal_statusbar.m` to the `cocoametal-dylib` source list.

### Trade-offs / uncertainties
- The black Metal area (the guest framebuffer) is deliberately NOT themed — only the
  app chrome + footer. "Matches the app" = chrome coherence, not recoloring AROS.
- Does `NSApp.appearance` reflow live windows under our hand-pumped run loop (no
  `[NSApp run]`)? The one doc-only unknown — verified by screenshotting each theme.

### Things to discuss in the [LED+THEME] walkthrough
- The DF0-disk-LED honesty call (Activity now, real disk later) — right boundary?
- `NSApp.appearance` under the hand-pumped loop — did it reflow cleanly?
- Is a host-acted `CM_OPT_THEME` the right home, vs a pure-defaults pref the status
  bar reads itself (no ABI surface at all)?

### [LED+THEME] DONE — what shipped + the decisions that landed
Built + green: `make cocoametal-statusbar` → `[STATUS] PASS`, and the existing
`[ABI]`/`[GSHELL]`/`[D1]`/`[D2t]`/`[D4D5]`/`[J5l fullscreen]`/`[LIVE]`/`[SET]` all
re-ran green. CM_ABI_VERSION stayed **2**. New TU `cocoametal_statusbar.m`; the
footer is now a native-material status bar (brand + PWR/ACT LEDs); theme is the
host-acted `CM_OPT_THEME = 0x05`, surfaced in Settings (General ▸ Appearance) and
View ▸ Theme, persisted in `cocoametal.theme`.

Decisions that landed during the build:
- **The Activity LED is labeled `ACT`, not `DF0`.** It's honestly a present-cadence
  light (the host can't see AROS disk I/O), so calling it DF0 would misrepresent it.
- **`presentCount` was already there** — the Activity LED samples it via a new
  `cm__present_count` accessor on a 0.1s main-loop NSTimer (common modes), so
  `cm_present` itself is untouched. The timer DOES fire under the hand-pumped run
  loop (proven: activity peaks 1.00 on presents, decays to 0.00 when they stop).
- **`NSApp.appearance` reflows live under the hand-pumped loop** — the one doc-only
  uncertainty, now resolved: the [STATUS] test asserts Dark→DarkAqua, Light→Aqua,
  System→nil on the live `NSApp` + window, no `[NSApp run]`. We also push the
  appearance onto each window + `displayIfNeeded` to force an immediate redraw.
- **Power LED needed a sentinel.** `reqPower`'s default 0 aliases `CM_POWER_REQUEST_
  DOWN`, so a fresh context would read as "shutting down". Added `powerReq` (init -1
  = running/green); amber on reset/request-down, red on force-down/quit.
- **The footer is host chrome the oracle can't see** (it's outside the Metal view, so
  `cm_readback`/`cm_capture_png` miss it). Verified the project way instead: [STATUS]
  asserts the AppKit objects directly AND renders the CMLEDView to a PNG via
  `cacheDisplayInRect` (no Screen-Recording/TCC) — proving the LEDs draw real
  saturated green+amber pixels (`run/…/aros-statusbar-leds.png`).
- **Link discipline:** `cocoametal_window.m` now calls `cm__build_status_bar`, which
  only `cocoametal_statusbar.m` defines. The non-statusbar test builds (d1/d2t/input/
  fullscreen/livedraw link window.m but not the status bar) link via a weak nil stub
  in the AppKit-free `cocoametal.m` (`@class NSView;` keeps it AppKit-free) — the same
  weak/strong split as `cm__install_shell`. All those targets re-ran green.

Pre-existing, NOT mine: `cocoametal-hiddsim` FAILs on its *incremental* sub-rect
upload→compose assert. My diff never touches `cm_upload_rect`/`cm_present`/
`cm_readback`/the oracle, and `[ABI]` (full-frame oracle, exact) passes — it's the
documented fbRing "requires full-frame uploads" tension in untouched code.

## Things to discuss in the walkthrough

- Why QEMU-first instead of attacking Apple Silicon head-on (observability + the
  undocumented-hardware wall — see the conversation that started this).
- The "all observation channels normalize to one verdict block" design, and why
  that's what makes an AI agent able to drive months of OS bring-up unattended.
- Where this plugs into real AROS: M2–M8 mirror what AROS's `exec` + the AArch64
  `kernel.resource` will need (vectors, MMU, context switch). Phase 1 is that
  backend in miniature, proven on QEMU, before grafting onto the AROS tree.

## 2026-06-28 — [FF0] native libavutil on AROS (PLAN, under /slow-ai)

Native ffmpeg port, first milestone: build **libavutil only** and prove it with one
AROS `C:` smoke (`av_version_info()` + `av_malloc`/`av_free`). A *native software
port* (ARM code built for AROS by the crosstools), the sibling of the Rust port.
Design: `docs/features/ffmpeg-native/README.md`.

### Gate 1 — reference summary (verified before designing)
A libc-heavy C probe (`stdio`/`stdlib`/`string`/`math`/`stdint`/`inttypes`)
compiled, linked, and **ran on booted AROS** (`sqrt2=1.414214 lrint=4`) — so
`printf` incl. `%f` floats (formerly broken), `malloc`/`free`, libm, `%PRId64` all
work. The toolchain entry point is the **AROS-patched clang driver**
(`tools/crosstools/bin/clang --target=aarch64-unknown-aros`): it auto-applies the
AROS spec includes (so the `cc_include` host-SDK pollution that bit the Rust glue is
avoided) and uses `collect-aros` as linker. Three AROS-specific flags, each grounded:
- `-mcmodel=large` → GOT-free `MOVW_UABS_*` relocs (LoadSeg-compatible; same lesson
  as Rust). Default small model risks a GOT the loader has no support for.
- `-Wl,--allow-multiple-definition` → the `AROS_LIBREQ` duplicate marker (UPSTREAM
  -NOTES item 18), link-only.
- `COMPILER_PATH=<tree>/tools:<tree>/tools/crosstools/bin` → driver finds
  `collect-aros`, and `collect-aros` finds `ld`.
API confirmed against Homebrew ffmpeg 8.1.2 headers: `avutil_version()`,
`av_version_info()`, `av_malloc(size_t)`, `av_free`.

### Gate 2 — change-set (one line each)
NEW `hosted/ffmpeg/` (mirrors `hosted/rust/`):
- `aros-cc.sh` — `--cc` wrapper: the AROS clang driver + the three flags. ffmpeg's
  `./configure --cc=…` points here; reusable for any configure-style port.
- `build.sh` — fetch pinned ffmpeg into gitignored `build/`, `configure` (cross,
  `--disable-everything`, libavutil only, `--disable-asm`), `make libavutil`.
- `ff0_main.c` — the AROS C: smoke (version string + non-null alloc → PASS/FAIL).
- `README.md` — provenance + status + the verified recipe.
- `graft/ffmpeg-smoke` — end-to-end (build → link → deploy → boot → assert), the
  `rust-smoke` sibling.

### Gate 3 — data flow
`build.sh` fetches ffmpeg n8.1.x → `configure --enable-cross-compile --arch=aarch64
--target-os=none --cc=aros-cc.sh --ar/--nm/--ranlib=<crosstools llvm-*>
--disable-everything --disable-asm --enable-static` (configure runs compile/**link**
probes through the wrapper) → `make libavutil/libavutil.a` → `aros-cc.sh` links
`ff0_main.c` + `libavutil.a` into an ET_REL `C:` command → `ffmpeg-smoke` boots AROS
and asserts the printed version + non-null alloc.

### Gate 4 — trade-offs / uncertainties
- **The real FF0 risk: ffmpeg's `configure` link-probes on a non-hosted target.** One
  wrongly-failing probe could misdetect/abort. Mitigation: `--disable-everything`
  shrinks the probe set; read the actual `configure` after fetch; pass explicit
  `--disable-*` if needed.
- **`--target-os=none`** (no `aros` os in ffmpeg) for FF0; a real `aros` arm is a
  later upstreamable patch.
- **Version pinned n8.1.x** (matches reference headers; FF0 is version-insensitive).
- **Source fetched, not vendored** (LGPL/GPL; built, not modified/committed).
- **Out of scope (boundary):** codecs (FF1), threading (FF2), NEON (FF3).

### Things to discuss in the [FF0] walkthrough
- Did `--disable-everything` keep configure's probe set small enough that none
  misdetect on a non-hosted target?
- The `aros-cc.sh` wrapper as a reusable "configure-style port" toolchain entry —
  right abstraction for the later ffmpeg codecs and other autotools ports?

### [FF0] DONE: native libavutil runs on AROS (PROVEN LIVE 2026-06-30)

`FF0Smoke` on booted AROS (`/tmp/arosbuild`): `libavutil 60.26.102 version_info=8.1.2`,
`av_malloc(4096) ok writable`, `av_mallocz(4096) ok zeroed`, `FFMPEG-AROS: [FF0] ALL
PASS`. Proof screenshot `run/darwin-aarch64/ff0-on-aros.png`.

Decisions / findings that landed during the build:
- **The "mistake" fix was `-D_GNU_SOURCE`, not a shim.** The four functions
  (`fdopen`/`mkstemp`/`tempnam`/`posix_memalign`) were always in the posixc headers,
  behind `_GNU_SOURCE`/`_XOPEN_SOURCE`; `--target-os=none` left the flag off.
  kalamatee diagnosed it. Dropped `aros-compat.h`, added `--extra-cflags=-D_GNU_SOURCE`.
- **Toolchain is split across trees (cost an hour, now in `[[aros-build-tree-layout]]`).**
  Canonical SDK at `/tmp/arosbuild`, AROS-patched crosstools at `/tmp/aros-crosstools`.
  Old `/private/tmp` session-scratchpad copies get OS-GC'd half-empty, so discovery
  must require a COMPLETE SDK (posixc/stdio.h + libmui.a + collect-aros), not newest
  clang. `aros-cc.sh` rewired: the patched clang's spec bakes a stale dir, so it uses
  explicit `-isystem` for the SDK includes and `-nostartfiles -nodefaultlibs` + explicit
  AROS startup/lib group at link.
- **`/tmp/arosbuild`'s distribution is MISSING `Libs/stdcio.library`** (it has the static
  `libstdcio.a` + headers, and posixc/stdc/dos/intuition runtime libs, but not the
  stdcio runtime module). FF0Smoke needs `StdCIOBase` via libavutil/posixc (can't be
  dropped: `-lstdcio` removal leaves `__aros_getbase_StdCIOBase` undefined). Stopgap:
  copied `stdcio.library` from a7d73cfa into `/tmp/arosbuild/Libs`. The real fix is the
  build installing it. **Open item: make the build install stdcio.library.**
- **Run method matters:** `graft/aros-ctl run` stages a known-good console boot; type +
  `shot`. Do NOT use `bench-run`'s custom Startup-Sequence on `/tmp/arosbuild` (it
  cold-start-halts at dos.library). Boots crash intermittently (known app bug): retry.

## RESUME 2026-06-30 (post-compaction handoff)

**Objectives next session, in order:**
1. **Quick wins first.** (a) Re-verify rust on `/tmp/arosbuild`: build RustHello
   (`hosted/rust/build.sh --build` + `aros-build.sh`), deploy to `/tmp/arosbuild`'s
   `AROS/C`, run via `aros-ctl` (below). (b) Switch `graft/ffmpeg-smoke` +
   `graft/rust-smoke` off `bench-run` to the `aros-ctl run` + `MacRW:` text-capture
   method (bench-run's custom Startup-Sequence cold-start-halts on `/tmp/arosbuild`).
2. **The big one: merge `origin/master` into the graft + full rebuild + boot-verify.**

**Merge facts:** graft `../aros-upstream` branch `aarch64-darwin-graft` HEAD `a3253c3d`;
`origin/master` `58b02588` (28-commit delta, mostly `stdc`/`posixc` conformance by
kalamatee + WiFi/raspi/AROSTCP by bsek); merge-base `5c78c7d5`. The known `__vcformat`
conflict is GONE (I reverted `__vcformat.c` to original-gated, so kalamatee's `fb49cfa4`
should merge clean). Watch for other conflicts in `stdc`/`posixc`/`config/make.tmpl`.
Reference worktree (detached at master) at `../aros-upstream-master`; remove with
`git -C ../aros-upstream worktree remove ../aros-upstream-master`.

**Build a module (or full):**
```sh
cd /tmp/arosbuild
export PATH="/tmp/graft-tools:/tmp/aros-crosstools/bin:/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:$PATH"
make compiler-stdc          # or compiler-stdcio, or a full `make`
```
`/tmp/graft-tools/objcopy` -> `llvm-objcopy` (macOS lacks objcopy). Toolchain split:
SDK `/tmp/arosbuild`, crosstools `/tmp/aros-crosstools` (see [[aros-build-tree-layout]]).

**Run a C: command + screenshot (the method that works):**
```sh
graft/aros-ctl run            # known-good console boot of /tmp/arosbuild
graft/aros-ctl type "FFProbe"; graft/aros-ctl enter
graft/aros-ctl shot run/darwin-aarch64/x.png   # then read the PNG
graft/aros-ctl stop
```
Boots crash intermittently (known app bug): retry a couple of times. NOT my code.

**Done + verified this session (don't redo):** ffmpeg `[FF0]` PROVEN LIVE
(`hosted/ffmpeg/`, libavutil 60.26.102; proof `run/darwin-aarch64/proofs/ff0-on-aros-20260630.png`);
`-D_GNU_SOURCE` replaced the compat shim; toolchain/tree-discovery fixed; `stdcio.library`
built+installed into `/tmp/arosbuild`; `__vcformat` link-layer fix committed (`a3253c3d`,
printf `%f` verified). `hosted/ffmpeg/*` changes in the Macaros repo are UNCOMMITTED
(on disk, safe) — commit when ready.

**Caveat to close in the rebuild:** the `__vcformat`/LDFLAGS fix was verified on one
disk program + the existing boot, NOT a full-tree rebuild. The merge's full rebuild is
also the confirmation that no early disk module regresses from dropping `-lstdc.static`
from the general LDFLAGS.

## Decision: the 68k proxy-seglist representation ([T0-P1], 2026-08-01)

For transparent 68k execution (docs/features/68k-transparent-exec/), hunk
binaries on non-m68k builds are represented as a **proxy seglist**: the hunk
payloads live in a 32-bit guest arena, relocated with **guest addresses** by
the [J4] relocator (the exact upstream algorithm), and LoadSeg's return value
is a chain of small **native** nodes — one per hunk, `[BPTR next][descriptor]`
— that the existing DOS machinery handles unchanged: fast-BPTR identity for
`GetSegListInfo(GSLI_68KHUNK)`, the blind `UnLoadSeg` walk frees the nodes,
and the guest arena teardown hangs off the **seg-registry removal** (the same
list entry that provides identity), so identity and lifetime share one
mechanism and `UnLoadSeg` needs no knowledge of the arena.

Why not the alternatives: the upstream loader itself cannot run on darwin
(hard `MEMF_31BIT` requirement vs. no low-4 GiB host mappings —
`internalloadseg_aos.c:196` / `bootstrap/memory.c:57`), and a guest-resident
seglist chain cannot be walked by native 64-bit DOS. Proof:
`make hosted-emu68k-t0p1` (`hosted/emu68k/t0p1_seglist.c`) — two real hunk
binaries (integer + hardware FP) loaded, identified (positive + negative),
run byte-exact through the full JIT **from the proxy chain alone**, and
unloaded leak-free.

Two byproduct findings, both feeding later phases:
- **The engine is single-run-per-process today**: a second `j5d_run` after
  `j5d_run_free()` in one process executes stale chained blocks in freed JIT
  memory (EXC_BAD_ACCESS). First-hand confirmation of the [T0-P3] scope; the
  proof forks one process per run.
- **libjit68k.a consumers must link `-Wl,-force_load`** (weak-default/strong-
  override pairs across archive members; documented at the Makefile target).

## Decision: engine instances + safe points land IN the engine ([T0-P3], 2026-08-01)

The 68k JIT is now multi-instance and interruptible, proven by
`make hosted-emu68k-t0p3`:

- **Instances.** All run-scoped engine state (block cache, stats, counters,
  flags) moved into `struct j5d_engine_state`; a process-wide active-instance
  pointer selects it, and the historical `g_*` names are `#define` views, so
  the ~60 existing use sites compiled unchanged. Load-bearing detail: the
  translate-time `&g_xxx` address bakes resolve to the OWNING instance, so
  cached blocks stay correct across instance switches.
- **Safe points.** The one place chained block->block execution avoids the C
  dispatcher is the block chain entry, so that is where the check is emitted
  (~11-19 words: test the instance's stop flag; on park, load any
  dirty-not-live FP regs so the epilogue's stores are value-preserving, write
  `yield_word = (1<<32)|entry_pc`, take the epilogue). The C dispatcher polls
  per roundtrip. `j5d_request_stop()` is one volatile store: signal-safe.
- **Yield/resume.** Poll callback answers CONTINUE/YIELD/KILL. A yield returns
  `J5D_RC_YIELD` with `st->pc` set; resume = call `j5d_run` again from
  `st->pc`. The trap found by the proof: the program-exit condition is "RTS
  pops back to the initial SP", so the SP baseline (+ call depth) must be
  saved in the instance across yields — a resumed run re-capturing the mid-run
  SP as baseline exits early on the next same-level RTS (j5t's D0 came out
  wrong exactly this way).
- **Fault containment.** `j5d_run` wraps the dispatcher in `sigsetjmp`; the
  `[J5n]` host-signal handler, after writing the crash bundle, `siglongjmp`s
  back (one-shot) instead of re-raising. The 68k program dies with a clean
  error + bundle; the embedding process survives and can run fresh instances.
  This is the contract the in-OS `RunSeg68k` needs.

Two emit-time gotchas recorded: `str64_offset`/`ldr64_offset` immediates are
BYTE offsets (scaled internally), and inside `translate_block` the caller's
`cb`/tmp block has `.pc` unset — use the `pc` parameter for baked entry PCs.

## Decision: the marshalling annotation vocabulary holds ([T0-P4], 2026-08-01)

`make hosted-emu68k-t0p4` proves the design-§4 schema on the five representative
cases with ONE generic descriptor-driven marshaller (the shape the genmodule
back-end will emit): scalar / buffer+length / C-string / opaque handle /
shadow struct (host-layout copy of BE guest fields, per-field direction map,
big-endian copy-back of OUT fields) / callback hook (nested engine re-entry,
Amiga A0/A2/A1 hook ABI, proven end-to-end from real 68k through the LVO
bridge) / taglist (per-domain kinds). Unknown tags AND unknown LVOs abort as
ledger-recorded capability gaps - the no-guessing rule is now executable
behavior, not prose.

Two findings the spike forced, both now binding:

- **Marshalling is two-pass: scalars first, then pointer kinds.** A buffer's
  length register can be declared after the buffer argument; in-order
  marshalling bounds-checks the buffer against length 0 and the native side
  then writes past the arena (caught here as a real 64-byte heap overwrite at
  the sandbox edge). Lengths are scalars, so pass 0 = scalars/handles,
  pass 1 = everything that dereferences.
- **Nested engine runs need recovery nesting.** A hook re-entering j5d_run
  inside the bridge would clobber the outer run's fault-recovery target;
  j5n_signal_set_recover now returns the previous target and j5d_run
  save/restores it (volatile local, longjmp-safe).

## Decision: the execution boundaries divert to a launch router ([T0-P2], 2026-08-01)

OS-side (fork commit 1eb9082854): both places that jump into a seglist now
hand tracked GSLI_68KHUNK seglists to `Emu68k_RouteSegList` with a versioned
launch context (origin CLI/PROC, seglist, name, args, process, mode). The
router is a stub (report + decline) - the seam the engine attaches to in T1.
DosEntry diverts only when the entry PC came from the seglist, so
SystemTagList's explicit-entry shell case is untouched. Verified live on
booted AROS from both boundaries (shell command with args; C:RunSeg for the
created-process path), with the desktop boot smoke green.

Findings:
- **Hunk LoadSeg failed entirely on darwin** (MEMF_31BIT unsatisfiable: no
  32-bit-addressable memory exists), so nothing downstream was reachable.
  The old "keymaps load as garbage" era predates the high RAM map. Enabler:
  64-bit fallback to plain memory for the hunk allocation. Contained: the
  relocations are truncated but nothing interprets them - execution diverts
  at the boundaries, parsekeymapseg already declines >4GiB seglists, and
  ReadDiskFont now declines them identically (the one data consumer that
  parsed blind). The real guest-address loader ([T0-P1]'s proxy seglist)
  replaces this fallback in T1.
- **GSLI_* tag constants live in the GENERATED dos/dostags.h** (SDK), not in
  compiler/include - grepping the source tree for them finds only users.

## Milestone: 68k programs run inside AROS ([T1], 2026-08-01)

Real classic-Amiga 68k hunk binaries run from the booted AROS shell, byte-exact
against the host engine. The chain: DOS entry boundary -> launch router ->
emu68k.library -> hostlib -> libemu68k.dylib -> the JIT.

Design points worth keeping:

- **The source-image stash.** The hunk seglist payload is not interpretable on
  this host (no 32-bit-addressable memory, truncated relocations), so the
  loader keeps an AllocVec'd copy of the source FILE with the registry node
  (`struct hunk_segnode`), freed with the node at unload. The router hands
  those bytes to the library, which re-loads them into a guest arena. This is
  the [T0-P1] proxy-seglist idea, minus the proxy chain: DOS keeps identity and
  lifetime, the engine keeps the guest address space.
- **Quanta, not a blocking call.** emu68k.library runs the program in bounded
  quanta under a semaphore, polling CTRL-C between them. That is what makes
  several translated processes interleave and a kill land.
- **The chain budget is load-bearing** (engine change forced here): the
  chain-entry safe point decrements a budget and parks at zero rather than only
  testing a stop flag. A self-chained loop never returns to C on its own, so
  without a budget nothing in-OS could ever poll CTRL-C; with it, `Break` kills
  a runaway 68k loop and the desktop stays live.
- **Failures are classified, not fatal.** An unmarshalled library call is a
  capability-gap abort with a per-LVO ledger entry (hit count + program name);
  a translated-code SIGSEGV is contained, bundled, and the next program runs.

Verified in one boot (run/darwin-aarch64/t1x-195732): hardware-FP j5t and fact
byte-exact from the shell, background Dhrystone interleaved with foreground
Mandelbrot both byte-exact, a gap reported, a fault contained with the next
program fine after it, a chained infinite loop killed with Break.

## Decision: how hardware use is detected ([T2a]/[T2b], 2026-08-01)

Two independent mechanisms, deliberately calibrated against different failure
costs. A wrong FULL sends a working program to an emulator (expensive, visible),
a wrong JIT is cheap (the guard catches it), so both lean toward JIT.

**Static scan** (`hosted/emu68k/scan68k.c`, shared by the `scan68k` CLI and the
in-OS router). The calibration was the work:

- A 2-byte sweep reads operands as opcodes and flagged every real program as a
  hardware banger. The scan now walks real instruction boundaries with a length
  table and STOPS at the first word it cannot size - never a guessed resync,
  because a desynced walk invents findings.
- With exact lengths the absolute operand is located precisely rather than
  guessed from the following words.
- Relocation sites are excluded: dhrystone's `move.l #0,$64` initialising a
  global looks exactly like an exception-vector write until you notice the
  address is a relocation target. Bitmap, so any number of relocs costs the same.
- `RTE` is not a banger signal even though it is privileged: the engine
  implements 68k exception dispatch, so the list is about THIS engine's reach.

**Runtime guard** (`hosted/emu68k/emu68k_host.c`): implemented as absence, not
as checking. The low 16 MiB of guest space is one PROT_NONE reservation with
only the arena window mprotected RW, so the chipset/CIA/vector page are simply
not mapped; a touch faults and the [T2b] classifier converts the faulting host
address back to the guest address it came from. No emitted bounds check, no hot
path cost. Two mechanical traps: a malloc'd arena does NOT fault just past its
end (reads land in the allocator's pages and silently succeed), and mprotect
rounds the length UP so a partial page handed the guest the very registers the
arena is sized to exclude - the window is page-aligned DOWN.

This is why the in-OS arena is ~9.9 MiB where run68k standalone uses 16 MiB: a
16 MiB arena swallows $DFF000.

## Milestone: the AmigaOS library bootstrap works in-OS ([T3] first slice, 2026-08-01)

A 68k program doing the universal Amiga idiom - `move.l 4.w,a6`, exec
OpenLibrary("dos.library"), calls through the returned base - runs from the
AROS shell with the calls performed by the REAL native dos.library.

Design points:

- **SysBase lives at guest address 4 in a READ-ONLY page.** Reading it is the
  standard idiom and must work; writing an exception vector is a program taking
  over the machine and must still fault into the [T2b] guard. One mapping gives
  both.
- **The engine recognises many library bases per run** (j5d_register_libbase).
  The bridge callback reads A6 to know which library was called; its signature
  is unchanged.
- **exec AllocMem is served from the GUEST heap**, never from the native
  allocator: the program dereferences what it allocates, so the memory must be
  inside the arena it can address.
- **Handles cross as opaque tokens.** A native BPTR is a 64-bit pointer; a 68k
  register holds 32 bits. Truncating Output()'s BPTR was the first live failure
  (a fault inside dos.library), exactly the handle case [T0-P4] predicted. A
  small token table maps 32-bit values the program never dereferences back to
  the real BPTRs.
- **The host stays OS-agnostic.** The oscall seam hands (libname, lvo, regs,
  guest base) to the embedder; emu68k.library performs the native call. The
  same seam is where the [T3a] generated tables will plug in.

## Method: harden against real software, ranked by what it asks for (2026-08-01)

`graft/68k-corpus` + `graft/68k-corpus-summary` make the loop one command: run a
folder of real 68k programs on booted AROS, get a verdict per program and a
ranked list of the missing calls, implement the top one, re-run. The gap names
come from AROS's own generated headers, so they are exact.

What two rounds against 20 Aminet programs actually taught (none of it guessable):

- **AllocVec, not AllocMem**, is what real code allocates with. It was the
  single most-wanted call in the corpus.
- **`ExecBase->ThisTask` is read directly**, not via FindTask. With it zero, a
  program reads pr_CLI from address 0, concludes it was launched from Workbench,
  and waits on a message that never comes. Two unrelated programs stopped there.
- **`OldOpenLibrary` is still in use** by pre-2.0 binaries.
- **Library vectors are not only reached through A6.** `move.l a6,a0 ; jsr
  -294(a0)` is ordinary compiler output; the engine now recognises a vector by
  its target address.
- **A base you can only call is not enough** - programs read fields from it, so
  the bases had to move inside the mapped arena.
- **List operations belong in the guest.** AddHead/AddTail/Remove manipulate
  guest structures; calling the native ones would write host pointers into a
  list the program cannot address.
- The guest heap being 28 KB was invisible until DMS said "Memory Allocation
  Failure" in its own words.

Each round moves the frontier: round 1's gaps were all exec, round 2's were all
dos, which is itself the signal that the bootstrap layer is done.

## Decision: ReadArgs is implemented in the guest, not bridged (2026-08-01)

`ReadArgs` is how every modern AmigaDOS CLI tool reads its arguments, so it is
the single call that decides whether ordinary command-line software does
anything useful. It is implemented over GUEST memory in the host service rather
than by calling the native AROS ReadArgs, because everything it produces is a
pointer the program dereferences: the argument strings, the LONG a `/N` writes,
the NULL-terminated vector a `/M` builds. Native pointers would be addresses the
guest cannot reach - the same rule that made file handles cross as tokens, one
level up.

Covered: positional, `/S`, `/K`, `/N`, `/A`, `/M`, `/F`, `=ALIAS`. Keywords are
matched anywhere on the line first, then the remaining tokens fill positionals
in template order, which is the AmigaDOS behaviour programs rely on. A missing
`/A` returns NULL with IoErr set - the failure path programs are written to
handle. `FreeArgs` is a no-op against the bump-allocated guest heap.

`make hosted-emu68k-t3readargs` drives a real 68k program through
`FILE/A,COUNT/N,ALL/S` and checks both directions (parses, and fails when the
required argument is absent).

## Findings from nine corpus rounds against real Aminet software (2026-08-01)

The loop (`graft/68k-corpus` + `graft/68k-corpus-summary`) ran nine times, each
round implementing whatever the ranking put on top. Beyond the calls themselves,
four findings were structural and none were guessable:

- **The loader was rejecting real programs.** It required a CODE/DATA hunk's
  payload length to EQUAL the header's size word. The header size is the
  ALLOCATION size and may exceed the payload; the real AROS loader leaves the
  remainder as the zeroed memory it allocated. AutoDoc from Aminet never ran a
  single instruction because of this, and since a load failure prints nothing,
  it looked exactly like a program that ran and said nothing.
- **A program that is silent AND reports no gap has given up somewhere**, and
  the only way to find where is to watch what it called: `EMU68K_TRACE_CALLS`
  logs every library call. That is what identified the loader bug.
- **Eight library bases were not enough** - real programs open more than that,
  and the failure surfaced as a bare "OpenLibrary gap" because `bridge()` was
  burying exec's specific message under a generic one. Specific reasons now
  survive.
- **Bases need a version.** A zeroed base reports lib_Version 0, and programs
  written for AmigaOS 2.0+ check that before doing anything.

Where nine rounds left the 20-program batch: 2 running, 5 stopped at a named
gap, 1 routed to hardware, 1 timing out, 10 running to completion without
output (which for tools that need arguments is their own correct behaviour).
The frontier is no longer dos: it is `MatchFirst` (2 programs) and the
GUI libraries (intuition, commodities), which is the honest signal that the
CLI/dos layer is broadly served.

## Finding: a guest blocked in a native call is past the guard (2026-08-02)

`EMU68K_MAX_SECONDS` and CTRL-C both land at quantum boundaries, which means
they can interrupt any amount of *translated* execution - including a fully
chained loop. What they cannot reach is a guest blocked inside a NATIVE call:
PPMore is a text viewer, it called Read on the console waiting for a keypress,
and the sweep sat there indefinitely because the program was not executing 68k
instructions at all.

Two consequences worth keeping:

- The corpus harness runs every program with `<NIL:`, so an interactive tool
  reads EOF instead of blocking a batch.
- The general fix, when it matters, is a timeout on the native side of the
  bridge rather than in the engine - the engine is not where the program is.

**Verified, so the guarantee is not overstated:** the wall-clock guard DOES stop
a fully chained infinite 68k loop (`EMU68K_MAX_SECONDS=2` kills it in two
seconds, "killed at safe point"). PPMore is therefore NOT looping in translated
code - it stops making library calls after AllocMem and hangs somewhere the
guard cannot see, and `<NIL:` input did not change that. Unidentified; it is
excluded from sweeps rather than left to stall them.

## Rule: bridge to AROS by default; implement in the guest only when the RESULT cannot cross (2026-08-02)

AROS is an AmigaOS reimplementation and has a full m68k-amiga target, where a
68k program calls the real 68k exec/dos with no translation at all. We cannot
have that on Apple Silicon without emulating a whole Amiga - which is exactly
what the FULL route delegates to. So model 2 is not a duplicate of AROS/m68k:
it is the case AROS/m68k cannot serve on this machine.

That said, the standing rule for every call is: **use AROS's implementation
unless the result cannot cross the boundary.**

- BRIDGE (the default): the work is AROS's and only the arguments are
  translated - Write/Open/Read/Seek/GetVar/SetVar/IoErr/Delay/PrintFault.
- GUEST-SIDE (only when forced): the call PRODUCES something the program
  dereferences or walks, which a native result cannot be - OpenLibrary (a guest
  base; AROS returns a 64-bit pointer a 32-bit big-endian program cannot hold),
  AllocMem and friends (guest arena), the list ops, InitSemaphore/CreateMsgPort
  (guest structures), FindTask (the guest Process).
- NO-OP: arbitration a single-threaded guest does not need (Forbid/Permit,
  Obtain/Release).

By that rule exactly ONE call is a genuine reimplementation: ReadArgs, because
working out what each array slot means requires parsing the template anyway.

**Correction to the earlier MatchFirst plan.** It was written as "guest-side,
like ReadArgs". That is wrong and would mean reimplementing AmigaDOS pattern
syntax when AROS already has it. The right shape is: call the NATIVE
MatchFirst/MatchNext into a NATIVE AnchorPath, then copy the few fields the
program actually reads (fib_FileName, fib_DirEntryType, fib_Size) into the guest
AnchorPath - the shadow-structure pattern from [T0-P4], not a rewrite.

The long-term answer to "are we hand-writing too much" is [T3a]: generate the
crossings from the .conf files instead of hand-writing them. The hand-written
table earned its keep by telling us WHICH functions real software needs; it
should not be how they stay implemented.

## Bug class: BPTRs handed to the guest must be SHIFTED (2026-08-02)

A 68k program is a classic AmigaOS program, and there a BPTR is the address
DIVIDED BY FOUR - `BADDR(x)` is `x << 2`. Native AROS on this target is built
with `AROS_FAST_BPTR`, where a BPTR is simply a pointer. So it is easy to plant
a raw address in a guest structure and be wrong in a way nothing complains
about: the program shifts it left by two and reads somewhere else entirely.

That is exactly what happened. `pr_CLI` was planted as $222400; the program
computed `BADDR` and got $889000 (visible in a call trace as `a0=00889000`),
and the resulting faults landed at guest $1E0004/$1E0008 - addresses derived
from reading structures that were never where the program looked. Three of the
crash bundles from one sweep were this.

**The rule: every BPTR-typed value handed to the guest goes through
`GUEST_MKBADDR` (>>2), and every BPTR received from the guest through
`GUEST_BADDR` (<<2).** Fields affected: pr_CLI (fixed), and still outstanding -
the file-handle tokens returned by Output/Input/Open, which a program that
dereferences its handle (to read fh_Type, say) will BADDR into nonsense. The
fix there is to back each token with a real guest structure and hand out
MKBADDR of its address, rather than an opaque tag.

This is the third instance of one theme: values crossing into the guest must
be shaped the way a 68k program expects, not the way the host holds them
(64-bit handles needed tokens, native pointers need translation, BPTRs need
shifting).

## Clipboard: one chord must have one owner (2026-08-02)

Shell copy/paste was broken in three independent ways at once, and the fixes
only make sense together.

`console.device`'s rawkey handler runs at priority 51, above intuition's 50.
That is deliberate. It means the console sees Right-Amiga+C/V **before**
intuition can turn the chord into a menu shortcut, so console.device is the
chord's natural owner. Giving the console window's Edit menu CommKeys `"c"` and
`"v"` for the same chord makes both act, and every operation happens twice. The
menu items stay (mouse access) but carry no CommKey; they inject the chord
directly into `CDInputHandler`, which intuition never sees.

`con-handler` computed its window signal mask once, before entering its event
loop. The boot console is opened `AUTO` and so has no window at that moment, so
the mask was zero for the life of the handler and no `IDCMP_MENUPICK` was ever
read in that window. An `AUTO` console also closes its window at EOF, which
left the same captured mask pointing at a freed port. The mask has to be
recomputed each time round the loop.

Known issues left open, deliberately:

- The bridge polls at 5 Hz, so copies made faster than ~200 ms apart coalesce
  and only the last crosses. Measured 3 of 8 at 50 ms spacing, 8 of 8 at 300 ms.
- Every host call sits inside `Forbid()` + `HostLib_Lock()`, so each pasteboard
  round trip stalls the whole guest scheduler briefly, five times a second.
- A `CBD_POST` clip can block the bridge indefinitely in `CMD_READ`. Latent: no
  in-tree program posts rather than writes.
- There is no bridge shutdown path; host process exit tears it down.
- The release bundle assigns `CLIPS:` to `SYS:clips` inside the signed `.app`.
  It is harmless only because the assign fails and clipboard.device falls back
  to `RAM:Clipboards`.
- `hosted/hostshell/settings.json` defaults `sharing.clipboard` to 0 while
  `hosted/cocoametal/settings.json` defaults it to 1.
Resolved while investigating the above, and worth remembering as a pattern:
`AddAudioModes` was ending in a DEADEND alert during desktop boots, which stops
the whole system, and since `ConClip` starts after it in both startup modes the
clipboard chain was dead with it. It was not an audio bug. The fault PC was
`main + 0x2c`, which disassembles to `ldr x2, [x23]` loading the program's own
`SysBase` global for its first `OpenLibrary` call, so the binary died before it
touched audio. It was a stale binary: `AddAudioModes` and `ahi.device` dated
from 07-18 while `posixc.library` and `stdc.library` had been rebuilt on 07-28
and 07-30 during pthread/errno work. `make workbench-devs-AHI-quick` fixed it;
10 of 10 desktop boots clean afterwards, against one captured crash before.

The general rule: after a batch of commits lands, a partially rebuilt tree can
fault in a program's startup long before that program reaches anything it is
actually named for. Check the module's date against the C runtime's before
reading anything into the backtrace.
