# Macaros build and verification entry points.
#
# The `image`/`run`/`shot`/`dbg`/`test` targets preserve the original standalone
# QEMU AArch64 bring-up as a platform-porting reference. Its source now lives in
# docs/hosted/initial-platform-bringup/qemu-virt/boot. Current hosted Macaros
# targets follow below.
#
#   make run                 build + boot + verify the default marker ([M1])
#   make run MARKER='[M3]'   verify a specific serial marker
#   make shot MARKER='[M9]'  same, plus a framebuffer screendump
#   make dbg                 boot frozen with a gdbstub + scripted lldb state dump
#   make clean
#
# Toolchain: LLVM (clang cross-compiles to any target; ld.lld emits ELF). The
# eventual real AROS port will use AROS's own GCC crosstools — this is just the
# bring-up toolchain. Override LLVM=... if your brew prefix differs.

LLVM    ?= /opt/homebrew/opt/llvm/bin
LLD     ?= /opt/homebrew/opt/lld/bin
CC      := $(LLVM)/clang
LD      := $(LLD)/ld.lld
OBJCOPY := $(LLVM)/llvm-objcopy

TARGET  := aarch64-none-elf
WARN    := -Wall -Wextra
COMMON  := --target=$(TARGET) -ffreestanding -nostdlib $(WARN)
ASFLAGS := $(COMMON)
# MMU is off until M4: all RAM is Device memory, so force aligned accesses and
# keep the compiler off the FP/NEON registers (not enabled yet).
CFLAGS  := $(COMMON) -O2 -mstrict-align -mgeneral-regs-only -fno-stack-protector

ELF     := build/aros-aarch64.elf
QEMU_BOOT := docs/hosted/initial-platform-bringup/qemu-virt/boot
OBJS    := build/start.o build/kmain.o build/uart.o build/shell.o build/exc.o build/vectors.o build/mmu.o build/irq.o build/pmm.o build/task.o build/switch.o build/fb.o build/sched.o
MARKER  ?= [M10]
# Cumulative markers a healthy boot prints, in order. Extend as milestones land.
MARKERS ?= [M2] [M3] [M4] [M5] [M6] [M7] [M8] [M9] [M10a] [M10]
# Keystrokes fed to the M8 shell over the serial socket (\n decoded by printf %b).
INPUT   ?= ping\nticks\nquit\n

# The AROS OS source tree (kernel, modules, libraries) lives OUTSIDE this repo;
# this one is the host/graft layer. Override if your checkout is elsewhere.
AROS_SRC ?= $(HOME)/Source/aros-upstream
M68K_AROS_BUILD ?= $(HOME)/aros-m68k-build
M68K_AROS_GCC ?= $(M68K_AROS_BUILD)/bin/darwin-aarch64/tools/crosstools/m68k-aros-gcc
ELF2HUNK ?= $(M68K_AROS_BUILD)/bin/darwin-aarch64/tools/elf2hunk
M68K_LIBS_PATH ?= $(HOME)/Source/references/aros-m68k-20260804/libs

.PHONY: image run shot dbg test hosted hosted-run hosted-preempt hosted-abi hosted-exec hosted-mem hosted-kern hosted-display hosted-cocoametal cocoametal-dylib cocoametal-abi cocoametal-shell cocoametal-statusbar cocoametal-hiddsim cocoametal-d2t cocoametal-input cocoametal-settings cocoametal-fullscreen cocoametal-livedraw cocoametal-show hosted-coreaudio coreaudio-dylib coreaudio-abi audio-smoke bench hosted-clipboard pasteboard-dylib pasteboard-abi hosted-hostvolume hosted-bsdsocket hosted-library hosted-signal hosted-msgport hosted-device hosted-execboot hosted-jit68k hosted-jit68k-hardened hosted-jit68k-j2 hosted-jit68k-j3 hosted-jit68k-j4 hosted-jit68k-j5a hosted-jit68k-j5b hosted-jit68k-j5c hosted-jit68k-j5d hosted-jit68k-j5e hosted-jit68k-j5f hosted-jit68k-j5g hosted-jit68k-j5h hosted-jit68k-j5i hosted-jit68k-j5j hosted-jit68k-j5k hosted-jit68k-j5l hosted-jit68k-j5m hosted-jit68k-j5n hosted-jit68k-j5o hosted-jit68k-j5p hosted-jit68k-j5q hosted-jit68k-j5r hosted-jit68k-j5s hosted-jit68k-j5t hosted-jit68k-apps libjit68k run68k hosted-jit68k-args hosted-emu68k-t0p1 hosted-emu68k-t0p3 hosted-emu68k-t0p4 scan68k hosted-emu68k-t2scan hosted-emu68k-t2guard hosted-emu68k-t3hello hosted-emu68k-t3setsignal hosted-emu68k-t3workbench hosted-emu68k-t3readargs hosted-emu68k-t3gen hosted-emu68k-t3mui hosted-emu68k-t3fmt hosted-emu68k-t3guestlib hosted-emu68k-t3guestlive hosted-emu68k-t3ereal rawdofmt-blob struct-layouts hosted-emu68k-t3lha hosted-emu68k-t3legacy hosted-emu68k-regina-fixtures hosted-jit68k-conform hosted-test clean

build:
	@mkdir -p build

build/%.o: $(QEMU_BOOT)/%.S | build
	$(CC) $(ASFLAGS) -c $< -o $@

build/%.o: $(QEMU_BOOT)/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(QEMU_BOOT)/linker.ld
	$(LD) -T $(QEMU_BOOT)/linker.ld $(OBJS) -o $@

image: $(ELF)
	@echo ">> built $(ELF)"

run: image
	IMG=$(ELF) INPUT='$(INPUT)' ./harness/run.sh '$(MARKER)'

shot: image
	IMG=$(ELF) INPUT='$(INPUT)' SHOT=1 ./harness/run.sh '$(MARKER)'

dbg: image
	SYMS=$(ELF) ./harness/lldb-dump.sh

test: image
	IMG=$(ELF) INPUT='$(INPUT)' ./harness/test.sh $(MARKERS)

# ---- Phase 2: hosted on macOS (a native arm64 process; macOS owns the drivers) ----
HOST_BIN := build/host-aros
hosted: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/host.c hosted/switch.S -o $(HOST_BIN)

hosted-run: hosted
	BIN=$(HOST_BIN) ./harness/run-hosted.sh '[H1]'

hosted-preempt: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/preempt.c -o build/host-preempt
	BIN=build/host-preempt ./harness/run-hosted.sh '[H2]'

# H3: the host-call ABI shim — marshal AROS-side args into Apple's arm64 variadic
# ABI (varargs on the stack). The make-or-break cross-ABI boundary, hand-written.
hosted-abi: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/abishim.c hosted/abishim.S -o build/host-abishim
	BIN=build/host-abishim ./harness/run-hosted.sh '[H3] host-call ABI shim ok'

# H4: the AROS exec scheduler model, hosted — priority TaskReady + the real
# core_Schedule/cpu_Switch/core_Switch/core_Dispatch call graph over SIGALRM.
hosted-exec: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/exec.c -o build/host-exec
	BIN=build/host-exec ./harness/run-hosted.sh '[H4] hosted AROS scheduler ok'

# H5: the AROS exec memory model, hosted — MemHeader/MemChunk first-fit + coalesce
# free-list allocator over an mmap'd region (macOS owns the pages).
hosted-mem: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/mem.c -o build/host-mem
	BIN=build/host-mem ./harness/run-hosted.sh '[H5] hosted AROS AllocMem ok'

# H6: a tiny hosted exec — H4 scheduler + H5 allocator composed. Tasks are
# AllocMem'd from the heap, scheduled preemptively, allocator made Forbid-safe.
hosted-kern: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/kern.c -o build/host-kern
	BIN=build/host-kern ./harness/run-hosted.sh '[H6] hosted exec ok'

# H7: the host display driver — AROS draws a framebuffer from its heap, macOS
# presents it (ImageIO PNG). The agent observes the pixels via the PNG file.
hosted-display: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/display.c -o build/host-display \
		-framework ImageIO -framework CoreGraphics -framework CoreFoundation
	BIN=build/host-display ./harness/run-hosted.sh '[H7] hosted display ok'

# D1: the Apple-native Cocoa/Metal display shim — prove the offscreen Metal
# present pipeline + readback oracle, standalone (no AROS build). Also runs the
# resolution-parametric [D2] check (640x512) and the [D] shader-stage check
# (cm_set_effect / CM_FX_SCANLINE: present-time effect, NOT in the oracle path).
# Offscreen BGRA8 target is the source of truth; the present is a render pass
# (framebufferOnly=YES); a live NSWindow is a non-fatal bonus.
# Host clang (NOT AROS crosstools); -fobjc-arc; clean-room from the spec.
hosted-cocoametal: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_control.m hosted/cocoametal/cocoametal_gpu.m \
		hosted/cocoametal/d1_test.m \
		-o build/host-cocoametal \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework ImageIO -framework QuartzCore -framework AppKit
	BIN=build/host-cocoametal ./harness/run-hosted.sh '[D1] PASS'

# Item 1 (INTERFACE.md §1c): the dlopen-loadable cocoametal.dylib — the REAL
# artifact the AROS side loads via hostlib.resource (HostLib_Open + GetInterface).
# Built from cocoametal.m + cocoametal_window.m (+ the cm_abi_version added in
# cocoametal.m); d1_test.m is NOT in the dylib. Pulls no AROS headers. Every cm_*
# symbol is exported with DEFAULT visibility via cocoametal.exports (the 10 frozen
# names in §1a order), the binary is NOT stripped (dlsym resolves by name), and it
# is ad-hoc codesigned so a hosted process can dlopen it on this Mac.
COCOAMETAL_DYLIB := build/cocoametal.dylib
cocoametal-dylib: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra -dynamiclib \
		-install_name @rpath/cocoametal.dylib \
		-exported_symbols_list hosted/cocoametal/cocoametal.exports \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_settings_schema.m hosted/cocoametal/cocoametal_control.m \
		hosted/cocoametal/cocoametal_shell.m hosted/cocoametal/cocoametal_statusbar.m \
		hosted/cocoametal/cocoametal_gpu.m \
		-o $(COCOAMETAL_DYLIB) \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework ImageIO \
		-framework AVFoundation -framework CoreVideo -framework CoreMedia
	codesign -s - -f $(COCOAMETAL_DYLIB)
	cp -f hosted/cocoametal/settings.json build/settings.json
	@echo ">> shipped build/settings.json (schema, resolved next to the dylib)"
	@echo ">> built $(COCOAMETAL_DYLIB) (exported cm_* symbols:)"
	@nm -gU $(COCOAMETAL_DYLIB) | grep ' _cm_' || true

# Item 2 (INTERFACE.md §8 #2 — highest value): the dlopen-based ABI conformance
# test. Plain C, links NONE of the .m files; it dlopens build/cocoametal.dylib the
# exact way HostLib_GetInterface does, dlsym's all 10 frozen symbols (errcount must
# be 0), checks cm_abi_version()==CM_ABI_VERSION, then drives open->upload->present
# ->readback(asserts the §6 oracle: 4 quadrants + marker exact)->pump->set_effect/
# target_size->close through the resolved function pointers. Green = the seam wires.
cocoametal-abi: cocoametal-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/cocoametal hosted/cocoametal/abi_test.c -o build/cocoametal-abi
	BIN=build/cocoametal-abi ./harness/run-hosted.sh '[ABI] PASS'

# GPU compute section ([GPU], docs/features/gpufx): dlopen the dylib, dlsym only
# the cm_gpu_* contract (not the frozen CMIFace), verify nearest scale byte-exact
# vs the CPU reference, bilinear within +/-1, YUV420->RGBA (both ranges) +/-2.
cocoametal-gpu: cocoametal-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/cocoametal hosted/cocoametal/gpu_test.c -o build/cocoametal-gpu
	BIN=build/cocoametal-gpu ./harness/run-hosted.sh '[GPU] PASS all'

# Regenerate the embedded shader library after editing cmshader.metal.
# (cmshader_metallib.h is committed so plain builds never need Xcode's metal
# toolchain; run this rule when the .metal source changes.)
cocoametal-shader:
	xcrun -sdk macosx metal -c hosted/cocoametal/cmshader.metal -o hosted/cocoametal/cmshader.air
	xcrun -sdk macosx metallib hosted/cocoametal/cmshader.air -o build/cmshader.metallib
	xxd -i -n cmshader_metallib build/cmshader.metallib > hosted/cocoametal/cmshader_metallib.h

# Host app shell ([GSHELL]): dlopen the REAL build/cocoametal.dylib, let cm_open
# install the menu bar + About + icon (cocoametal_shell.m), then assert — against
# the production dylib — that the menu tree is installed AND invoking a menu item
# drives the real cm_* ABI (host-acted -> cm_set_option/get_option; AROS-facing ->
# CM_EV_SETTING). The merge's de-risk: the POC's [G-MENU]/[G-ACTION], now through
# the real dylib + real wiring (not a mock sink). Links AppKit; dlopens the dylib.
cocoametal-shell: cocoametal-dylib
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/cocoametal hosted/cocoametal/shell_test.m -o build/cocoametal-shell \
		-framework AppKit -framework Foundation -framework AVFoundation -framework CoreMedia
	BIN=build/cocoametal-shell ./harness/run-hosted.sh '[GSHELL] PASS'

# Status bar LEDs + theme ([STATUS]): dlopen the REAL build/cocoametal.dylib, let
# cm_open build the status bar (cocoametal_statusbar.m), then assert — against the
# production dylib — that the NSVisualEffectView + CMLEDView are installed, that
# cm_set_option(CM_OPT_THEME,…) drives NSApp.appearance (Dark/Light/System), and
# that the Activity LED lights on cm_present and decays when presenting stops. The
# footer is host chrome (not in the oracle), so it is asserted via AppKit objects —
# the same unattended technique as [GSHELL]. Links AppKit; dlopens the dylib.
cocoametal-statusbar: cocoametal-dylib
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/cocoametal hosted/cocoametal/statusbar_test.m -o build/cocoametal-statusbar \
		-framework AppKit -framework Foundation
	BIN=build/cocoametal-statusbar ./harness/run-hosted.sh '[STATUS] PASS'

# D3 host-support (INTERFACE.md §2a + §8): the HIDD-shaped behavioral harness —
# the de-risk + reference for the AROS bitmap-class UpdateRect wiring. Plain C,
# links NONE of the .m files; it dlopens build/cocoametal.dylib (the REAL boundary)
# and drives it the way the AROS HIDD will, BEYOND abi_test's single sequence:
# AROS owns a host-side W*H*4 BGRA8 framebuffer (the AllocMem stand-in) filled with
# the PINNED §2a CMPixelDesc; lazy cm_open on first "Show"; a DIRTY-RECT STREAM of
# partial/overlapping cm_upload_rect+cm_present (the many-small-UpdateRects pattern,
# not a full-frame blit); then cm_readback asserts the composed oracle == an
# independent host reference framebuffer BYTE-EXACT (§6 under realistic usage) and a
# known-pixel round-trip proves B/G/R/A land in the asserted byte positions (catches
# a swizzle bug where the AROS side can't see it). Bounded + watchdog. Marker
# [HIDDSIM] PASS. Additive — no ABI change.
cocoametal-hiddsim: cocoametal-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/cocoametal hosted/cocoametal/hiddsim_test.c -o build/cocoametal-hiddsim
	BIN=build/cocoametal-hiddsim ./harness/run-hosted.sh '[HIDDSIM] PASS'

# Item 3 (INTERFACE.md §8 #3 — the de-risk): D2 under the REAL graft threading
# model. Drives cm_open (window) + cm_present (nextDrawable) + cm_pump_events from
# the MAIN pthread under manual CFRunLoopRunInMode(kCFRunLoopDefaultMode,0,true) —
# NEVER NSApplicationMain / [NSApp run]. Documents the minimal AppKit init the boot
# task must do once and reports whether window/nextDrawable work hand-pumped; the
# offscreen + cm_readback oracle must pass regardless. Links the .m files directly
# (it is a host harness exercising the threading model, not the dlopen seam).
cocoametal-d2t: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_control.m hosted/cocoametal/cocoametal_gpu.m \
		hosted/cocoametal/d2t_test.m \
		-o build/cocoametal-d2t \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	BIN=build/cocoametal-d2t ./harness/run-hosted.sh '[D2t] PASS'

# D4/D5 (INTERFACE.md §5 — input): the REAL cm_pump_events drain. Drives the input
# pump under the same main-pthread / manual-CFRunLoop / NO-NSApplicationMain model
# as D2t, then SYNTHESIZES NSEvents and injects them in-process via
# [NSApp postEvent:atStart:] (no TCC/accessibility) and asserts the drained
# CMEvent[] field-for-field: [D4] mouse move (exact logical x,y, Y-flip) + LMB
# down/up (code=0 pressed 1/0); [D5] keyDown/Up with a known keyCode + Shift
# (code==keyCode, pressed 1/0, mods&CM_MOD_SHIFT). Value-asserting markers
# [D4] PASS / [D5] PASS. Links the .m files (it exercises the AppKit pump path).
cocoametal-input: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_control.m hosted/cocoametal/cocoametal_gpu.m \
		hosted/cocoametal/input_test.m \
		-o build/cocoametal-input \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	BIN=build/cocoametal-input ./harness/run-hosted.sh '[D4D5] PASS'

# SET (INTERFACE.md §9 — settings & options, ABI v2): the host settings panel +
# key/value option ABI. Drives, under the same main-pthread / manual-CFRunLoop /
# NO-NSApplicationMain model as D2t: (1) cm_set_option(CM_OPT_EFFECT,SCANLINE) ->
# cm_present -> the PRESENTED path reflects the effect (odd rows darker, via
# cm_render_effect_readback) while the OFFSCREEN ORACLE stays pass-through
# unchanged; cm_get_option reflects it. (2) cm_set_option of an AROS-facing key
# (CM_OPT_REQUEST_MODE_W=640,_H=512) -> cm_pump_events returns a CM_EV_SETTING
# carrying the key/value (host did NOT act). (3) NSUserDefaults persistence
# round-trip: set an option, simulate reopen (re-read defaults), assert restored.
# Links the .m files (incl. cocoametal_settings.m for the panel + persistence).
# Bounded + watchdog. Value-asserting marker [SET] PASS.
cocoametal-settings: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_settings.m hosted/cocoametal/cocoametal_control.m \
		hosted/cocoametal/cocoametal_gpu.m hosted/cocoametal/settings_test.m \
		-o build/cocoametal-settings \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	BIN=build/cocoametal-settings ./harness/run-hosted.sh '[SET] PASS'

# FS (INTERFACE.md §9 — CM_OPT_FULLSCREEN now REAL): cm_set_option(CM_OPT_FULLSCREEN,1)
# enters REAL native AppKit fullscreen via -[NSWindow toggleFullScreen:] (no longer
# a stored-flag stub); 0 exits it. Drives it under the SAME main-pthread / manual-
# CFRunLoop / NO-NSApplicationMain model as D2t and asserts PROGRAMMATICALLY (not a
# screencapture) that the window actually entered fullscreen (styleMask &
# NSWindowStyleMaskFullScreen AND window.frame == screen.frame), that the §6 oracle
# stays BYTE-EXACT across the transition (the present still composes to the now-
# fullscreen drawable), and that it exits cleanly. Documents the hand-pumped-
# transition finding (does the async toggle complete under CFRunLoopRunInMode(...,
# 0,true)? how many pumps?). Headless-safe: skips the window asserts with no window
# server but keeps the oracle assert. Links the .m files (it exercises the AppKit
# window path). Bounded + watchdog. Value-asserting marker [FS] PASS.
cocoametal-fullscreen: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_settings.m hosted/cocoametal/cocoametal_control.m \
		hosted/cocoametal/cocoametal_gpu.m hosted/cocoametal/fullscreen_test.m \
		-o build/cocoametal-fullscreen \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	BIN=build/cocoametal-fullscreen TIMEOUT=$${TIMEOUT:-30} ./harness/run-hosted.sh '[FS] PASS'

# LIVE (INTERFACE.md §2a — the live present FILLS the drawable): the readback test
# the OFFSCREEN ORACLE was blind to. A USER saw the live content as a small white
# rect in a black fullscreen window — the CAMetalLayer drawable was not resized to
# fill the content view on the fullscreen transition. This test sets the live layer
# framebufferOnly=NO, uploads the 4-quadrant + marker scene, composes it into the
# live drawable via the REAL present pass, READS THE LIVE DRAWABLE BACK, and asserts
# the scene FILLS the drawable (drawable == content-view backing size; the four
# quadrant colours land in the aspect-fit content rect; the letterbox is BLACK, never
# white) — WINDOWED and after entering fullscreen. Headless-safe (skips the live
# asserts, keeps the §6 oracle). Drives the async fullscreen toggle under the same
# main-pthread / manual-CFRunLoop model as [FS] (bounded [NSApp run]+watchdog only to
# OBSERVE the transition; prod shim stays non-blocking, §3). Links the .m files (it
# exercises the AppKit window path). Value-asserting marker [LIVE] PASS.
cocoametal-livedraw: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_settings.m hosted/cocoametal/cocoametal_control.m \
		hosted/cocoametal/cocoametal_gpu.m hosted/cocoametal/livedraw_test.m \
		-o build/cocoametal-livedraw \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	BIN=build/cocoametal-livedraw TIMEOUT=$${TIMEOUT:-30} ./harness/run-hosted.sh '[LIVE] PASS'

# SHOW (human-facing — NOT in the regression matrix): a PERSISTENT on-screen build so
# the USER can LOOK and confirm the fix. Opens the window, draws an OBVIOUS scene (4
# quadrant colours + a 1px bright border at the framebuffer edges so edge-fill is
# visible + a marker), presents CONTINUOUSLY, stays WINDOWED a few seconds, then
# ENTERS FULLSCREEN and stays so the user can SEE it fill the screen (black letterbox
# bars if the screen aspect differs, never white). Bounded: auto-exits after ~20s or
# on a keypress so it can't hang. `make cocoametal-show`.
cocoametal-show: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/cocoametal/cocoametal.m hosted/cocoametal/cocoametal_window.m \
		hosted/cocoametal/cocoametal_settings.m hosted/cocoametal/cocoametal_control.m \
		hosted/cocoametal/cocoametal_gpu.m hosted/cocoametal/show.m \
		-o build/cocoametal-show \
		-framework Metal -framework Foundation -framework CoreGraphics \
		-framework QuartzCore -framework AppKit -framework CoreFoundation
	@echo ">> running build/cocoametal-show — a window opens, then goes fullscreen; auto-exits ~20s (or press a key / Esc)"
	build/cocoametal-show

# V: the host-volume Mac glue — self-contained NFC normalization (table-driven,
# NOT CFStringNormalize), the ".<name>.amimeta" sidecar (atomic temp+rename,
# omit-when-default), and Latin-1<->UTF-8 filename charset glue, proved against
# the real macOS FS in a temp dir. Plain host clang (no framework); clean-room
# from docs/features/host-volume/spec.md. These are the functions the AROS
# emul-handler per-host overlay will call (built later by the AROS crosstools).
hosted-hostvolume: | build
	clang -O2 -Wall -Wextra hosted/hostvolume/hv_norm.c \
		hosted/hostvolume/hv_charset.c hosted/hostvolume/hv_meta.c \
		hosted/hostvolume/v_test.c -o build/host-hostvolume
	BIN=build/host-hostvolume ./harness/run-hosted.sh '[V] PASS'

# A: the CoreAudio host ring + headless/silent offline-render proof — a single-
# producer/single-consumer lock-free ring of int16 stereo PCM (AHIST_S16S) feeds
# an offline kAudioUnitSubType_GenericOutput AudioUnit whose render callback pulls
# from the ring (RT contract: memcpy + int16->float32, no locks/alloc/blocking),
# driven by AudioUnitRender to a WAV in run/. Asserts RMS + Goertzel-440Hz +
# underruns==0 over a run that wraps the ring ~108x under a real two-thread race.
# Host clang (NOT AROS crosstools); clean-room from the spec; no live device.
hosted-coreaudio: | build
	clang -arch arm64 -O2 -Wall -Wextra \
		hosted/coreaudio/coreaudio_shim.c hosted/coreaudio/a_test.c \
		-o build/host-coreaudio \
		-framework AudioToolbox -framework AudioUnit \
		-framework CoreFoundation -framework Foundation
	BIN=build/host-coreaudio ./harness/run-hosted.sh '[A] PASS'

# Deployable CoreAudio host shim: the future AROS AHI sub-driver will load this
# through hostlib.resource, peer to cocoametal.dylib/libpasteboard.dylib.
COREAUDIO_DYLIB := build/libcoreaudio.dylib
coreaudio-dylib: | build
	clang -arch arm64 -O2 -Wall -Wextra -dynamiclib \
		-install_name @rpath/libcoreaudio.dylib \
		-exported_symbols_list hosted/coreaudio/coreaudio.exports \
		hosted/coreaudio/coreaudio_shim.c -o $(COREAUDIO_DYLIB) \
		-framework AudioToolbox -framework AudioUnit \
		-framework CoreFoundation -framework Foundation
	codesign -s - -f $(COREAUDIO_DYLIB)
	@echo ">> built $(COREAUDIO_DYLIB) (exported ca_* symbols:)"
	@nm -gU $(COREAUDIO_DYLIB) | grep ' _ca_' || true

# The same numeric CoreAudio proof, but linked through the deployable dylib
# boundary instead of compiling the shim directly into the test binary.
coreaudio-abi: coreaudio-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/coreaudio hosted/coreaudio/a_test.c \
		-Lbuild -lcoreaudio -Wl,-rpath,@executable_path \
		-o build/coreaudio-abi \
		-framework AudioToolbox -framework AudioUnit \
		-framework CoreFoundation -framework Foundation
	BIN=build/coreaudio-abi ./harness/run-hosted.sh '[A] PASS'

# End-to-end AROS audio smoke: deploy the CoreAudio host dylib, boot windowed
# AROS with a short startup file, register DEVS:AudioModes/COREAUDIO, and run
# C:AHISmoke through ahi.device. The harness asserts live CoreAudio ring output
# and captures a screenshot under run/darwin-aarch64/.
audio-smoke:
	./graft/audio-smoke

# Run the in-tree AROS benchmark suite (exec + clib) on booted AROS and print the
# results. Build the binaries first from the AROS build dir:
#   make test-benchmarks-exec-quick test-benchmarks-clib-quick
# Pass benchmark names as BENCH=..., e.g. make bench BENCH="clib/dhrystone".
# See docs/features/benchmarks/README.md.
bench:
	./graft/bench-run $(BENCH)

# C: the NSPasteboard clipboard host shim — prove pasteboard text get/set, the
# changeCount change-signal source, and the ISO-8859-1<->UTF-8 transcode the bridge
# mandates, standalone (no AROS build). Uses a uniquely-named NSPasteboard so it
# never touches the user's real clipboard. Host clang (NOT AROS crosstools);
# -fobjc-arc; clean-room from clipboard-bridge/spec.md.
hosted-clipboard: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra \
		hosted/clipboard/pasteboard.m hosted/clipboard/c_test.m \
		-o build/host-clipboard \
		-framework Foundation -framework AppKit
	BIN=build/host-clipboard ./harness/run-hosted.sh '[C] PASS'

# The deployable clipboard host shim: build/libpasteboard.dylib — the artifact the
# AROS clipboard-sync task loads via hostlib.resource (peer of cocoametal.dylib).
# Exported symbols are exactly the pasteboard.h contract (pasteboard.exports); the
# binary is unstripped (dlsym by name) + ad-hoc signed so a hosted process dlopens it.
PASTEBOARD_DYLIB := build/libpasteboard.dylib
pasteboard-dylib: | build
	clang -fobjc-arc -arch arm64 -O2 -Wall -Wextra -dynamiclib \
		-install_name @rpath/libpasteboard.dylib \
		-exported_symbols_list hosted/clipboard/pasteboard.exports \
		hosted/clipboard/pasteboard.m -o $(PASTEBOARD_DYLIB) \
		-framework Foundation -framework AppKit
	codesign -s - -f $(PASTEBOARD_DYLIB)
	@echo ">> built $(PASTEBOARD_DYLIB) (exported host_pb_* symbols:)"
	@nm -gU $(PASTEBOARD_DYLIB) | grep ' _host_' || true

# dlopen-based ABI conformance for libpasteboard.dylib (the HostLib_Open boundary).
pasteboard-abi: pasteboard-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/clipboard hosted/clipboard/abi_test.c -o build/pasteboard-abi
	BIN=build/pasteboard-abi ./harness/run-hosted.sh '[PBABI] PASS'

# N: the bsdsocket host pump — non-blocking host BSD sockets + a kqueue readiness
# pump thread that converts fd-readiness into a per-target wake (the stand-in for
# an AROS Signal/WaitSelect), proved standalone against a localhost TCP echo
# server (hermetic, no entitlement, no DNS). Asserts: [N-1] non-blocking connect
# (EINPROGRESS via the pump) + send + recv round-trip; [N-2] WaitSelect-style —
# register several sockets, drive a subset, the pump reports EXACTLY the ready
# set; [N-3] a would-block recv returns EWOULDBLOCK and the pump LATER wakes when
# data arrives (no busy-spin). Host clang (NOT AROS crosstools), libSystem has
# sockets+kqueue; clean-room from docs/features/bsdsocket-net/spec.md.
hosted-bsdsocket: | build
	clang -arch arm64 -O2 -Wall -Wextra \
		hosted/bsdsocket/bsdsock_pump.c hosted/bsdsocket/bsdsock_shim.c \
		hosted/bsdsocket/bsdsock_test.c -o build/host-bsdsocket
	BIN=build/host-bsdsocket ./harness/run-hosted.sh '[N] PASS'

# The deployable bsdsocket host shim: build/libbsdsockhost.dylib — the artifact the
# AROS-side bsdsocket.library (arch/all-unix/bsdsocket) loads via hostlib.resource
# (peer of cocoametal.dylib / libpasteboard.dylib). Pump + non-blocking socket shim;
# the readiness wake is the ps_create_cb seam (the standalone proof's self-pipe wake
# becomes exec Signal(task, readySig) in the graft). Exported symbols are exactly
# bsdsock.exports; unstripped (dlsym by name) + ad-hoc signed so a hosted process
# dlopens it.
BSDSOCK_DYLIB := build/libbsdsockhost.dylib
bsdsock-dylib: | build
	clang -arch arm64 -O2 -Wall -Wextra -dynamiclib \
		-install_name @rpath/libbsdsockhost.dylib \
		-exported_symbols_list hosted/bsdsocket/bsdsock.exports \
		hosted/bsdsocket/bsdsock_pump.c hosted/bsdsocket/bsdsock_shim.c \
		hosted/bsdsocket/bsdsock_resolve.c \
		-o $(BSDSOCK_DYLIB)
	codesign -s - -f $(BSDSOCK_DYLIB)
	@echo ">> built $(BSDSOCK_DYLIB) (exported symbols:)"
	@nm -gU $(BSDSOCK_DYLIB) | grep ' _' || true

# dlopen-based ABI conformance for libbsdsockhost.dylib (the HostLib_Open boundary):
# resolve every bsdsock.exports symbol, then drive the pump through the CALLBACK seam
# (ps_create_cb) — register a socketpair read end, poke the write end, assert the
# wake callback fires and pump_drain reports the fd ready. The exact load path the
# AROS bsdsocket.library will use.
bsdsock-abi: bsdsock-dylib
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/bsdsocket hosted/bsdsocket/bsdsock_abi_test.c -o build/bsdsock-abi
	BIN=build/bsdsock-abi ./harness/run-hosted.sh '[NABI] PASS'

# The Darwin-BSD-errno -> AmiTCP-errno translation table ([N4]), unit-tested host-side
# before it lands in the AROS module (arch/all-unix/bsdsocket/errno_xlate.c). The
# load-bearing check is the one NON-identity case the explicit table caught: macOS
# EOPNOTSUPP==102 vs AmiTCP EOPNOTSUPP==45 (the rest of the BSD socket range is
# identity but asserted entry by entry). Pure int->int, no sockets.
bsdsock-errno: | build
	clang -arch arm64 -O2 -Wall -Wextra \
		-Ihosted/bsdsocket hosted/bsdsocket/errno_test.c hosted/bsdsocket/errno_xlate.c \
		-o build/bsdsock-errno
	BIN=build/bsdsock-errno ./harness/run-hosted.sh '[NERR] PASS'

# H8: a tiny exec.library via the real AROS LVO mechanism — JumpVec table built by
# MakeLibrary, indirect LVO dispatch, SetFunction hot-patch. Data-pointer vectors,
# so no Apple-Silicon W^X / MAP_JIT wall.
hosted-library: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/library.c -o build/host-library
	BIN=build/host-library ./harness/run-hosted.sh '[H8] hosted exec.library ok'

# H9: exec Wait()/Signal() — tasks that genuinely block on TS_WAIT/TaskWait and
# wake via Signal. Producer/consumer ping-pong + a free-runner proof.
hosted-signal: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/signal.c -o build/host-signal
	BIN=build/host-signal ./harness/run-hosted.sh '[H9] hosted exec Wait/Signal ok'

# H10: exec message ports — PutMsg/WaitPort/GetMsg/ReplyMsg on Wait/Signal. The
# canonical client/server request-reply (device-I/O) loop, hosted.
hosted-msgport: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/msgport.c -o build/host-msgport
	BIN=build/host-msgport ./harness/run-hosted.sh '[H10] hosted exec message ports ok'

# H11: a device backed by a real macOS file — AROS exec I/O (DoIO/IORequest) over
# the message ports drives pread/pwrite on a real file. The "macOS owns the
# drivers" thesis, end to end.
hosted-device: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/device.c -o build/host-device
	BIN=build/host-device ./harness/run-hosted.sh '[H11] hosted device ok'

# H12 (capstone): exec.library boot — the full exec reached through the LVO hub.
# Services (AllocMem/FreeMem/Signal/Wait/AddTask) live in the jump-vector table
# below SysBase; tasks call them through the base, scheduled preemptively.
hosted-execboot: | build
	clang -arch arm64 -O2 -Wall -Wextra hosted/execboot.c -o build/host-execboot
	BIN=build/host-execboot ./harness/run-hosted.sh '[H12] exec.library boot ok'

# J1: the W^X-aware MAP_JIT executable-memory layer — the substrate the adapted
# Emu68 emitter ([J2]) and the native LoadSeg path both sit on. mmap(MAP_JIT) +
# pthread_jit_write_protect_np(0/1) + sys_icache_invalidate; a hand-assembled
# AArch64 stub (movz w0,#imm; ret) is written, finalized, and CALLED, asserting
# the exact returned constant (stale I-cache = wrong value, not a crash).
#
# Entitlement/codesign, RESOLVED empirically on this Mac (macOS 26.5, Apple silicon):
#   * The default linker AD-HOC signature (NO hardened runtime) already permits
#     MAP_JIT — so the plain build below PASSES with NO codesign step.
#   * Under the HARDENED RUNTIME (codesign -o runtime, as graft/bootrun.sh uses),
#     mmap(MAP_JIT) fails EINVAL UNLESS com.apple.security.cs.allow-jit is granted.
#     `make hosted-jit68k-hardened` reproduces that resolved incantation.
hosted-jit68k: | build
	clang -arch arm64 -O2 -Wall -Wextra \
		hosted/jit68k/jit_region.c hosted/jit68k/j1_test.c -o build/host-jit68k
	BIN=build/host-jit68k ./harness/run-hosted.sh '[J1] PASS'

# Same binary under the hardened runtime + the allow-jit entitlement — the exact
# signing the AROS bootstrap needs once it adopts -o runtime (R-JIT-ENTITLE).
hosted-jit68k-hardened: | build
	clang -arch arm64 -O2 -Wall -Wextra \
		hosted/jit68k/jit_region.c hosted/jit68k/j1_test.c -o build/host-jit68k-hardened
	codesign -s - -f -o runtime \
		--entitlements hosted/jit68k/jit68k.entitlements.plist build/host-jit68k-hardened
	BIN=build/host-jit68k-hardened ./harness/run-hosted.sh '[J1] PASS'

# J2: prove the ADOPTED Emu68 AArch64 emitter (the verbatim, MPL-quarantined
# hosted/jit68k/emu68/A64.h) runs HOSTED in our [J1] MAP_JIT region. The glue
# (j2_build.c, OURS) hand-decodes a fixed register-only 68k block
#   moveq #10,d0 ; moveq #7,d1 ; add.l d1,d0 ; rts   (expect d0=17)
# into AArch64 with Emu68's encoders, writes it into the MAP_JIT cache, and runs
# it under W^X. The result (full d0..d7/a0..a7/ccr/pc register file) is asserted
# BIT-IDENTICAL to a from-scratch INDEPENDENT 68k interpreter (j2_interp.c, OURS —
# NOT Emu68's own decode). Validates the [J0] bet that Emu68's emitter separates
# cleanly from its bare-metal runtime. The Emu68 emitter is #included from the
# quarantine dir (-Ihosted/jit68k/emu68); no Emu68 source is copied into our files.
#
# Build/sign reuses the [J1] path (plain ad-hoc already permits MAP_JIT on this Mac).
hosted-jit68k-j2: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 \
		hosted/jit68k/jit_region.c hosted/jit68k/j2_build.c \
		hosted/jit68k/j2_interp.c hosted/jit68k/j2_test.c -o build/host-jit68k-j2
	BIN=build/host-jit68k-j2 ./harness/run-hosted.sh '[J2] PASS'

# J3: prove the 68k -> native LVO-call bridge (the integration boundary). Three
# grounded parts, all value-asserting:
#   (1) vector recognition: compute the 68k jump-target  libbase - n*6  via the REAL
#       negative-offset rule (__AROS_GETJUMPVEC, LIB_VECTSIZE==6;
#       arch/m68k-all/include/aros/cpu.h:82,81) and assert it round-trips to n;
#   (2) the marshaller: three native AArch64 stubs declared with the REAL register
#       macros AROS_LHA/AROS_UFHA (libcall.h:1586 / asmcall.h:822), each with a
#       DIFFERENT 68k register map (D0 ; D0+A0 ; A1+D1+D2). A reverse-H3 marshal
#       thunk is EMITTED via the adopted Emu68 emitter (emu68/A64.h) into the [J1]
#       MAP_JIT region: it reads the source 68k registers from a struct M68KState,
#       places them in AAPCS64 x0..x7, blr's the stub, and stores the return in d0;
#   (3) verify: each stub records the exact args it saw; PASS only if every
#       function's args AND its 68k-d0 return are exact.
# Reuses the [J1] build/sign path (plain ad-hoc already permits MAP_JIT on this Mac).
# The Emu68 emitter is #included from the quarantine dir (-Ihosted/jit68k/emu68);
# no Emu68 source is copied into our files.
hosted-jit68k-j3: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 \
		hosted/jit68k/jit_region.c hosted/jit68k/j3_vector.c \
		hosted/jit68k/j3_marshal.c hosted/jit68k/j3_test.c -o build/host-jit68k-j3
	BIN=build/host-jit68k-j3 ./harness/run-hosted.sh '[J3] PASS'

# J4: prove the load -> relocate -> place-in-sandbox -> translate -> run -> return
# chain end-to-end for a REAL (hand-assembled) big-endian 68k hunk binary whose
# entry code uses only the register-only opcodes the [J2] path handles (moveq/rts).
#   * Minimal hunk loader (OURS, j4_loader.c): parse HUNK_HEADER/CODE/DATA/RELOC32/END,
#     allocate each hunk in a 32-bit sandbox, apply HUNK_RELOC32 EXACTLY as the real
#     AROS loader rom/dos/internalloadseg_aos.c:292-332 (read BE32 at offset, add the
#     target hunk's sandbox base, write BE32). Hunk types from doshunks.h.
#   * The test binary has a HUNK_CODE (moveq #42,d0 ; rts) + a HUNK_DATA pointer slot
#     + a HUNK_RELOC32 that patches it into the CODE hunk -> relocation is exercised.
#   * The entry is translated through the [J2] Emu68-emitter path (emu68/A64.h) into
#     the [J1] MAP_JIT region and RUN under W^X, returning the 68k d0.
#   * Value-asserts (PASS iff BOTH): (a) the relocated DATA pointer == CODE_base +
#     addend, byte-exact big-endian; (b) the executed entry returns d0 == 42. A
#     negative control (skip relocation -> raw addend) proves the relocation assert
#     bites. Watchdog 10 s -> FAIL.
# DEFERRED to [J5]: the full Emu68 decoder + register-allocator lift (memory ops,
# branches, real jsr-through-vector) + the pointer/sandbox boundary for memory ops
# and library calls from the running program. This spike proves the loader/relocator/
# sandbox/run pipeline, not a rich CPU.
# Reuses the [J1] build/sign path (plain ad-hoc already permits MAP_JIT on this Mac).
# The Emu68 emitter is #included from the quarantine dir (-Ihosted/jit68k/emu68);
# no Emu68 source is copied into our files.
hosted-jit68k-j4: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 \
		hosted/jit68k/jit_region.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/j4_build.c hosted/jit68k/j4_test.c -o build/host-jit68k-j4
	BIN=build/host-jit68k-j4 ./harness/run-hosted.sh '[J4] PASS'

# J5a: translate a small block that TOUCHES MEMORY through the sandbox-pointer
# boundary, verified against an INDEPENDENT reference. A scoped increment of the
# [J5] decoder/RA mountain — load/store + the sandbox boundary only.
#   * Block: move.l (a0),d0 ; addq #1,d0 ; move.l d0,(a0) ; rts  (load via A0, +1,
#     store back). Loaded from a REAL big-endian hunk binary via the [J4] loader
#     (reused), placed in a 32-bit sandbox.
#   * EA decode + memory access are HAND-ROLLED (j5a_build.c, OURS) around the
#     adopted Emu68 EMITTER (emu68/A64.h): Emu68's M68k_EA.c + RegisterAllocator64.c
#     do NOT lift incrementally for a hosted sandbox — they assume An is a host
#     pointer (1:1 MMU, no sandbox base), read every ext word through the ICACHE
#     software cache (cache.c), and keep SR/CTX in EL0 system registers. The
#     [J5a] adoption finding is documented in j5a_jit68k.h.
#   * Sandbox-pointer boundary: each access maps An -> host = (host_mem-origin)+An
#     (UXTW add), BOUNDS-CHECKS An (single unsigned (An-origin)>u(size-4) compare,
#     clean fault on out-of-range — no host OOB), and byteswaps big-endian (REV).
#   * Value-asserts (PASS iff ALL): JITed registers == the independent interpreter's
#     (j5a_interp.c, OURS, no Emu68); JITed sandbox MEMORY == the interpreter's,
#     byte-exact big-endian; d0 == value+1 and the stored longword == value+1.
#   * Negative controls (each must bite): skip the store (memory unchanged);
#     wrong-endianness (no REV -> diverges); out-of-range A0 (must fault cleanly).
#     Watchdog 10 s -> FAIL.
# DEFERRED to [J5b]: branches/loops, full opcode + addressing-mode coverage, OUR
# register allocator around the emitter, real jsr-through-vector decode from a
# stream, library calls from the running program, and a sandbox-backed allocator
# for return-pointers outside the sandbox.
# Reuses the [J1] build/sign path (plain ad-hoc already permits MAP_JIT on this Mac).
# The Emu68 emitter is #included from the quarantine dir (-Ihosted/jit68k/emu68);
# no Emu68 source is copied into our files.
hosted-jit68k-j5a: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 \
		hosted/jit68k/jit_region.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/j5a_build.c hosted/jit68k/j5a_interp.c \
		hosted/jit68k/j5a_test.c -o build/host-jit68k-j5a
	BIN=build/host-jit68k-j5a ./harness/run-hosted.sh '[J5a] PASS'

# J5b: translate a self-contained 68k LOOP — a real conditional BACKWARD branch with
# genuine condition codes — in a SINGLE jit_region, verified against an INDEPENDENT
# reference. A scoped increment of the [J5] decoder/RA mountain: control flow only.
#   * Loop: moveq #0,d0 ; moveq #5,d1 ; L: add.l d1,d0 ; subq.l #1,d1 ; bne.s L ; rts
#     (sums 5+4+3+2+1 = 15 into d0 over 5 iterations, d1=0). Loaded from a REAL
#     big-endian hunk binary via the [J4] loader (reused), placed in a 32-bit sandbox.
#   * REAL condition codes: subq.l #1,d1 is emitted as `subs w_d1,w_d1,#1` (the
#     FLAG-SETTING subtract, emu68/A64.h); the bne.s is an AArch64 `b.ne` (A64_CC_NE,
#     Z==0) consuming the NZCV the subs just produced. The full 68k CCR (N/Z/V/C/X) is
#     ALSO recomputed into state->ccr with NON-flag-setting ops (cset/orr/str) emitted
#     between the subs and the b.ne, so the branch still sees the subs flags. (68k
#     subtract C = borrow = AArch64 carry-CLEAR; derived explicitly.)
#   * SINGLE-REGION internal backward branch: the whole loop is emitted once; the
#     loop top is a recorded output-word index and the b.ne offset is (target-word -
#     bne-word) — NEGATIVE. No cross-region chaining (deferred to [J5c]).
#   * EA decode / branch / CCR are HAND-ROLLED (j5b_build.c, OURS) around the adopted
#     Emu68 EMITTER (emu68/A64.h) — Emu68's M68k_EA.c + RegisterAllocator64.c do NOT
#     lift incrementally (the [J5a] finding); NO NEW Emu68 file vendored, so the
#     Exhibit-B check is unchanged. Only existing A64.h encoders are used.
#   * Value-asserts (PASS iff ALL): JITed registers == the independent interpreter's
#     (j5b_interp.c, OURS, no Emu68, with the real subtract flag rule); JITed CCR ==
#     the interpreter's full N/Z/V/C/X; d0==15 && d1==0 && Z set; the loop ran exactly
#     5 iterations and terminated.
#   * Negative control (must bite): emit the backward branch as ALWAYS-taken (broken Z
#     test) -> the loop never terminates; run in a forked child with its own 2s alarm
#     and assert the child HUNG (was killed by the alarm). Main watchdog 10s -> FAIL.
# DEFERRED to [J5c]: cross-region block chaining + an instruction cache, full Bcc/DBcc
# condition coverage, forward branches across blocks, real jsr-through-vector decode
# from a stream, library calls from the running program, and OUR register allocator.
# Reuses the [J1] build/sign path (plain ad-hoc already permits MAP_JIT on this Mac).
# The Emu68 emitter is #included from the quarantine dir (-Ihosted/jit68k/emu68);
# no Emu68 source is copied into our files.
hosted-jit68k-j5b: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 \
		hosted/jit68k/jit_region.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/j5b_build.c hosted/jit68k/j5b_interp.c \
		hosted/jit68k/j5b_test.c -o build/host-jit68k-j5b
	BIN=build/host-jit68k-j5b ./harness/run-hosted.sh '[J5b] PASS'

# J5c: RE-HOST Emu68's REAL per-opcode decoders + register allocator (NOT hand-rolled),
# to prove broad opcode coverage is reachable by ADOPTING Emu68's decode logic. The
# verbatim, MPL-quarantined emu68/M68k_LINE{8,9,B,C,D}.c + M68k_MOVE.c + M68k_MULDIV.c +
# M68k_EA.c are DRIVEN (via the line dispatch) to translate a richer block than the
# hand-rolled [J2]..[J5b] path can reach:
#   moveq #-5,d2 ; add.l d2,d0 ; sub.l d3,d0 ; and.l d4,d0 ; or.l d5,d0 ; eor.l d6,d1 ;
#   muls.w d7,d1 ; cmp.l d1,d0 ; and.l d2,d2 ; rts   (8 opcodes, 6 real LINE decoders).
# This required PROVIDING hosted replacements for exactly the three [J5a] couplings:
#   HOOK 1 sandbox/EA       — surface present; the richer block is register-direct (the
#                             memory-EA modes stay blocked by EA's no-base + BE-CPU emit).
#   HOOK 2 cache_read_16    — j5c_shims.c: big-endian fetch straight from the host stream,
#                             splicing back the high 32 bits the uint32_t param truncates
#                             (a DEEPER fetch coupling: Emu68's fetch addr is 32-bit).
#   HOOK 3 RA SR/CTX        — j5c_ra.c (OURS): memory-backed CCR (ldr/str from the state
#                             struct), NOT mrs/msr TPIDR_EL0; D0..D7/A0..A7 in the Emu68 map.
# A FOURTH portability coupling: Emu68's decoders use GNU __attribute__((alias)) function
# aliases, which clang/Mach-O REJECTS — emu68_darwinize.pl (OURS) rewrites the alias chains
# into plain-C tail-call forwarders in build-dir copies, KEEPING the quarantine byte-verbatim.
#   * Value-asserts (PASS iff ALL): JITed D0..D7/A0..A7 == an INDEPENDENT from-scratch
#     interpreter (j5c_interp.c, OURS, no Emu68), byte-exact; moveq #-5 REAL sign-extend
#     (d2==0xFFFFFFFB); the final CCR Z/N == the reference's (non-trivial: N set).
#   * Negative control (must bite): corrupt the decoded opcode stream -> the REAL decoder
#     emits a different (valid) instruction -> JITed value diverges from the reference.
#     Watchdog 10s -> FAIL.
# VERDICT (printed): re-hosting Emu68's REAL decoder + RA WORKS for register/ALU/control
# opcodes; broad coverage of that class = vendor more M68k_LINE*.c + extend the oracle.
# The memory-EA modes remain blocked (edit M68k_EA.c — the [J5a] surgery).
# The darwinize step regenerates build/emu68-darwin/*.c from the quarantine on each build.
hosted-jit68k-j5c: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_MOVE M68k_MULDIV M68k_EA; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-DJ5C_EARLY_DECODER_SET=1 \
		-Wno-unused-function \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5c_build.c hosted/jit68k/j5c_interp.c hosted/jit68k/j5c_test.c \
		build/emu68-darwin/M68k_LINE8.c build/emu68-darwin/M68k_LINE9.c \
		build/emu68-darwin/M68k_LINEB.c build/emu68-darwin/M68k_LINEC.c \
		build/emu68-darwin/M68k_LINED.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		-o build/host-jit68k-j5c
	BIN=build/host-jit68k-j5c ./harness/run-hosted.sh '[J5c] PASS'

# [J5d] BROADEN the [J5c] re-hosting so the WHOLE apps68k corpus runs through the JIT.
# Where [J5c] drove the REAL Emu68 decoders for ONE straight-line register block, [J5d]
# is a little engine: a per-basic-block translator driving the REAL decoders for every
# data/ALU/move/memory opcode + OUR re-hosted dispatcher ("MainLoop") owning inter-block
# control flow, the (An)/(An)+ sandbox-memory EA, and the jsr-through-vector -> [J3]
# library bridge. It runs all four corpus programs (mul=42, fact=120, arraysum=150,
# libcall=0 + the AllocMem/PutChar/FreeMem stub log) byte-exact vs an INDEPENDENT
# from-scratch interpreter (j5d_interp.c, OURS, no Emu68).
#   * Vendors M68k_LINE5.c (addq/subq) + M68k_CC.c (EMIT_TestCondition, link-only)
#     verbatim into the quarantine (Exhibit-B re-grep clean; NOTICE updated).
#   * The (An) EA edit is the disclosed [J5a] fix applied to the BUILD-DIR copy of
#     M68k_EA.c by emu68_darwinize.pl: each (An)-class load/store site is rewritten to
#     call OUR j5d_ea_mem emitter (sandbox-base add + REV byteswap + post/pre index).
#     The QUARANTINE M68k_EA.c stays BYTE-VERBATIM (diff vs upstream empty).
#   * Reuses the [J5c] HOOK 2 fetch + HOOK 3 memory-backed RA (j5c_shims.c, j5c_ra.c)
#     and the REAL [J3] marshaller (j3_marshal.c) + vector math (j3_vector.c).
#   * Negative control (must bite): corrupt mul's add dest reg -> the REAL decoder emits
#     a different valid insn -> d0 diverges from 42. Watchdog 15s -> FAIL.
hosted-jit68k-j5d: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_MOVE.c build/emu68-darwin/M68k_MOVE.c --move-no-merge
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5n_diag.c hosted/jit68k/j5n_symbols.c \
		hosted/jit68k/j5d_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c \
		build/emu68-darwin/M68k_EA.c build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5d
	BIN=build/host-jit68k-j5d ./harness/run-hosted.sh '[J5d] PASS'

# [J5e] THE OPTIMIZE DELIVERABLE: a block-scoped register allocator. The REAL Emu68
# decoders already keep the 68k Dn/An in fixed host regs across a block (no per-op spill);
# what [J5d] did naively was bracket EVERY block with a fixed frame loading all 16 Dn/An +
# storing all 16 back unconditionally (32 state ldr/str/block). [J5e]'s RA (j5c_ra.c) tracks
# which regs are READ before written (live-in) and WRITTEN (dirty) as the decoders run, and
# the engine (j5d_engine.c) loads ONLY live-in regs in the prologue + stores back ONLY dirty
# regs in the epilogue. SPILL POLICY: every block exit (RTS / branch / the jsr->[J3] library
# bridge) stores dirty regs to the state struct BEFORE the boundary, so the memory state is
# consistent for the bridge marshal / next block. The marker `[J5e] PASS` is gated on BOTH
# the corpus staying byte-exact vs the independent interpreter AND a measured reduction in
# emitted instructions + state-memory traffic. Same sources/decoders as [J5d]; only the
# test driver differs (it reports the before/after numbers). Watchdog 15s -> FAIL.
hosted-jit68k-j5e: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5e_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c \
		build/emu68-darwin/M68k_EA.c build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5e
	BIN=build/host-jit68k-j5e ./harness/run-hosted.sh '[J5e] PASS'

# [J5f] GENERALISE the flat-PC engine into a PC-DRIVEN dispatcher with a REAL 68k RETURN
# STACK + a PC-keyed BLOCK CACHE. Where [J5d]/[J5e] ran a flat PC (rts = top-level exit,
# jsr d16(A6) = library vector, 8-bit Bcc only), [J5f] adds: nested bsr/jsr/rts that push
# and pop big-endian 68k return addresses on a7 in the SANDBOX; computed jsr(An)/jmp(An)
# whose target comes from a register; the full Bcc/BRA/BSR .B/.W/.L displacement widths;
# and a block cache so a loop body / repeatedly-called subroutine translates ONCE. The
# new subroutine program (apps68k/sumsq.s -> bin/sumsq.exe, sum of squares 1..5 = 55 via a
# `square` subroutine that nests a `mul` helper, called from a loop + once via a COMPUTED
# jsr(a0)) runs through the SAME engine that drives the REAL Emu68 per-opcode decoders. The
# marker `[J5f] PASS` is gated on the result (55), the FULL register file (incl. a7 back at
# the initial SP), AND the sandbox memory INCLUDING THE RETURN STACK all byte-exact vs the
# independent from-scratch interpreter (j5d_interp.c, OURS, no Emu68 — extended to model the
# same SP/stack/control-flow), plus the return-stack telemetry (pushes==pops, max nest depth
# 2, >=1 computed jump) and the block-cache win (translations << executions, real hits).
# Negative controls bite: a corrupt muls source reg -> wrong result; a wild computed
# jmp(a1) (a1=0, out of sandbox) -> caught cleanly (no host crash). Same engine/decoders as
# [J5d]/[J5e]; only the engine's control-flow generalisation + the test driver differ.
# Watchdog 15s -> FAIL. NO new Emu68 file vendored (the return stack / branch decode /
# computed jumps are dispatcher-level C, not emitted decoders) -> Exhibit-B unchanged/clean.
hosted-jit68k-j5f: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5f_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c \
		build/emu68-darwin/M68k_EA.c build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5f
	BIN=build/host-jit68k-j5f ./harness/run-hosted.sh '[J5f] PASS'

# [J5g] BROADEN the ISA + addressing-mode coverage toward running any self-contained 68k
# program. Where [J5d]..[J5f] drove the register/ALU/control class (LINE5/8/9/B/C/D/MOVE/
# MULDIV) over (An)/(An)+/-(An), [J5g] VENDORS three MORE real Emu68 decoders verbatim and
# drives them, plus the full M68000 addressing modes:
#   * vendors M68k_LINE0.c (immediates/bit ops: ori/andi/eori/subi/addi/cmpi/btst/...),
#     M68k_LINE4.c (misc: clr/neg/not/tst/ext/swap/lea/pea/movem/...), and M68k_LINEE.c
#     (shifts/rotates: asl/asr/lsl/lsr/rol/ror) BYTE-VERBATIM (Exhibit-B re-grep clean,
#     diff vs upstream 305f686 empty, NOTICE updated). The engine dispatcher drives
#     EMIT_line0/line4/lineE for groups 0/4/E.
#   * extends emu68_darwinize.pl's --ea-sandbox transform: in addition to the direct
#     (An)-class sites, it now rewrites the FOUR EA funnel helpers (load/store_reg_from/
#     to_addr[_offset]) so the (d16,An), (d8,An,Xn), abs.w/abs.l, (d16,PC)/(d8,PC,Xn)
#     modes all route through OUR sandbox EA (base-adjust + REV + index/scale) — the
#     quarantine M68k_EA.c stays byte-verbatim; only the build copy is patched.
#   * adds j5g_shims.c (link stubs for the un-driven LINE4 sub-ops: debug/disasm trace
#     gate + M68K_PopReturnAddress) so the verbatim files LINK.
# The demanding program (apps68k/bubsort.s -> bin/bubsort.exe, vasm -no-opt): a bubble sort
# of {17,3,42,8,99,23} via (d8,An,Xn.L) indexed memory load/store, then a checksum/mixer
# over the sorted array using the full shift/rotate + immediate (LINE0) + misc (LINE4) set,
# -> d0 = 0x00F5B9F5. Run through the REAL decoders, asserted byte-exact (the full register
# file + the sandbox memory INCLUDING the in-place sorted array) vs the independent
# from-scratch interpreter (j5d_interp.c, OURS, no Emu68 — extended to cover every new
# opcode/mode/size). Negative control: flip the sort branch -> the array sorts wrong -> the
# checksum diverges (the byte-exact assert bites). Watchdog 15s -> FAIL.
hosted-jit68k-j5g: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5g_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5g
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5g \
		./harness/run-hosted.sh '[J5g] PASS'

# [J5h] CLOSE the X-bit multi-precision chain coverage gap [J5g] deferred. A self-contained
# 64-bit-arithmetic 68k program (apps68k/mp64.s -> bin/mp64.exe, vasm -no-opt real hunk):
#   * 64-bit ADD  via add.l (low) + addx.l (high): the carry out of the low longword,
#     recorded in the 68k X bit, is consumed by the high longword's addx.l.
#   * 64-bit NEGATE via neg.l (low) + negx.l (high): X carries the borrow lo->hi.
#   -> d0 = 0x000004FC.  Run through the REAL Emu68 LINED (add/addx) + LINE4 (neg/negx)
# decoders + our dispatcher, asserted BYTE-EXACT — the full register file AND the CCR byte
# INCLUDING the X bit AND the sandbox memory — vs the independent from-scratch interpreter
# (j5d_interp.c, OURS, no Emu68; extended with addx/subx/negx + the multi-precision Z rule).
# Resolves the [J5g] deferral: the register-direct addx/subx/negx/neg/not X-bit handling was
# empirically ground against the PRM (4th ed., ADDX/SUBX/NEGX flag rows) and found byte-exact
# CORRECT real 68k in Emu68 — the deferral was conservative (the ops were un-oracled, not
# proven wrong). NO new Emu68 file is vendored: addx/subx/negx live in the already-driven,
# already-vendored LINE4/LINE9/LINED decoders. Negative control: flip addx.l back to a plain
# add.l (drop the X carry) -> the high longword is off by one -> the byte-exact assert bites.
# Watchdog 15s -> FAIL.
hosted-jit68k-j5h: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5h_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5h
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5h \
		./harness/run-hosted.sh '[J5h] PASS'

# [J5i] the 68k EXCEPTION / SR model. A real vasm-assembled hunk program (apps68k/j5i.s ->
# bin/j5i.exe, -kick1hunks for the jmp-finish RELOC32) installs 68k exception handlers in a
# sandbox vector table (the VBR stand-in @ 0x00240000) and raises three exceptions from three
# REAL causes — trap #1 (-> vector 33), divu.w #0 (-> vector 5), ILLEGAL (-> vector 4) — plus
# hand-built micro-tests for the SR+PC frame/rte resume and a bus error (out-of-sandbox jmp ->
# vector 2, the graft/cpu_aarch64.h SIGSEGV seam modeled in-band). OUR C dispatcher owns the
# exception model (Emu68's bare-metal EMIT_Exception/VBR path is a no-op stub in the re-hosted
# runtime); the body opcodes still run through the REAL Emu68 decoders. Each exception is
# asserted byte-exact (registers + CCR/SR + sandbox memory + the per-exception frame log) vs
# the independent from-scratch oracle (j5d_interp.c, OURS). Negative control: NOP the vector
# store so no handler is installed -> the value diverges (clean fault, no host crash). The
# whole existing corpus stays byte-exact ([J1]-[J5h] + apps68k green). Watchdog 15s -> FAIL.
hosted-jit68k-j5i: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5i_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5i
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5i \
		./harness/run-hosted.sh '[J5i] PASS'

# [J5j] THE CAPABILITY CAPSTONE: a SUBSTANTIAL, recognisable real 68k program through the
# JIT. A fixed-point integer Mandelbrot ASCII renderer (apps68k/mandel.s -> bin/mandel.exe,
# vasm -no-opt) — three nested loops (row x col x iterate, ~50k inner iterations), each with
# signed muls.w fixed-point multiplies + asr shifts + the full add/sub/cmp + Bcc set +
# (d16,a5) displacement-EA memory loads/stores, plus a PutChar library call per cell + a
# newline per row through the [J3] negative-offset LVO bridge. Runs through the REAL Emu68
# per-opcode decoders + OUR re-hosted PC-driven dispatcher; its PutChar OUTPUT STREAM, final
# registers, and full sandbox memory are asserted BYTE-EXACT vs the independent from-scratch
# interpreter (j5d_interp.c, OURS, no Emu68) — AND the fractal is printed so it's visible.
# The capstone surfaced + closed a real oracle coverage gap: the immediate-source ALU forms
# add.l/sub.l/cmp.l #imm,Dn (LINED/LINEB 0xD0BC/0x90BC/0xB0BC, which vasm emits under -no-opt)
# were translated correctly by the JIT (the REAL EMIT_lineD/lineB decoders handle the
# immediate EA) but were missing from the oracle; j5d_interp.c now models them and the
# byte-exact assert verifies the real decoder against the extension. Negative control: corrupt
# the escape compare's destination register so the streams diverge (the assert bites). NO new
# Emu68 file is vendored (the decoders are the already-vendored verbatim set); the change is
# entirely in OUR files (the oracle + the program + the test). Watchdog 20s -> FAIL.
hosted-jit68k-j5j: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5j_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5j
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5j \
		./harness/run-hosted.sh '[J5j] PASS'

# [J5k] CROSS-REGION BLOCK CHAINING: chain cached blocks with direct AArch64 branches past the
# C dispatcher (lazy backpatch/linking), keeping the 68k register file pinned live in host regs
# across the hop (the file spills to struct j5d_m68k_state ONLY at a dispatcher boundary). The
# chain-heavy Mandelbrot is run through the chained engine; its PutChar stream + final registers
# + full sandbox memory are asserted byte-exact vs the independent interpreter (correctness gates
# the marker), and the C-dispatcher round-trips are measured before vs after (~42k -> ~1.7k). The
# change is entirely in OUR engine (j5d_engine.c) below the frozen seam; no emu68/ file is touched,
# struct M68KState/j5d_m68k_state layout, the [J3] LVO contract, and the [J5i] exception model are
# unchanged. Negative control bites. Watchdog 20s -> FAIL.
hosted-jit68k-j5k: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5k_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5k
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5k \
		./harness/run-hosted.sh '[J5k] PASS'

# [J5l] movem (move-multiple-registers) — the opcode every compiler-generated 68k function uses
# in its prologue/epilogue (movem.l d2-d7/a2-a6,-(sp) save + movem.l (sp)+,d2-d7/a2-a6 restore),
# so it is required to run real compiled Amiga code. This DRIVES Emu68's REAL EMIT_MOVEM decoder
# (the verbatim, quarantined M68k_LINE4.c) — the predecrement REVERSED register-mask order, the
# post/pre-increment An update, the control / (d16,An) / .w forms — and routes its memory touches
# through the sandbox (j5d_ea_helpers.c j5d_movem_* helpers: host = base_adjust + An UXTW + a
# per-register big-endian REV, with pair decomposition + the pre/post-index An update preserved).
# EMIT_MOVEM does NOT use M68k_EA.c's ldr_offset(reg_An,...) sites the --ea-sandbox transform
# rewrites — it emits its own stp/str/strh/ldr/ldp/ldrsh straight off the EA-base register — so a
# SECOND darwinize pass (--movem-sandbox, on M68k_LINE4.c) rewrites exactly those movem memory
# sites to the helpers. The QUARANTINE M68k_LINE4.c stays BYTE-VERBATIM (diff vs upstream empty);
# only the build copy is patched. No new emu68/ file is vendored (LINE4 was already vendored at
# [J5g]); the Exhibit-B grep is unchanged/clean.
#   * j5l.exe: a compiler-style non-leaf subroutine `work` saves d2-d7/a2-a6 with a predecrement
#     movem, CLOBBERS them all, restores them with a postincrement movem, and returns; the caller
#     asserts every one SURVIVED (d0 == 0x7FF). Plus the control/(d16,An)/.w forms on a fixed frame.
#   * Asserted BYTE-EXACT (full register file + the WHOLE sandbox memory, incl. the saved stack
#     frame + the control frame) vs the independent from-scratch interpreter (j5d_interp.c, OURS,
#     extended to model movem: both mask orders, the An update, .w sign-extend on load).
#   * NEGATIVE CONTROL (must bite): zero the epilogue restore mask in the JIT copy -> the clobbered
#     regs leak, d0 != 0x7FF, JIT diverges from the oracle.
#   * REGRESSION: the whole corpus (mul/fact/arraysum/libcall/sumsq/bubsort/mp64/mandel) re-run
#     through the SAME (movem-edited) engine, each byte-exact. Watchdog 30s -> FAIL.
# The change is below the frozen seam (jit_region API, struct M68KState layout, the [J3] LVO
# contract, the [J5i] exception model are all UNCHANGED): it is decoder/EA coverage only.
hosted-jit68k-j5l: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5l_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5l
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5l \
		./harness/run-hosted.sh '[J5l] PASS'

# apps68k: run REAL 68k Amiga programs (vasm-assembled hunk executables) through the
# JIT. The toolchain (tools/build-vasm.sh builds vasm from source) produces the
# big-endian AmigaOS hunk binaries in apps68k/bin/ from the *.s sources; the runner
# loads each into the [J4] sandbox (with real HUNK_RELOC32 relocation) and runs ALL
# FOUR THROUGH THE [J5d] JIT ENGINE — Emu68's REAL per-opcode decoders for every ALU/
# move/memory opcode + OUR re-hosted dispatcher for inter-block control flow + the (An)
# sandbox-memory EA edit + the jsr-through-vector -> [J3] library bridge:
#   * mul.exe       -> d0 = 42   (moveq/add.l/subq.l/bne.s/rts)
#   * fact.exe      -> d0 = 120  (+ reg-to-reg move.l + cmp.l + nested loops)
#   * arraysum.exe  -> d0 = 150  (+ relocated lea DATA, add.l (a0)+ via the REAL EA decoder)
#   * libcall.exe   -> d0 = 0    (+ AllocMem/PutChar/FreeMem via jsr -off(a6) -> [J3] bridge)
# Each register file + sandbox memory is asserted byte-exact vs an INDEPENDENT from-
# scratch interpreter (j5d_interp.c, OURS, no Emu68); NO faked passes.
# Prereqs: the *.exe binaries are committed; to regenerate, run
#   apps68k/tools/build-vasm.sh && apps68k/tools/assemble.sh
# The vendored Emu68 decoders are darwinized (alias-forwarders + the (An) EA edit) into
# build/emu68-darwin/ first; the quarantine stays byte-verbatim. No Emu68 source is
# copied into our glue.
hosted-jit68k-apps: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 -Ihosted/jit68k \
		-Ihosted/jit68k/apps68k -Wno-unused-function \
		hosted/jit68k/apps68k/runner.c hosted/jit68k/apps68k/stublib.c \
		hosted/jit68k/j4_loader.c \
		hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/jit_region.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c \
		build/emu68-darwin/M68k_EA.c build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-apps
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-apps \
		./harness/run-hosted.sh '[apps68k] PASS'

# [J5m] THE CAPSTONE: a C CROSS-COMPILER on this Mac -> a 68k AmigaOS hunk executable ->
# run COMPILER-GENERATED code through the JIT. The toolchain is built FROM SOURCE by
# apps68k/tools/build-vbcc.sh: vbcc (Volker Barthelmann's portable C compiler, same author
# as vasm) + vlink (Frank Wille's linker), targeting m68k/AmigaOS hunk output. The pipeline
#   vbcc (C -> vasm-mot asm) -> vasm (asm -> vobj) -> vlink (vobj -> hunk .exe)
# compiles a self-contained C program (apps68k/j5m.c: iterative+recursive Fibonacci, a
# factorial table, an in-place bubble sort, integer printing, a 32-bit checksum returned in
# d0) + a hand-written crt0.s (entry->main->exit; PutChar LVO shim) into apps68k/bin/j5m.exe
# (committed). j5m_test.c loads it via the [J4] loader, runs it through the [J5d] JIT, and
# asserts BYTE-EXACT (regs + whole sandbox memory + the full PutChar output stream + exit
# d0) vs the independent interpreter (j5d_interp.c, OURS, no Emu68), with a negative control.
# Prereq: bin/j5m.exe is committed; to (re)build the toolchain + binary:
#   apps68k/tools/build-vasm.sh && apps68k/tools/build-vbcc.sh && apps68k/tools/compile-j5m.sh
# The darwinize transform here adds --move-no-merge (route the compiler's back-to-back
# register pushes through the sandbox-aware EA path, not Emu68's raw stp/ldp pair fast-path).
hosted-jit68k-j5m: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_MOVE.c build/emu68-darwin/M68k_MOVE.c --move-no-merge
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k/emu68 -Ihosted/jit68k \
		-Ihosted/jit68k/apps68k -Wno-unused-function \
		hosted/jit68k/j5m_test.c hosted/jit68k/apps68k/stublib.c \
		hosted/jit68k/j4_loader.c \
		hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/jit_region.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c \
		build/emu68-darwin/M68k_EA.c build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5m
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5m \
		./harness/run-hosted.sh '[J5m] PASS'

# [J5n] THE DIAGNOSTICS SUBSYSTEM: faults are never silent. ANY 68k-JIT fault produces a
# single, self-contained, shareable crash BUNDLE (a tar.gz) with everything a developer needs
# to reproduce + diagnose: a friendly README.txt, a precise MANIFEST.txt, a two-level REPORT.txt
# (the coordinate + the faulting 68k instruction + 68k regs D0-D7/A0-A7/PC/SR + host AArch64 regs
# x0-x30/sp/pc + the 68k call stack AND the native host backtrace + a flight-recorder ring), a
# reloadable core.snapshot (M68KState + the full sandbox image), program.exe + program.sha256,
# REPRODUCE.txt (the run-to #N replay command + git commit + build config), and diverge.txt when
# the differential mode caught it. A LOUD banner prints the bundle's absolute path. Plus:
#   * the DIFFERENTIAL (lockstep-vs-oracle) mode (JIT68K_DIFF=1) that traps at the first
#     instruction where the JIT diverges from the independent interpreter -> a runtime
#     mistranslation locator;
#   * deterministic REPLAY-TO-N (JIT68K_RUNTO=N): re-run the same program, break at exactly the
#     global 68k instruction number #N the crash recorded;
#   * the host-signal safety net (SIGSEGV/SIGBUS/SIGILL/SIGFPE on a sigaltstack) so a genuine
#     out-of-sandbox HOST access is caught + bundled, never a silent crash;
#   * HUNK_SYMBOL parsing (the loader skips it) into a PC->symbol map so the report names the
#     faulting function (apps68k/diagfault.exe is assembled WITH symbols for this).
# The j5n_test driver triggers each fault kind (div0/illegal/out-of-sandbox), asserts a complete
# bundle is written with the banner path + the report contents, drives the differential trap +
# replay-to-N + the host-signal net + a snapshot reload, then cleans up its bundles. Watchdog 30s.
# The whole subsystem is INTERNAL to the engine, below the frozen seam: it is wired via a
# side-channel (j5d_set_diag, mirroring j5d_set_exc_log) and reads struct M68KState read-only;
# struct layout, the jit_region API, the [J3] LVO contract, and the [J5i] exception model are all
# UNCHANGED. The diagnostics are off the hot path (chaining stays on for the corpus; diag==NULL).
# diagfault.exe/diagill.exe/diagbus.exe are committed; to regenerate:
#   apps68k/tools/build-vasm.sh && apps68k/tools/assemble.sh   (assemble.sh builds the diag* too)
hosted-jit68k-j5n: | build
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-DJ5N_GIT_COMMIT="\"$$(git rev-parse HEAD 2>/dev/null || echo unknown)\"" \
		-DJ5N_BUILD_CONFIG="\"clang -arch arm64 -O2 (hosted darwin-aarch64); emu68 darwinized\"" \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5n_diag.c hosted/jit68k/j5n_symbols.c \
		hosted/jit68k/j5n_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-j5n
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5n TIMEOUT=40 \
		./harness/run-hosted.sh '[J5n] PASS'

# [J5v] THE TRANSLATOR CONFORMANCE SUITE. One tiny program per (instruction,
# addressing-mode) pair, each run through BOTH the JIT and the interpreter
# oracle, comparing the final register file and memory. The oracle decodes
# every mode in plain C with no register allocator and no emission layer, so
# where they disagree the JIT is what is wrong.
#
# This exists because every translator bug so far was found by a real program
# dying days later, in a place unrelated to the cause. A program only exercises
# the modes its compiler happened to emit; this exercises all of them.
#
# NOT the lockstep differ: that compares at block boundaries, where the two
# engines legitimately line up at transiently different points (see run68k.c).
# Comparing the final state of a small program has no such ambiguity.
hosted-jit68k-conform: | build
	@python3 graft/gen-68k-conformance build/conform
	@for f in build/conform/*.s; do \
		hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
			-o "$${f%.s}.exe" "$$f" >/dev/null 2>&1 || \
			{ echo "conform: $$f did not assemble"; exit 1; }; \
	done
	mkdir -p build/emu68-darwin
	for f in M68k_LINE0 M68k_LINE4 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox
	clang -arch arm64 -O2 -Wall -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5n_diag.c hosted/jit68k/j5n_symbols.c \
		hosted/jit68k/j5v_conform.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c \
		-o build/host-jit68k-conform
	@build/host-jit68k-conform build/conform

# [J5o] THE 68881/68882 FPU CORE: the FIRST corpus program to use the FPU coprocessor (line-F).
# Drives Emu68's REAL EMIT_FPU decoder (the verbatim, quarantined M68k_LINEF.c) — FMOVE (reg<->
# reg, mem<->reg, format conversions .s/.d and int .l/.w/.b<->FP), FADD/FSUB/FMUL/FDIV/FSQRT/
# FABS/FNEG, FCMP/FTST — routing its FP MEMORY touches through the sandbox (j5d_ea_helpers.c
# j5d_fpu_* helpers: base-adjust + per-element byteswap) and the FP register file FP0..FP7 (mapped
# to AArch64 d8..d15) + FPCR/FPSR through the APPENDED state (struct j5d_m68k_state's [J5o] fields,
# every existing offset unchanged). j5o_test.c runs j5o.exe through the JIT AND the INDEPENDENT
# C-double oracle (j5d_interp.c) and asserts BIT-EXACT (FP0..FP7 raw bits + FPSR cc byte + the
# stored .d/.s/.l/.w/.b results + the integer regs + memory + d0), with a negative control + the
# whole corpus re-confirmed green. PRECISION MODEL: double (80-bit extended not bit-reproducible).
# The darwinize transform adds --fpu-sandbox (M68k_LINEF.c: route fldd/fstd/flds/fsts through the
# sandbox FP-mem helpers + neutralise the bare-metal Poly/cache asm) + --libm-asm-fix (math/libm.h:
# the two clang-incompatible inline-asm constraints). The quarantine files stay BYTE-VERBATIM.
# Prereq: bin/j5o.exe is committed; to regenerate:
#   apps68k/tools/build-vasm.sh && apps68k/tools/assemble.sh   (assemble.sh builds j5o too, -m68882)
hosted-jit68k-j5o: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5o_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5o
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5o TIMEOUT=40 \
		./harness/run-hosted.sh '[J5o] PASS'

# [J5p] 68881/68882 TRANSCENDENTAL + FP-UTILITY: the next FP increment after the [J5o] core.
# Drives Emu68's REAL EMIT_FPU decoder (the same verbatim M68k_LINEF.c) for the 68881
# transcendentals (FSIN/FCOS/FTAN/FASIN/FACOS/FATAN, FSINH/FCOSH/FTANH/FATANH, FETOX/FETOXM1/
# FTWOTOX/FTENTOX, FLOGN/FLOGNP1/FLOG10/FLOG2, FSINCOS) + the FP-utility ops (FINT/FINTRZ/
# FGETEXP/FGETMAN/FMOD/FREM/FSCALE). The 68881 transcendentals are implementation-defined in
# their last ULPs, so the faithful hosted realization ROUTES the decoder's transcendental
# helper sites (which bake in &sin/&cos/... at translate time and blr them) to the HOST libm,
# and the INDEPENDENT oracle (j5d_interp.c) uses the SAME host libm — so the assert is BIT-EXACT
# and verifies the TRANSLATION (decode + the right register-as-argument + the store), not a
# re-derivation of sin(). The routing is ENTIRELY in our shim (j5o_fpu_shims.c): the standard
# libm names resolve to the system libm at link time; exp10/sincos/remquo are thin wrappers
# (the host has no exp10 / 2-arg struct-returning sincos/remquo). The QUARANTINE M68k_LINEF.c
# stays BYTE-VERBATIM (the same --fpu-sandbox/--libm-asm-fix passes as [J5o]; no new edit).
# j5p_test.c runs j5p.exe through the JIT AND the oracle, asserts BIT-EXACT (FP0..FP7 raw bits +
# FPSR cc byte + the stored doubles + integer regs + memory + d0), with NaN edge cases
# (FACOS(10)/FLOGN(-1)/FATANH(10) -> NaN, setting the FPSR NAN/I bits), a negative control
# (FSIN->FCOS in the JIT copy only -> diverge), and the whole corpus + the [J5o] FP core
# re-confirmed green. PRECISION MODEL: double (80-bit extended not bit-reproducible on AArch64).
# Prereq: bin/j5p.exe is committed; to regenerate:
#   apps68k/tools/build-vasm.sh && apps68k/tools/assemble.sh   (assemble.sh builds j5p too, -m68882)
hosted-jit68k-j5p: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5p_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5p
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5p TIMEOUT=40 \
		./harness/run-hosted.sh '[J5p] PASS'

# [J5q] 68881/68882 FP CONDITIONAL CONTROL-FLOW: FBcc/FScc/FDBcc/FTRAPcc. These read the FPSR
# condition byte (N/Z/NAN — made live + verified in [J5o]/[J5p]) and branch/set/trap on the
# 68881 FP predicate. Decoded at the DISPATCHER level in C (the way integer Bcc is — j5d_engine.c),
# NOT through Emu68's bare-metal REG_PC branch funnel (FBcc/FScc emit it + the 0xfffffffe
# sentinel; FDBcc/FTRAPcc have NO decoder body at all). The FP predicate (j5q_fp_cond_taken,
# shared header, used by BOTH the dispatcher and the oracle) is OUR re-derivation of the table
# Emu68's verbatim FBcc decoder emits in AArch64, evaluated in C over {N,Z,NAN}; the IEEE
# UNORDERED (NaN) cases are the load-bearing part (ordered predicates FALSE on NaN, unordered
# TRUE). FTRAPcc routes to the [J5i] exception path (vector 7). j5q_test.c runs j5q.exe through
# the JIT AND the oracle, asserts BYTE-EXACT (integer + FP regs + FPSR cc + the whole sandbox +
# d0 == 0x000103FF) + correct control flow, with NaN ordered-vs-unordered path checks, a negative
# control (FBOR->FBUN in the JIT copy only -> diverge), and the whole corpus + the [J5o]/[J5p] FP
# programs re-confirmed green. The Emu68 quarantine stays BYTE-VERBATIM (the predicate + the
# control-flow are OURS in the dispatcher/oracle; no new darwinize pass, no new vendored file).
# Prereq: bin/j5q.exe is committed; to regenerate: apps68k/tools/assemble.sh (builds j5q, -m68882).
hosted-jit68k-j5q: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5q_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5q
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5q TIMEOUT=40 \
		./harness/run-hosted.sh '[J5q] PASS'

# [J5r] FMOVEM + FP SYSTEM-REGISTER MOVES + the 80-bit EXTENDED (.x) memory format — the last
# decoder-level FP gap before the capstone. FMOVEM.x saves/restores the FP register list to/from
# memory in the 96-bit extended format (the FP function prologue/epilogue: FMOVEM.x <list>,-(An)
# save + FMOVEM.x (An)+,<list> restore + the control-mode forms), and FMOVE/FMOVEM move FPCR/FPSR/
# FPIAR (single + multi-control-reg list). The .x conversion (j5r_double_to_x/j5r_x_to_double in
# j5d_jit68k.h, shared by the dispatcher AND the oracle) rebiases the exponent (double bias 1023
# <-> extended 16383), sets the explicit integer bit, and places the 52-bit fraction in the high
# mantissa bits — double->.x->double round-trips EXACTLY (the FP regs are double). ±0/inf/NaN
# handled. Decoded at the DISPATCHER level in C (the .x conversion + the sandbox memory + the
# reglist are OURS — Emu68's verbatim FMOVEM/FMOVE-special bodies are bare-metal: they blr the
# abort-stub Load96bit/Store96bit with a 32-bit-truncated helper address + manipulate the real
# FPCR via msr fpcr); the Emu68 quarantine stays BYTE-VERBATIM (no new vendored file, no new
# darwinize pass). j5r_test.c runs j5r.exe (an FP function whose clobberer SAVE+RESTOREs fp0-fp7
# so the caller's FP regs survive) through the JIT AND the oracle, asserts BYTE-EXACT (integer +
# FP regs + FPCR/FPSR/FPIAR + the whole sandbox incl. the .x extended bytes), with an independent
# hand-check (FP survival + the .x encoding + the sys-reg round-trips), a negative control (drop
# fp7 from the epilogue restore in the JIT copy -> diverge), and the whole corpus + the [J5o]/
# [J5p]/[J5q] FP programs re-confirmed green. SEAM: fpiar is APPENDED after fpsr (offset 152);
# fpcr==144/fpsr==148 static-asserts hold. Prereq: bin/j5r.exe committed; regen: apps68k/tools/
# assemble.sh (builds j5r, -m68882).
hosted-jit68k-j5r: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5r_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5r
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5r TIMEOUT=40 \
		./harness/run-hosted.sh '[J5r] PASS'

# [J5s] the 68881/68882 FP EXCEPTION MODEL — the FPSR exception (EXC) + accrued (AEXC) bytes,
# the FPCR exception-enable + rounding-mode/precision bytes, the FP exception traps (vectors
# 48..54), and BSUN. Same vendored FP decoder + darwinize passes as [J5r] (no new quarantine
# file, no new darwinize pass — the exception model is OURS in j5s_fpu_exc.h + the dispatcher/
# oracle, using <fenv.h> on the host). Adds j5s_test.c; everything else identical to [J5r].
hosted-jit68k-j5s: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MOVE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5s_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5s
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5s TIMEOUT=40 \
		./harness/run-hosted.sh '[J5s] PASS'

# [J5t] THE FP CAPSTONE: a vbcc-COMPILER-GENERATED 68k program doing REAL HARDWARE floating-point
# work (Newton's-method sqrt, a Taylor exp series, vector mean/variance/stddev, a sin table) run
# THROUGH the JIT byte-exact vs the independent interpreter — the FP analog of the [J5m] integer
# capstone, closing out the FPU goal ([J5o]-[J5t]).  The C `double` arithmetic is lowered by
# vbcc -cpu=68020 -fpu=68881 to LINE-F FP opcodes (FMOVE/FADD/FSUB/FMUL/FDIV/FCMP/FBcc/FMOVEM/
# fintrz), plus the hardware transcendentals FSQRT/FSIN/FETOX (tiny crt0_fp.s shims the C calls);
# every result is integer-ized (scaled, double->int via fintrz.x) and printed through the integer
# PutChar path, so the deferred FP->decimal (.p) format is NOT needed.  The toolchain's vbcc is
# rebuilt with dtgen cross=y so FP CONSTANTS are big-endian IEEE-754 (cross=n byte-swapped them).
# j5t_test.c runs j5t.exe through the JIT AND the INDEPENDENT C-double oracle (j5d_interp.c) and
# asserts BYTE-EXACT (int regs + the FP register file FP0..FP7 + FPSR/FPCR + the whole sandbox
# memory + the full PutChar output stream + exit d0), with a negative control + the corpus green.
# Same vendored FP decoder + darwinize passes as [J5o]/[J5s] (M68k_LINEF.c --fpu-sandbox, math/
# libm.h --libm-asm-fix), plus j5m's --move-no-merge (route the compiler's back-to-back register
# pushes through the sandbox-aware EA path, not Emu68's raw stp/ldp fast-path).  The quarantine
# files stay BYTE-VERBATIM; the seam is unchanged.
# Prereq: bin/j5t.exe is committed; to (re)build the toolchain + binary:
#   apps68k/tools/build-vasm.sh && apps68k/tools/build-vbcc.sh && apps68k/tools/compile-j5t.sh
hosted-jit68k-j5t: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_MOVE.c build/emu68-darwin/M68k_MOVE.c --move-no-merge
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	clang -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k -Ihosted/jit68k/emu68 \
		-Ihosted/jit68k/apps68k -Wno-unused-function -Wno-xor-used-as-pow \
		-Wno-incompatible-library-redeclaration \
		hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
		hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
		hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
		hosted/jit68k/j5t_test.c \
		hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c \
		hosted/jit68k/apps68k/stublib.c \
		build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
		build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
		build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
		build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
		build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
		build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
		build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c \
		-o build/host-jit68k-j5t
	APPS68K_DIR=hosted/jit68k/apps68k BIN=build/host-jit68k-j5t TIMEOUT=40 \
		./harness/run-hosted.sh '[J5t] PASS'

# run68k: a REAL command-line tool (NOT a test harness) that runs a self-contained 68k
# Amiga hunk executable through the JIT (CPU + FPU) from the terminal, piping the
# program's PutChar output to stdout and exiting with the program's 68k D0.  It is a
# usability WRAPPER over the existing engine — no new emulation.  Build = the FULL [J5t]
# decoder set (integer LINE0..E + the LINEF FPU + the FP shims + --move-no-merge) UNION
# the [J5n] crash-bundle diagnostics (j5n_diag.c / j5n_symbols.c + the git/build defines),
# so it runs the whole corpus (integer + hardware FP) AND bundles any fault.
#   build/run68k <program.exe>            # run a 68k hunk, output -> stdout, exit = D0
#   build/run68k --help                   # usage
# PATH: `cp build/run68k /usr/local/bin/` (or add build/ to PATH) to call it anywhere.
# [T0a] libjit68k: the SAME engine object set run68k always compiled inline, packaged
# as a static library so a second consumer (emu68k.library — see
# docs/features/68k-transparent-exec/) can link the identical engine.  The library is
# OS-policy-free: run68k.c (the CLI) and apps68k/stublib.c (the stub-OS bridge) stay
# with the front-end, so the seam is exactly the public entry points run68k.c already
# limits itself to (j4_load_hunks / stublib_* / j5d_run / j5d_set_diag / j5n_*).
#
# CONSUMERS MUST LINK WITH -Wl,-force_load,build/libjit68k.a — never a bare -ljit68k.
# The engine binds cross-object through WEAK defaults that strong members override
# (j5d_engine.c's weak EMIT_lineF vs the real one in M68k_LINEF.o; the weak [J5n]
# hooks vs j5n_diag.o).  Mach-O archives only load members that satisfy an UNDEFINED
# symbol, and a weak definition is not undefined, so lazy archive linking silently
# drops the strong overrides (first symptom: FP programs die with "decoder consumed
# 0 insns").  force_load restores exactly the monolithic-link semantics.
JIT68K_ENGINE_SRC = \
	hosted/jit68k/jit_region.c hosted/jit68k/j5c_shims.c hosted/jit68k/j5g_shims.c \
	hosted/jit68k/j5c_ra.c hosted/jit68k/j5o_fpu_shims.c \
	hosted/jit68k/j5d_engine.c hosted/jit68k/j5d_ea_helpers.c hosted/jit68k/j5d_interp.c \
	hosted/jit68k/j5n_diag.c hosted/jit68k/j5n_symbols.c \
	hosted/jit68k/j3_vector.c hosted/jit68k/j3_marshal.c hosted/jit68k/j4_loader.c
JIT68K_EMU68_SRC = \
	build/emu68-darwin/M68k_LINE0.c build/emu68-darwin/M68k_LINE4.c \
	build/emu68-darwin/M68k_LINE5.c build/emu68-darwin/M68k_LINE8.c \
	build/emu68-darwin/M68k_LINE9.c build/emu68-darwin/M68k_LINEB.c \
	build/emu68-darwin/M68k_LINEC.c build/emu68-darwin/M68k_LINED.c \
	build/emu68-darwin/M68k_LINEE.c build/emu68-darwin/M68k_MOVE.c \
	build/emu68-darwin/M68k_MULDIV.c build/emu68-darwin/M68k_EA.c \
	build/emu68-darwin/M68k_CC.c build/emu68-darwin/M68k_LINEF.c
JIT68K_CFLAGS = -arch arm64 -O2 -Wall -Wextra -Ibuild/emu68-darwin -Ihosted/jit68k \
	-Ihosted/jit68k/emu68 -Wno-unused-function -Wno-xor-used-as-pow \
	-Wno-incompatible-library-redeclaration \
	-DJ5N_GIT_COMMIT="\"$$(git rev-parse HEAD 2>/dev/null || echo unknown)\"" \
	-DJ5N_BUILD_CONFIG="\"clang -arch arm64 -O2 (hosted darwin-aarch64); emu68 darwinized\""

libjit68k: | build
	mkdir -p build/emu68-darwin build/emu68-darwin/math build/jit68k-obj
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE M68k_MULDIV M68k_CC; do \
		perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c; \
	done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE4.c build/emu68-darwin/M68k_LINE4.c --movem-sandbox
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_MOVE.c build/emu68-darwin/M68k_MOVE.c --move-no-merge
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_EA.c build/emu68-darwin/M68k_EA.c --ea-sandbox
	for f in M68k_LINE0 M68k_LINE5 M68k_LINE8 M68k_LINE9 M68k_LINEB M68k_LINEC M68k_LINED M68k_LINEE; do perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/$$f.c build/emu68-darwin/$$f.c --rmw-sandbox; done
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINE0.c build/emu68-darwin/M68k_LINE0.c --rmw-sandbox --cas-sandbox   # [STD68K] atomics: sandbox rebase + byteswap
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/math/libm.h build/emu68-darwin/math/libm.h --libm-asm-fix
	perl hosted/jit68k/emu68_darwinize.pl hosted/jit68k/emu68/M68k_LINEF.c build/emu68-darwin/M68k_LINEF.c --fpu-sandbox
	for f in $(JIT68K_ENGINE_SRC) $(JIT68K_EMU68_SRC); do \
		clang -c $(JIT68K_CFLAGS) $$f -o build/jit68k-obj/$$(basename $$f .c).o || exit 1; \
	done
	rm -f build/libjit68k.a
	ar rcs build/libjit68k.a build/jit68k-obj/*.o
	@echo ">> built build/libjit68k.a — the 68k JIT engine as a linkable library"

run68k: libjit68k
	clang $(JIT68K_CFLAGS) -Ihosted/jit68k/apps68k \
		hosted/jit68k/run68k.c hosted/jit68k/apps68k/stublib.c \
		-Wl,-force_load,build/libjit68k.a \
		-o build/run68k
	@echo ">> built build/run68k — run a 68k hunk:  build/run68k hosted/jit68k/apps68k/bin/mandel.exe"

# [T1] libemu68k.dylib: the host-side 68k execution service — the engine (libjit68k)
# behind the small quantum-run API of hosted/emu68k/emu68k_host.h, loaded by AROS's
# emu68k.library via hostlib.resource. Deployed to ~/lib by aros-ctl deploy.
# NOTE: check the EXIT STATUS of this target, never a grep of its output. A build
# that fails while a stale build/libemu68k.dylib is present looks like success to
# `make ... | grep -c error`, and the next run silently exercises the old code -
# which cost real debugging time (an exec call that was "implemented" but kept
# reporting a capability gap, because the object had never been rebuilt).
emu68k-dylib: libjit68k
	clang -dynamiclib $(JIT68K_CFLAGS) -Ihosted/jit68k/apps68k -Ihosted/emu68k \
		hosted/emu68k/emu68k_host.c hosted/emu68k/emu68k_exec.c \
		hosted/emu68k/emu68k_dos.c hosted/emu68k/emu68k_graphics.c \
		hosted/emu68k/emu68k_gadtools.c \
		hosted/emu68k/emu68k_intuition.c hosted/emu68k/emu68k_layers.c \
		hosted/emu68k/emu68k_utility.c hosted/emu68k/emu68k_cybergraphics.c \
		hosted/emu68k/emu68k_taskresource.c hosted/emu68k/emu68k_timerdevice.c \
		hosted/emu68k/scan68k.c \
		hosted/emu68k/guestlib68k.c hosted/emu68k/bridge_lab.c \
		hosted/jit68k/apps68k/stublib.c \
		-Wl,-force_load,build/libjit68k.a \
		-install_name @rpath/libemu68k.dylib \
		-o build/libemu68k.dylib
	@echo ">> built build/libemu68k.dylib"

hosted-emu68k-bridge-cap: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/bridge_lab_test.c hosted/emu68k/bridge_lab.c \
		-o build/host-emu68k-bridge-cap
	build/host-emu68k-bridge-cap

# Regina/ARexx execution fixtures. These are ordinary AROS-m68k programs, then
# converted to classic HUNK files so transparent exec exercises the same loader
# path as third-party applications. Stage 0 uses `success` as trip.rexx's
# external-command probe; Stage 2 uses the launcher to put RexxMast, RX and the
# real TurboCalc application in one guest arena before replaying its script.
hosted-emu68k-regina-fixtures: | build
	mkdir -p build/emu68k-regina
	$(M68K_AROS_GCC) hosted/emu68k/regina/success.c \
		-o build/emu68k-regina/success.elf
	$(ELF2HUNK) build/emu68k-regina/success.elf \
		build/emu68k-regina/success
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/stage2_launcher.c \
		-o build/emu68k-regina/stage2-launcher.elf
	$(ELF2HUNK) build/emu68k-regina/stage2-launcher.elf \
		build/emu68k-regina/stage2-launcher
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/echo_host.c \
		-o build/emu68k-regina/echohost.elf
	$(ELF2HUNK) build/emu68k-regina/echohost.elf \
		build/emu68k-regina/echohost
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/echo_launcher.c \
		-o build/emu68k-regina/echo-launcher.elf
	$(ELF2HUNK) build/emu68k-regina/echo-launcher.elf \
		build/emu68k-regina/echo-launcher
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/progdir_child.c \
		-o build/emu68k-regina/progdirchild.elf
	$(ELF2HUNK) build/emu68k-regina/progdirchild.elf \
		build/emu68k-regina/progdirchild
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/progdir_parent.c \
		-o build/emu68k-regina/progdir-parent.elf
	$(ELF2HUNK) build/emu68k-regina/progdir-parent.elf \
		build/emu68k-regina/progdir-parent
	$(M68K_AROS_GCC) -noposixc hosted/emu68k/regina/idle_window.c \
		-o build/emu68k-regina/idlewindow.elf
	$(ELF2HUNK) build/emu68k-regina/idlewindow.elf \
		build/emu68k-regina/idlewindow
	@echo ">> built Regina Stage 0, Stage 2, ECHO PROGDIR and IDLE m68k HUNK fixtures"

# [T2a] scan68k: the static hardware-use scanner + the diagnosis CLI. Answers
# "how would this 68k program run here, and why" without running it.
scan68k: | build
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/scan68k.c hosted/emu68k/scan68k_main.c -o build/scan68k
	@echo ">> built build/scan68k — try: build/scan68k hosted/jit68k/apps68k/bin/mandel.exe"

# [T2a] the scanner regression: every crafted case routes as designed AND no real
# program is mis-routed. Both halves matter — a wrong FULL sends a working program
# to an emulator, which is the failure the confidence grading exists to avoid.
hosted-emu68k-t2scan: scan68k
	@mkdir -p hosted/emu68k/scantests/bin; \
	for src in hosted/emu68k/scantests/*.s; do \
	  name="$$(basename "$$src" .s)"; \
	  hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
	    -o "hosted/emu68k/scantests/bin/$$name.exe" "$$src" >/dev/null || exit 1; \
	done
	@fail=0; \
	check() { got="$$(build/scan68k -q hosted/emu68k/scantests/bin/$$1.exe | cut -d' ' -f1-2)"; \
	  if [ "$$got" != "$$2" ]; then echo "  FAIL $$1: got '$$got' want '$$2'"; fail=1; \
	  else echo "  ok   $$1: $$got"; fi; }; \
	echo "== [T2a] crafted cases =="; \
	check chipbang     "FULL 2"; \
	check ciapeek      "FULL 2"; \
	check vecwrite     "FULL 2"; \
	check superviolate "FULL 2"; \
	check datadecoy    "JIT 1"; \
	check opdecoy      "JIT 0"; \
	check computedhw   "JIT 0"; \
	check color00      "JIT 1"; \
	echo "== [T2a] real programs must never be mis-routed to FULL =="; \
	for f in hosted/jit68k/apps68k/bin/*.exe hosted/jit68k/bench/bin/dhry.exe; do \
	  [ -f "$$f" ] || continue; \
	  r="$$(build/scan68k -q $$f 2>/dev/null | cut -d' ' -f1)"; \
	  if [ "$$r" != "JIT" ]; then echo "  FAIL $$(basename $$f): routed $$r"; fail=1; fi; \
	done; \
	[ $$fail -eq 0 ] || { echo "[T2a] FAIL"; exit 1; }; \
	echo "[T2a] PASS: 4 hardware-bangers routed FULL, 4 negative/hosted controls routed JIT (including the exact COLOR00 calibration sink), and every real corpus program routes JIT."

# [T2b] the runtime hardware guard: a guest touch of the Amiga hardware comes back
# as a classified routing event naming the register, not a crash. Needs the scanner
# test programs, so it depends on the scanner target that assembles them.
hosted-emu68k-t2guard: emu68k-dylib hosted-emu68k-t2scan
	clang -arch arm64 -O2 -Wall -Ihosted/emu68k hosted/emu68k/t2b_guard_test.c \
		-o build/host-emu68k-t2guard
	@out="$$(build/host-emu68k-t2guard build/libemu68k.dylib 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T2B] PASS"*) : ;; *) echo "[T2B] FAIL"; exit 1;; esac

# [T3] the native-library bootstrap: SysBase at 4 -> OpenLibrary -> calls through
# the returned base, served at the oscall seam where AROS's libraries plug in.
hosted-emu68k-t3hello: emu68k-dylib
	clang -arch arm64 -O2 -Wall -Ihosted/emu68k hosted/emu68k/t3_nativelib_test.c \
		-o build/host-emu68k-t3hello
	@out="$$(build/host-emu68k-t3hello 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T3HELLO] PASS"*) : ;; *) echo "[T3HELLO] FAIL"; exit 1;; esac

# [T3] Exec signals use the guest Task's tc_SigRecvd word.  This focused
# standalone fixture proves SetSignal has the same state as Wait and PutMsg.
hosted-emu68k-t3setsignal: emu68k-dylib
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/setsignal.exe hosted/emu68k/nativelib/setsignal.s >/dev/null
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/t3_setsignal_test.c -o build/host-emu68k-t3setsignal
	@out="$$(build/host-emu68k-t3setsignal 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T3SETSIGNAL] PASS"*) : ;; *) echo "[T3SETSIGNAL] FAIL"; exit 1;; esac

# [T3] A Workbench-launched program sees classic startup semantics in its own
# arena: pr_CLI is zero and a big-endian WBStartup is already queued on its
# embedded Process port before instruction zero.
hosted-emu68k-t3workbench: emu68k-dylib
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/workbench-startup.exe hosted/emu68k/nativelib/workbench_startup.s >/dev/null
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/t3_workbench_test.c -o build/host-emu68k-t3workbench
	@out="$$(build/host-emu68k-t3workbench 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T3WORKBENCH] PASS"*) : ;; *) echo "[T3WORKBENCH] FAIL"; exit 1;; esac

# [T3] PROGDIR: belongs to the PROGRAM, not to the process running it. One 68k
# program starts another that lives in a different drawer, and the child opens
# a file next to ITSELF. Both are contexts of one emu68k run inside one native
# process, which is the case that used to resolve PROGDIR: to the launcher's
# drawer and silently mis-start every classic application launched from a
# script. Needs a booted instance and RESTARTS it, so it is not part of the
# default gate; the negative control is proven (reverting the OS-side apply
# turns this into "could not open PROGDIR:progdirchild.data").
hosted-emu68k-progdir: hosted-emu68k-regina-fixtures
	cp build/emu68k-regina/progdir-parent $(HOME)/AROS/Shared/Regina68k/commands/
	mkdir -p $(HOME)/AROS/Shared/Regina68k/progdir
	cp build/emu68k-regina/progdirchild $(HOME)/AROS/Shared/Regina68k/progdir/
	printf 'progdir-marker\n' > $(HOME)/AROS/Shared/Regina68k/progdir/progdirchild.data
	rm -f $(HOME)/AROS/Shared/Regina68k/progdir.result
	AROS_CTL_RESTART=1 \
	AROS_CTL_STARTUP_FILE=$(HOME)/AROS/Shared/regina-progdir-startup \
	EMU68K_GUESTSIDE_LIBS="stdc.library,posixc.library" \
	EMU68K_LIBS_PATH="$(HOME)/Source/references/aros-m68k-20260804/libs:$(HOME)/AROS/Shared/Regina68k/libs" \
	graft/aros-ctl run >/dev/null 2>&1
	@for i in $$(seq 1 60); do \
	  grep -q "PASS\|FAIL" $(HOME)/AROS/Shared/Regina68k/progdir.result 2>/dev/null && break; \
	  sleep 2; done; \
	out="$$(cat $(HOME)/AROS/Shared/Regina68k/progdir.result 2>/dev/null)"; echo "$$out"; \
	case "$$out" in *"PROGDIR-PASS"*) echo "[T3PROGDIR] PASS: a child program resolved its own drawer";; \
	  *) echo "[T3PROGDIR] FAIL"; exit 1;; esac

# [T3] An idle 68k GUI program must not starve the system that feeds it. A
# window is opened, the program goes idle in Wait, and the click is sent ten
# seconds LATER - racing the idle would prove nothing. AROS schedules
# cooperatively, so a host-side sleep on this task leaves `cocoa.hidd input`
# and `input.device` READY and never run, and the click can never be produced.
# Needs a booted instance and RESTARTS it. Negative control is proven: with the
# native idle replaced by a host sleep, the task dump shows RUN with those two
# tasks READY behind it and the click never arrives.
hosted-emu68k-idle: hosted-emu68k-regina-fixtures
	cp build/emu68k-regina/idlewindow $(HOME)/AROS/Shared/Regina68k/commands/
	rm -f $(HOME)/AROS/Shared/Regina68k/idle.result
	AROS_CTL_RESTART=1 \
	AROS_CTL_STARTUP_FILE=$(HOME)/AROS/Shared/regina-idle-startup \
	EMU68K_GUESTSIDE_LIBS="stdc.library,posixc.library,fd.library" \
	EMU68K_LIBS_PATH="$(HOME)/Source/references/aros-m68k-20260804/libs" \
	graft/aros-ctl run >/dev/null 2>&1
	@for i in $$(seq 1 45); do \
	  grep -q "IDLE-READY\|FAIL" $(HOME)/AROS/Shared/Regina68k/idle.result 2>/dev/null && break; \
	  sleep 2; done; \
	grep -q "IDLE-READY" $(HOME)/AROS/Shared/Regina68k/idle.result 2>/dev/null || { \
	  echo "[T3IDLE] FAIL: the window never opened"; exit 1; }
	@sleep 10
	@graft/aros-ctl click 0 250 140 >/dev/null 2>&1; \
	for i in $$(seq 1 15); do \
	  grep -q "PASS\|FAIL" $(HOME)/AROS/Shared/Regina68k/idle.result 2>/dev/null && break; \
	  sleep 1; done; \
	out="$$(cat $(HOME)/AROS/Shared/Regina68k/idle.result 2>/dev/null)"; echo "$$out"; \
	case "$$out" in *"IDLE-PASS"*) echo "[T3IDLE] PASS: input still reached a guest that had been idle for ten seconds";; \
	  *) echo "[T3IDLE] FAIL: the instance stopped scheduling while the guest idled"; exit 1;; esac

# [T3] ReadArgs: the call every AmigaDOS CLI tool parses its arguments with.
hosted-emu68k-t3readargs: emu68k-dylib
	clang -arch arm64 -O2 -Wall -Ihosted/emu68k hosted/emu68k/t3_readargs_test.c \
		-o build/host-emu68k-t3ra
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o /tmp/ra.exe hosted/emu68k/nativelib/readargs.s >/dev/null 2>&1 || true
	@out="$$(build/host-emu68k-t3ra 'HelloArgs 42 ALL' 2>&1)"; echo "$$out"; \
	case "$$out" in *"HelloArgs"*) : ;; *) echo "[T3RA] FAIL: template did not parse"; exit 1;; esac; \
	build/host-emu68k-t3ra "" >/dev/null 2>&1; rc=$$?; \
	[ "$$rc" = "2" ] || { echo "[T3RA] FAIL: missing /A argument should fail (got $$rc)"; exit 1; }; \
	echo "[T3RA] PASS: a 68k program parsed \"FILE/A,COUNT/N,ALL/S\" through ReadArgs and read back the string it produced; a missing required argument failed the AmigaDOS way."

# [T3e] The two real Resident forms + the guest-library dispatch distinction.
# test.library has direct executable rt_Init code; autoinit.library contains two
# named four-long RTF_AUTOINIT residents covering relative and absolute function
# tables plus InitStruct. The reusable loader constructs their vector/base areas,
# then the harness calls public vectors via real jsr d16(a6). Any bridge hit fails.
hosted-emu68k-t3guestlib: libjit68k
	mkdir -p build/emu68k-nativelib
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/test.library hosted/emu68k/nativelib/testlib.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/autoinit.library hosted/emu68k/nativelib/autoinitlib.s
	clang $(JIT68K_CFLAGS) -Ihosted/emu68k \
		hosted/emu68k/t3e_guestlib_test.c hosted/emu68k/guestlib68k.c \
		-Wl,-force_load,build/libjit68k.a -o build/host-emu68k-t3guestlib
	build/host-emu68k-t3guestlib build/emu68k-nativelib/test.library \
		build/emu68k-nativelib/autoinit.library

# [T3e] The actual exec.OpenLibrary seam: a program requests the disk library
# by name, initializes and opens it, closes/expunges it, reloads it, then proves
# version rejection. EMU68K_LIBS_PATH is the host-visible LIBS: search list.
hosted-emu68k-t3guestlive: emu68k-dylib
	mkdir -p build/emu68k-nativelib
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/autoinit.library hosted/emu68k/nativelib/autoinitlib.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/test.library hosted/emu68k/nativelib/testlib.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/guestopen.exe hosted/emu68k/nativelib/guestopen.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/cyclea.library hosted/emu68k/nativelib/cyclea.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/cycleb.library hosted/emu68k/nativelib/cycleb.s
	hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/emu68k-nativelib/clone.library hosted/emu68k/nativelib/clonelib.s
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/t3e_live_test.c -o build/host-emu68k-t3guestlive
	build/host-emu68k-t3guestlive build/emu68k-nativelib/guestopen.exe

# [T3e] Real third-party chain. The copyrighted/free-noncommercial Aminet
# package remains outside the tree; the runner verifies its pinned SHA-256,
# extracts it into a temporary directory, and requires full xQuery metadata
# from xpkmaster.library -> xpkNONE.library (not just a successful Open).
hosted-emu68k-t3ereal: emu68k-dylib
	clang -arch arm64 -O2 -Wall -Wextra -Ihosted/emu68k \
		hosted/emu68k/t3e_real_test.c -o build/host-emu68k-t3ereal
	./graft/68k-xpk-query

VASM := hosted/jit68k/apps68k/.toolchain/vasmm68k_mot

# Reassemble the in-guest OS routines into their checked-in C header.
rawdofmt-blob:
	python3 graft/gen-rawdofmt-blob $(VASM) hosted/emu68k/nativelib/rawdofmt.s \
		hosted/emu68k/nativelib/rawdofmt_blob.h

# [T3] The end-to-end test: a REAL 68k program doing its real job. LhA
# compresses, lists and extracts, and the extracted bytes must equal the
# original. Exercises the bridge, the generated crossings, in-guest RawDoFmt,
# PC-relative addressing and the FileInfoBlock conversion at once. Skips
# cleanly when no LhA binary is present (it is third-party, not checked in).
hosted-emu68k-t3lha:
	@./graft/68k-lha-roundtrip

hosted-emu68k-t3legacy:
	@./graft/68k-legacy-suite

# [BRIDGE LAB] Runtime CONTRACTS, checked from a real run's trace rather than
# from the program's own output: a fixture can print PASS while violating an
# invariant nobody looked at. The trace is the evidence and the report is the
# assertion.
hosted-emu68k-t3event:
	@mkdir -p build/t3event
	@for f in genport genproc genidcmp genframeyield; do \
		hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym \
			-kick1hunks -o build/t3event/$$f \
			hosted/emu68k/nativelib/$$f.s >/dev/null || exit 1; \
	done
	@rm -f $(HOME)/AROS/Shared/t3event.trace.jsonl
	@EMU68K_BRIDGE_TRACE=$(HOME)/AROS/Shared/t3event.trace.jsonl \
	EMU68K_MAX_SECONDS=60 CORPUS_TIMEOUT=300 \
	./graft/68k-corpus build/t3event build/t3event-out.txt >/dev/null 2>&1
	@./graft/bridge-lab report $(HOME)/AROS/Shared/t3event.trace.jsonl \
		> build/t3event-report.json || { \
		echo "[T3EVENT] FAIL: no report from the run's trace"; exit 1; }
	@# The gate is CONSISTENCY between the two axes, which is what makes it
	@# self-maintaining: a contract the registry calls supported may never be
	@# observed violated, while one still under review may be - that is what
	@# "under review" means. Promoting a contract therefore tightens this gate
	@# automatically, with no edit here.
	@python3 -c "import json,sys; \
r=json.load(open('build/t3event-report.json')); \
bad=[(f['contract'],f['maturity'],f['observation']) for f in r['findings'] \
     if f['maturity']=='supported' and f['observation'] \
        not in ('exercised-conformant','not-exercised')]; \
print('[T3EVENT]', json.dumps({f['contract']: f['maturity']+'/'+f['observation'] \
                               for f in r['findings']})); \
print('[T3EVENT] INCONSISTENT:', bad) if bad else None; \
sys.exit(1 if bad else 0)" || { \
		echo "[T3EVENT] FAIL: a supported contract was observed violated"; \
		./graft/bridge-lab report $(HOME)/AROS/Shared/t3event.trace.jsonl --text; \
		exit 1; }
	@# Every fixture below certifies a supported runtime contract and must pass.
	@for t in T3PORT T3PROC T3IDCMP T3YIELD; do \
		grep -q "\[$$t\] PASS" build/t3event-out.txt || { \
			echo "[T3EVENT] FAIL: $$t did not pass:"; \
			cat build/t3event-out.txt; exit 1; }; \
	done
	@python3 -c "import json,sys; \
e=[json.loads(l) for l in open('$(HOME)/AROS/Shared/t3event.trace.jsonl')]; \
b=[x for x in e if x.get('event')=='event.source.bind' and x.get('kind')=='idcmp']; \
shared=[x for x in b if x.get('reason')=='ModifyIDCMP']; \
p=[x for x in e if x.get('event')=='event.pump' and x.get('matched_sources',0)>=2]; \
opened=any(x.get('reason')=='OpenWindowTagList' for x in b); \
ok=len(shared)>=2 and bool(p) and len({x.get('destination') for x in shared})==1 and opened; \
print('[T3BROKER] PASS: typed shared-port sources matched without delivery' if ok \
      else '[T3BROKER] FAIL: missing automatic window source, typed source, or matched idle pump'); \
sys.exit(0 if ok else 1)"
	@# Identities must be namespaced per run. A sweep appends several programs
	@# to one trace and a bump allocator hands each the same guest addresses,
	@# so an unnamespaced identity silently merges two programs' evidence.
	@python3 -c "import json,sys,collections; \
seen=collections.defaultdict(set); bad=[]; \
[seen[v].add(e.get('run')) for e in \
   (json.loads(l) for l in open('$(HOME)/AROS/Shared/t3event.trace.jsonl')) \
   for k,v in e.items() \
   if isinstance(v,str) and (':' in v) and k in ('port','task','owner','destination','mailbox')]; \
bad=[i for i,runs in seen.items() if len(runs)>1 or not i.startswith('r')]; \
print('[T3EVENT] identities:', len(seen), 'namespaced across', \
      len({r for rs in seen.values() for r in rs}), 'runs'); \
print('[T3EVENT] LEAKED ACROSS RUNS:', bad) if bad else None; \
sys.exit(1 if bad else 0)" || { \
		echo "[T3EVENT] FAIL: an identity was reused across runs"; exit 1; }
	@python3 -c "import json,sys; \
r=json.load(open('build/t3event-report.json')); \
sys.exit(0) if r.get('events',0) > 0 and r.get('programs') else sys.exit(1)" || { \
		echo '[T3EVENT] FAIL: the recorder produced no run.start, so an empty'; \
		echo '  trace and a run with no events cannot be told apart'; exit 1; }
	@echo "[T3EVENT] PASS: each contract's observed behaviour is consistent with what the registry claims for it, focused fixtures certify the port, process and IDCMP-binding mechanisms independently, and no identity is shared between two runs of one trace."

# Regenerate the m68k-vs-native structure layouts from the AROS headers.
struct-layouts:
	python3 graft/gen-struct-layouts --emit \
		$(AROS_SRC)/arch/all-darwin/libs/emu68k/emu68k_layouts.h \
		--emit-offsets hosted/emu68k/emu68k_guest_offsets.h
	@cp hosted/emu68k/emu68k_guest_offsets.h \
		$(AROS_SRC)/arch/all-darwin/libs/emu68k/emu68k_guest_offsets.h

# [T3] exec.RawDoFmt, which runs IN THE GUEST because it calls the program's own
# PutChProc once per character. Checks the blob has not drifted from its
# assembly, then proves it in-OS with a 68k program that formats through a real
# callback and compares the bytes it got back.
hosted-emu68k-t3fmt:
	python3 graft/gen-rawdofmt-blob $(VASM) hosted/emu68k/nativelib/rawdofmt.s \
		hosted/emu68k/nativelib/rawdofmt_blob.h --check
	@mkdir -p build/t3fmt && rm -f build/t3fmt/*
	@$(VASM) -Fhunkexe -nosym -kick1hunks -o build/t3fmt/fmttest \
		hosted/emu68k/nativelib/fmttest.s >/dev/null
	@EMU68K_MAX_SECONDS=8 ./graft/68k-corpus build/t3fmt build/t3fmt-out.txt >/dev/null 2>&1; \
	grep -q '\[T3FMT\] PASS' build/t3fmt-out.txt || { \
		echo "[T3FMT] FAIL:"; cat build/t3fmt-out.txt; exit 1; }
	@echo "[T3FMT] PASS: a 68k program formatted through exec.RawDoFmt running in the guest, driving its OWN PutChProc callback per character, and got back the exact bytes expected (strings, signed decimals, hex, width, zero-pad, left-align, .limit)."

# [T3a] The GENERATED half of the library bridge. Two checks, because they fail
# in different ways: --check catches a generated file that drifted from the
# .conf it came from, and the 68k program proves the emitted crossings actually
# work in-OS. It has to run on booted AROS - the generated table lives in
# emu68k.library, so a host-side harness with a stub oscall would not touch it.
hosted-emu68k-t3gen:
	python3 graft/gen-struct-layouts --emit \
		$(AROS_SRC)/arch/all-darwin/libs/emu68k/emu68k_layouts.h --check
	python3 graft/gen-emu68k-bridge --check $(AROS_SRC)/arch/all-darwin/libs/emu68k/
	@# The .conf is the interface oracle and the hand-written bridge is the
	@# semantic one: a constant written from memory calls the wrong vector,
	@# and a derived crossing must never shadow a hand-written decision.
	@for handler in \
		hosted/emu68k/emu68k_exec.c \
		hosted/emu68k/emu68k_dos.c \
		hosted/emu68k/emu68k_graphics.c \
		hosted/emu68k/emu68k_intuition.c \
		hosted/emu68k/emu68k_layers.c \
		hosted/emu68k/emu68k_utility.c \
		hosted/emu68k/emu68k_cybergraphics.c \
		hosted/emu68k/emu68k_gadtools.c; do \
		python3 graft/gen-emu68k-bridge --validate-handwritten "$$handler" || exit $$?; \
	done
	@mkdir -p build/t3gen build/t3gen-gadget build/t3gen-menuitem && \
		rm -f build/t3gen/* build/t3gen-gadget/* build/t3gen-menuitem/*
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genbridge hosted/emu68k/nativelib/genbridge.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/gentagbad hosted/emu68k/nativelib/gentagbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genobject hosted/emu68k/nativelib/genobject.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genobjectbad hosted/emu68k/nativelib/genobjectbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genfacade hosted/emu68k/nativelib/genfacade.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genhook hosted/emu68k/nativelib/genhook.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genhookbad hosted/emu68k/nativelib/genhookbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genboopsi hosted/emu68k/nativelib/genboopsi.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genboopsibad hosted/emu68k/nativelib/genboopsibad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genosobjects hosted/emu68k/nativelib/genosobjects.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genosobjectsbad hosted/emu68k/nativelib/genosobjectsbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genprefs hosted/emu68k/nativelib/genprefs.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genrecord hosted/emu68k/nativelib/genrecord.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genrecordbad hosted/emu68k/nativelib/genrecordbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genlayoutbad hosted/emu68k/nativelib/genlayoutbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genrefused hosted/emu68k/nativelib/genrefused.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/gennoop hosted/emu68k/nativelib/gennoop.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genexecfull hosted/emu68k/nativelib/genexecfull.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/gendrawbad hosted/emu68k/nativelib/gendrawbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genwindow hosted/emu68k/nativelib/genwindow.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen/genwindowbad hosted/emu68k/nativelib/genwindowbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-gadget/gengadget hosted/emu68k/nativelib/gengadget.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-gadget/gengadgetbad hosted/emu68k/nativelib/gengadgetbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-menuitem/genmenuitem hosted/emu68k/nativelib/genmenuitem.s >/dev/null
	@mkdir -p build/t3proc
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3proc/genproc hosted/emu68k/nativelib/genproc.s >/dev/null
	@mkdir -p build/t3port
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3port/genport hosted/emu68k/nativelib/genport.s >/dev/null
	@mkdir -p build/t3gen-owngad build/t3gen-owngadbad build/t3gen-owngadcyc
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-owngad/genowngadget hosted/emu68k/nativelib/genowngadget.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-owngadbad/genowngadgetbad hosted/emu68k/nativelib/genowngadgetbad.s >/dev/null
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3gen-owngadcyc/genowngadgetcycle hosted/emu68k/nativelib/genowngadgetcycle.s >/dev/null
	@CORPUS_BEFORE='Assign LOCALE: SYS:Locale' \
	CORPUS_ORACLE='C:CatalogProbe\nC:PrefsProbe' EMU68K_MAX_SECONDS=20 \
	./graft/68k-corpus build/t3gen build/t3gen-out.txt >/dev/null 2>&1; \
	grep -q '\[T3GEN\] PASS' build/t3gen-out.txt || { \
		echo "[T3GEN] FAIL:"; cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3CAT-NATIVE\].*NONNULL' build/t3gen-out.txt || { \
		echo "[T3CAT-NATIVE] FAIL: exact native OpenCatalogA oracle did not open the installed catalog:"; \
		cat build/t3gen-out.txt; exit 1; }
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3gen-gadget build/t3gen-gadget-out.txt >/dev/null 2>&1
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3gen-menuitem build/t3gen-menuitem-out.txt >/dev/null 2>&1
	@grep -q 'unknown tag 8fffffff.*graphics.best_mode' build/t3gen-out.txt || { \
		echo "[T3TAG] FAIL: unknown tag was not reported by domain:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3TAG\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3OBJ\] PASS' build/t3gen-out.txt || { \
		echo "[T3OBJ] FAIL: typed native object roundtrip did not pass:"; \
		cat build/t3gen-out.txt; exit 1; }
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3gen-owngad build/t3gen-owngad-out.txt >/dev/null 2>&1
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3gen-owngadbad build/t3gen-owngadbad-out.txt >/dev/null 2>&1
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3gen-owngadcyc build/t3gen-owngadcyc-out.txt >/dev/null 2>&1
	@EMU68K_MAX_SECONDS=20 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3port build/t3port-out.txt >/dev/null 2>&1
	@EMU68K_MAX_SECONDS=30 CORPUS_TIMEOUT=120 \
	./graft/68k-corpus build/t3proc build/t3proc-out.txt >/dev/null 2>&1
	@grep -q '\[T3PROC\] PASS' build/t3proc-out.txt || { \
		echo "[T3PROC] FAIL: two 68k processes did not exchange a message:"; \
		cat build/t3proc-out.txt; exit 1; }
	@grep -q '\[T3PORT\] PASS' build/t3port-out.txt || { \
		echo "[T3PORT] FAIL: a message port the program owns did not work:"; \
		cat build/t3port-out.txt; exit 1; }
	@grep -q '\[T3OWNGAD\] PASS' build/t3gen-owngad-out.txt || { \
		echo "[T3OWNGAD] FAIL: a Gadget family the program owns did not cross:"; \
		cat build/t3gen-owngad-out.txt; exit 1; }
	@grep -q 'Image at .*outside guest memory' build/t3gen-owngadbad-out.txt || { \
		echo "[T3OWNGADBAD] FAIL: an Image pointer set AFTER mirror creation was not revalidated and refused:"; \
		cat build/t3gen-owngadbad-out.txt; exit 1; }
	@grep -q 'exceeds .* members or contains a cycle' build/t3gen-owngadcyc-out.txt || { \
		echo "[T3OWNGADCYC] FAIL: a cyclic family was not refused:"; \
		cat build/t3gen-owngadcyc-out.txt; exit 1; }
	@grep -q 'stale or unknown Locale object token .*memory the program owns' build/t3gen-out.txt || { \
		echo "[T3OBJ] FAIL: guest-owned memory was accepted as a native Locale facade:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3OBJ-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3FACADE\] PASS' build/t3gen-out.txt || { \
		echo "[T3FACADE] FAIL: generated DiskObject facade was not guest-readable or failed closed:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3HOOK\] PASS' build/t3gen-out.txt || { \
		echo "[T3HOOK] FAIL: native utility.library did not re-enter the guest Hook ABI:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q 'Hook entry at 00000000' build/t3gen-out.txt || { \
		echo "[T3HOOK] FAIL: invalid Hook entry was not rejected at the boundary:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3HOOK-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3BOOPSI\] PASS' build/t3gen-out.txt || { \
		echo "[T3BOOPSI] FAIL: native NewObjectA did not re-enter the guest IClass dispatcher ABI:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q 'BOOPSI dispatcher at 00000000' build/t3gen-out.txt || { \
		echo "[T3BOOPSI] FAIL: invalid IClass dispatcher was not rejected at the boundary:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3BOOPSI-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3OSOBJ\] PASS' build/t3gen-out.txt || { \
		echo "[T3OSOBJ] FAIL: semaphore/Region bridge or guest MsgPort path failed:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q 'stale or unknown Region object token' build/t3gen-out.txt || { \
		echo "[T3OSOBJ] FAIL: disposed Region handle was not rejected:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3OSOBJ-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3PREF\] PASS' build/t3gen-out.txt || { \
		echo "[T3PREF] FAIL: size-limited struct Preferences crossing failed:"; \
		cat build/t3gen-out.txt; exit 1; }
	@g="$$(sed -n 's/.*\[T3PREF-GUEST\] //p' build/t3gen-out.txt | tr -d '\r')"; \
	n="$$(sed -n 's/.*\[T3PREF-NATIVE\] //p' build/t3gen-out.txt | tr -d '\r')"; \
	[ -n "$$g" ] && [ "$$g" = "$$n" ] || { \
		echo "[T3PREF] FAIL: the guest's Preferences do not match the native oracle's."; \
		echo "  guest:  $$g"; echo "  native: $$n"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3RECORD\] PASS' build/t3gen-out.txt || { \
		echo "[T3RECORD] FAIL: terminated NewMenu records did not cross and free cleanly:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q 'CreateMenusA.newmenu\[0\].nm_Type uses an unsupported variant' build/t3gen-out.txt || { \
		echo "[T3RECORD] FAIL: image-valued NewMenu record was not refused precisely:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3RECORD-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q 'tag GTMN_Checkmark in gadtools.layout_menus needs object policy' build/t3gen-out.txt || { \
		echo "[T3LAYOUT] FAIL: Image-valued layout tag was not refused precisely:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3LAYOUT-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q 'gadtools.library.GT_FilterIMsg refused: filter returns an embedded or allocated mutable message' build/t3gen-out.txt || { \
		echo "[T3REFUSED] FAIL: reviewed function refusal was not reported precisely:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3REFUSED\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3NOOP\] PASS' build/t3gen-out.txt || { \
		echo "[T3NOOP] FAIL: source-proven no-op read an argument or called native code:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3NOOP\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3EXECFULL\] PASS' build/t3gen-out.txt || { \
		echo "[T3EXECFULL] FAIL:"; cat build/t3gen-out.txt; exit 1; }
	@grep -q 'stale or unknown RastPort object token 12345678' build/t3gen-out.txt || { \
		echo "[T3DRAW] FAIL: invalid embedded RastPort facade was not rejected:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3DRAW-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3WINDOW\] PASS' build/t3gen-out.txt || { \
		echo "[T3WINDOW] FAIL: Window facade or GadTools refresh lifecycle failed:"; \
		cat build/t3gen-out.txt; exit 1; }
	@grep -q 'stale or unknown Window object token 12345678' build/t3gen-out.txt || { \
		echo "[T3WINDOW] FAIL: invalid Window token was not rejected:"; \
		cat build/t3gen-out.txt; exit 1; }
	@! grep -q '\[T3WINDOW-BAD\] FAIL' build/t3gen-out.txt || { cat build/t3gen-out.txt; exit 1; }
	@grep -q '\[T3GADGET\] PASS' build/t3gen-gadget-out.txt || { \
		echo "[T3GADGET] FAIL: linked Gadget/NewGadget lifecycle did not pass:"; \
		cat build/t3gen-gadget-out.txt; exit 1; }
	@grep -q 'stale or unknown Gadget object token' build/t3gen-gadget-out.txt || { \
		echo "[T3GADGET] FAIL: freeing the family did not invalidate its member token:"; \
		cat build/t3gen-gadget-out.txt; exit 1; }
	@! grep -q '\[T3GADGET-BAD\] FAIL' build/t3gen-gadget-out.txt || { cat build/t3gen-gadget-out.txt; exit 1; }
	@grep -q '\[T3MENUITEM\] PASS' build/t3gen-menuitem-out.txt || { \
		echo "[T3MENUITEM] FAIL: Menu.FirstItem facade did not cross LayoutMenuItemsA:"; \
		cat build/t3gen-menuitem-out.txt; exit 1; }
	@echo "[T3GEN] PASS: generated values, tags, typed objects, callbacks and terminated record arrays ran in AROS; ambiguous variants, unknown tags, stale tokens and invalid callback addresses failed closed."

# [T3MUI] An external 68k Zune class, not one compiled into muimaster.  This is
# deliberately a separate breadth gate: it needs the m68k AROS build tree, and
# its category/program/program corpus layout carries Busy.mcc beside the test's
# PROGDIR.  The class opens more than sixteen distinct native dependencies,
# making this a regression proof for dynamically allocated library facades too.
hosted-emu68k-t3mui: emu68k-dylib
	$(MAKE) -C $(M68K_AROS_BUILD) workbench-classes-zune-busy
	@mkdir -p build/t3mui-suite/zune/genmui/Zune build/t3mui-results
	@hosted/jit68k/apps68k/.toolchain/vasmm68k_mot -Fhunkexe -nosym -kick1hunks \
		-o build/t3mui-suite/zune/genmui/genmui \
		hosted/emu68k/nativelib/genmui.s >/dev/null
	@rm -f build/t3mui-suite/zune/genmui/Zune/Busy.mcc
	@$(ELF2HUNK) \
		$(M68K_AROS_BUILD)/bin/amiga-m68k/AROS/Classes/Zune/Busy.mcc \
		build/t3mui-suite/zune/genmui/Zune/Busy.mcc >/dev/null
	@EMU68K_GUESTSIDE_LIBS='muimaster.library,stdc.library,posixc.library,fd.library' \
	EMU68K_LIBS_PATH=$(M68K_LIBS_PATH) \
	EMU68K_MAX_SECONDS=60 CORPUS_TIMEOUT=180 \
	./graft/68k-corpus build/t3mui-suite build/t3mui-results/external.txt >/dev/null 2>&1
	@grep -q '\[T3MUI\] PASS' build/t3mui-results/external.txt || { \
		echo "[T3MUI] FAIL:"; cat build/t3mui-results/external.txt; exit 1; }
	@echo "[T3MUI] PASS: guest muimaster loaded, created and disposed the external 68k Busy.mcc class through its guest dispatcher."

# [T1] host-side smoke of the dylib API (quantum runs, sink, kill) before it goes in-OS.
hosted-emu68k-t1dyl: emu68k-dylib
	clang -arch arm64 -O2 -Wall -Ihosted/emu68k hosted/emu68k/t1_dylib_test.c \
		-o build/host-emu68k-t1dyl
	@out="$$(build/host-emu68k-t1dyl build/libemu68k.dylib 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T1DYL] PASS"*) : ;; *) echo "[T1DYL] FAIL"; exit 1;; esac

# [T0-P1] the guest-address / loader-representation proof for transparent 68k
# execution (docs/features/68k-transparent-exec/plan.md): loads two REAL hunk
# binaries into 32-bit guest arenas with guest-address relocation, builds the
# native proxy seglist (the shape DOS registers/walks), identifies via the modeled
# GetSegListInfo, runs both through the full JIT from the proxy alone, unloads
# leak-free.  Second consumer of libjit68k (validates the [T0a] seam too).
hosted-emu68k-t0p1: libjit68k
	clang $(JIT68K_CFLAGS) -Ihosted/jit68k/apps68k \
		hosted/emu68k/t0p1_seglist.c hosted/jit68k/apps68k/stublib.c \
		-Wl,-force_load,build/libjit68k.a \
		-o build/host-emu68k-t0p1
	@out="$$(build/host-emu68k-t0p1)"; echo "$$out"; \
	case "$$out" in *"[T0P1] PASS"*) : ;; *) echo "[T0P1] FAIL"; exit 1;; esac

# [T0-P3] engine instances + safe points + fault containment (docs/features/
# 68k-transparent-exec/plan.md): two interleaved instances on one thread, two
# sequential same-process runs, a chained infinite loop killed from a signal
# handler, a translated-code SIGSEGV contained to a clean error.
hosted-emu68k-t0p3: libjit68k
	clang $(JIT68K_CFLAGS) -Ihosted/jit68k/apps68k \
		hosted/emu68k/t0p3_engine.c hosted/jit68k/apps68k/stublib.c \
		-Wl,-force_load,build/libjit68k.a \
		-o build/host-emu68k-t0p3
	@out="$$(build/host-emu68k-t0p3 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T0P3] PASS"*) : ;; *) echo "[T0P3] FAIL"; exit 1;; esac

# [T0-P4] the marshalling-schema spike (docs/features/68k-transparent-exec/plan.md):
# the annotation vocabulary + descriptor-driven marshaller proven on the five
# representative cases (buffer+length, shadow struct, handle, callback hook, taglist).
hosted-emu68k-t0p4: libjit68k
	clang $(JIT68K_CFLAGS) \
		hosted/emu68k/t0p4_marshal.c \
		-Wl,-force_load,build/libjit68k.a \
		-o build/host-emu68k-t0p4
	@out="$$(build/host-emu68k-t0p4 2>&1)"; echo "$$out"; \
	case "$$out" in *"[T0P4] PASS"*) : ;; *) echo "[T0P4] FAIL"; exit 1;; esac

# [args] run68k CLI argument passing (AmigaDOS convention): build run68k, run the args
# demo (apps68k/bin/args.exe, compiled with crt0_args.s) with a FIXED arg vector, and
# assert the program PRINTED the passed args byte-exact. Proves run68k delivers the args
# (A0 = the arg string, D0 = its length incl '\n') into the running 68k program, the
# crt0_args.s splitter builds argv[], and main(argc,argv) saw them. Also re-checks a
# no-args-reading program (mandel) still exits 0 (A0/D0 setup is backward-compatible).
hosted-jit68k-args: run68k
	@echo "== [args] run68k hello world 42 =="
	@out="$$(build/run68k hosted/jit68k/apps68k/bin/args.exe hello world 42 2>/dev/null)"; \
	exp="$$(printf 'argc=4\nargv[0]=a.out\nargv[1]=hello\nargv[2]=world\nargv[3]=42\n')"; \
	echo "$$out"; \
	[ "$$out" = "$$exp" ] || { echo "[args] FAIL: output mismatch"; exit 1; }
	@echo "== [args] no args (empty AmigaDOS string, D0=1) =="
	@out="$$(build/run68k hosted/jit68k/apps68k/bin/args.exe 2>/dev/null)"; \
	exp="$$(printf 'argc=1\nargv[0]=a.out\n')"; \
	echo "$$out"; \
	[ "$$out" = "$$exp" ] || { echo "[args] FAIL: no-args output mismatch"; exit 1; }
	@echo "== [args] backward-compat: a no-args-reading program (mandel) still exits 0 =="
	@build/run68k hosted/jit68k/apps68k/bin/mandel.exe >/dev/null 2>&1; \
	rc=$$?; [ $$rc -eq 0 ] || { echo "[args] FAIL: mandel exit $$rc (expected 0)"; exit 1; }
	@echo "[args] PASS: run68k delivered the AmigaDOS CLI args (A0=string, D0=len incl newline)"
	@echo "[args]       into the 68k program; crt0_args.s split them into argv[]; the no-args"
	@echo "[args]       case and the backward-compat corpus (mandel) are unaffected."

# [rust68k] REAL RUST on the 68k JIT: run the COMMITTED no_std core+alloc corpus
# binaries (hosted/jit68k/rust68k/bin, rebuilt from src by rust68k/tools/build-rust68k.sh)
# under BOTH engines — the JIT and the independent reference interpreter (--interp) —
# and assert the exits agree with the expected values. vecsum_inclusive is the CCR
# CANARY: its correct answer is 91, but the open upstream LLVM M68k bug (MOVE clobbers
# CCR — rust68k/UPSTREAM-LLVM-CCR-BUG.md) makes it return 1 on any correct 68k; when
# this row starts failing with 91/91, the upstream fix has landed — flip the expectation.
hosted-jit68k-rust: run68k
	@pass=1; \
	printf 'fib.exe 55\nallocprobe.exe 10\nvecsum.exe 91\nvecsum_inclusive.exe 1\n' | \
	while read exe want; do \
	  build/run68k          hosted/jit68k/rust68k/bin/$$exe >/dev/null 2>&1; j=$$?; \
	  build/run68k --interp hosted/jit68k/rust68k/bin/$$exe >/dev/null 2>&1; i=$$?; \
	  if [ "$$j" = "$$want" ] && [ "$$i" = "$$want" ]; then \
	    echo "  $$exe: jit=$$j interp=$$i (want $$want) ok"; \
	  else \
	    echo "  $$exe: jit=$$j interp=$$i (want $$want) MISMATCH"; exit 1; \
	  fi; \
	done || { echo "[rust68k] FAIL"; exit 1; }
	@out="$$(build/run68k hosted/jit68k/rust68k/bin/hello.exe 2>/dev/null)"; \
	outi="$$(build/run68k --interp hosted/jit68k/rust68k/bin/hello.exe 2>/dev/null)"; \
	exp="$$(printf 'Hello from Rust on 68k AROS!\nfib(10) = 55\n6 * 7 = 42\n')"; \
	if [ "$$out" = "$$exp" ] && [ "$$outi" = "$$exp" ]; then \
	  echo "  hello.exe: PutChar stream byte-exact under both engines"; \
	else \
	  echo "  hello.exe: OUTPUT MISMATCH"; echo "[rust68k] FAIL"; exit 1; \
	fi
	@echo "[rust68k] PASS: Rust (no_std core+alloc, m68k-unknown-none-elf) hunk binaries ran"
	@echo "[rust68k]       byte-agreeing through BOTH engines (JIT + interpreter), incl. the CCR canary"

# Phase-2 regression matrix: build + run every hosted spike, assert each marker.
hosted-test:
	./harness/test-hosted.sh

# Re-ground the hardware map against the ACTUAL machine: dump + decode the DTB
# this exact QEMU/flags combination emits. Source of truth for HARDWARE.md.
dtb: | build
	qemu-system-aarch64 -machine virt,dumpdtb=build/virt.dtb -cpu cortex-a72 -display none
	dtc -I dtb -O dts build/virt.dtb -o build/virt.dts 2>/dev/null
	@echo ">> decoded device tree -> build/virt.dts"

# Product-release entry points. These consume the already prepared hosted AROS
# tree; see RELEASE.md for provenance, smoke, signing, and notarization gates.
.PHONY: macaros-compatibility macaros-release macaros-release-check macaros-dmg
macaros-compatibility:
	./graft/macaros-compatibility.sh

macaros-release:
	./graft/make-aros-release.sh

macaros-release-check:
	./graft/make-aros-release.sh --check

macaros-dmg:
	./graft/make-aros-release.sh --dmg

clean:
	rm -rf run build
