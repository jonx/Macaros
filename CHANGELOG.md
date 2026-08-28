# Changelog

## 2026-08-28 - Every setting does something

- **The Settings window now changes the machine, not just a file.** Choices that
  were stored and never read are wired up: quitting asks first, the Dock icon
  can be turned off, recordings use the frame rate and codec you picked, and
  Capture Input is a real grab with the release combination you chose (it can
  also turn itself on when you go full screen).
- **The keyboard layout applies while AROS runs.** Picking a layout switches it
  immediately instead of waiting for the next launch. It was worse than that
  before: the window wrote `0` for every layout, so the choice never arrived at
  all.
- **Settings changed outside the app are picked up.** The config file is shared
  with the command-line tools, so a disk shared with `aros-ctl media`, an edit
  made by hand, or a change from another Macaros is noticed, applied, and shown
  in the window while everything keeps running.
- **File ▸ Open Folder as Volume… works.** Choosing a Mac folder mounts it in
  the running system straight away. The setting for the *shared* folder is
  marked "next launch", because a mounted volume cannot be moved out from under
  open files, and the window no longer pretends otherwise.

## 2026-08-28 - Mount a real USB stick inside Macaros

- **Macaros can now mount a physical Mac disk, and you choose which one.** An
  exFAT (or FAT) stick, SD card, or attached disk image can be handed to AROS,
  where it appears as an ordinary volume. Files written on the Mac read inside
  AROS, and files AROS writes come back byte-exact on the Mac, with the volume
  still passing `fsck_exfat`.
- **Settings has a Media tab.** It lists the removable media the Mac has, never
  its own disk, and gives each one a choice of not shared, read only, or read &
  write. The list follows the hardware: plug a stick in while the window is open
  and it appears. Sharing takes effect immediately, without a reboot, and a disk
  you stop sharing goes back to the Finder within a second. The same choices are
  available from the command line with `aros-ctl media`.
- Sharing a disk unmounts it on the Mac first, so the two systems never write one
  filesystem at once. A read-only share really is read-only: AROS cannot write
  to the disk at all. A shared disk is remembered by the volume itself, not by
  the port it was in, so replugging it elsewhere still works.
- A disk AROS may not write now says so: it shows as `read only` in `Info`, and
  an attempt to change anything on it comes back as "disk is write-protected"
  instead of failing further down with a vaguer error. That covers a stick with
  its write-protect switch on as well as a read-only share.
- The route is `hostdisk.device`, which needed three fixes to work on macOS at
  all: the darwin build was compiling a placeholder backend, its geometry code
  did not compile and mis-sized disks over 2 TB, and it gave up instead of
  falling back to read-only when the Mac allowed reading but not writing.

## 2026-08-25 - exFAT proves itself on real USB hardware

- **The exFAT handler passed its last acceptance gate: a physical USB stick
  on a Raspberry Pi 4B.** The stick mounts read/write as `FATX` on hotplug
  and every test payload reads back byte-exact. The failure that blocked
  this since early August was a one-line handler-routing bug: a masked
  `FAT` table entry shadowed the `FATX` entry, so correctly identified
  exFAT partitions were handed to the FAT handler and reported as
  "Not a DOS disk". A new on-screen `EXFATBootRegionProbe` diagnostic
  (in `hosted/exfat-tests/`) proved the device data byte-exact first,
  which pinned the fault to discovery and made the fix a certainty
  instead of a guess.

## 0.2.1 - 2026-08-16

- **Legacy 68k support is complete in the delivery image.** The 0.2.0 package
  contained `emu68k.library` but omitted its arm64 host engine. The image now
  includes and audits both halves. A missing engine produces a useful message
  instead of making every 68k program look invalid; the classic 68k LhA command
  has been exercised through archive creation, listing, and extraction.
- **Clipboard sharing works when launched from the disk image.** `CLIPS:` now
  uses a writable RAM-backed directory instead of the sealed AROS system volume.
  Edit -> Paste and Command-V work without developer files or configuration.
- **Compatibility reports stay local and bounded.** The Help menu opens the
  report folder and issue tracker, while session, task, call, and JIT traces have
  fixed retention and size ceilings so diagnostics cannot consume the disk.
- **Release identity has one source.** The root `VERSION` file now supplies the
  bundle, disk image, manifest, and About window. About reports 0.2.1 build 6;
  release builds fail when their generated metadata disagrees.
- **The signed image now passes its own boot gate.** The delivery DMG and outer
  application bundle are Developer ID signed. The hosted AROS engine and bridge
  libraries retain ad-hoc signatures without hardened runtime because enabling
  it currently stops signal-context task switching during bootstrap. This is an
  explicit interim limitation. The image is therefore Developer ID signed but
  not Apple-notarized, and publication requires a boot from the exact signed
  disk image.

## 2026-08-15 - Macaros gets its own release home

- **The product now lives in `jonx/Macaros`.** The graft, documentation,
  harness, hosted bridges, QEMU AArch64 bring-up, and their working history have
  moved out of AROS-AArch64. The complete modified operating-system source
  remains in the `aarch64-darwin-graft` branch of `jonx/AROS`.
- **The delivery image is auditable before it is signed.** It includes a local
  compatibility checker, release notes, source revisions and artifact hashes,
  complete licence material, and explicit checks for Zed, Ferail, transparent
  68k execution, and exFAT. Moonstone is absent by default and remains a private
  opt-in build.
- **The hosted ABI is stated as part of the product.** Macaros is a place to
  test our own AROS and application-platform concepts; a bare-metal Apple
  Silicon version is not planned. Developers must reserve Apple platform
  register `x18`, because Darwin signal delivery cannot preserve a guest value
  there.

## 2026-08-08 - A buffer overflow in the 68k translator

- **68k programs can use fonts they loaded themselves.** Creative software of
  the era often ships its own bitmap fonts and loads them directly rather than
  asking the system for them. The font then belongs to the program, in the
  program's memory, and the system's drawing routines could not read it, so
  every such program stopped at its first line of text. Those fonts now work:
  the font is converted once when the program first hands it over, and the
  program keeps seeing its own font exactly where it put it.

- **68k applications launched from the desktop get their startup message.**
  Double-clicking an icon starts a program differently from typing its name: the
  desktop hands the program a message describing what it was started on, and
  waits to get it back. A 68k program never received that message, and worse,
  it stayed in the way and was picked up by the program's first disk operation,
  which ended the program with "unexpected DOS packet received". Imagine 4, for
  example, ran fine from the shell and died from its icon. The program now
  receives the message, starts in the drawer it was launched from, and can read
  the icons it was dropped on.

- **Large 68k programs no longer corrupt memory as they are translated.** The
  translator built each block of AArch64 code in a scratch buffer eight times
  larger than the buffer it then copied the result into, and checked only the
  scratch one. Any program with a big enough basic block quietly wrote past the
  end of a stack buffer; what you saw afterwards was arbitrary, from a
  misleading error message to a crash with no relation to the real cause. The
  translator now bounds itself against the buffer it actually writes to, and a
  block too large to fit is a clear refusal instead of silent damage. This had
  been happening for some time and only became visible when a system library
  update turned on the overflow check that caught it, so it is worth suspecting
  behind unexplained failures before this date.

## 2026-08-07 - The window frame belongs to the Mac, the inside to AROS

- **Clicking the window's title bar no longer reaches AROS.** A right-click on
  the Macaros title bar used to pop the AROS menus, because every click on the
  window was handed to the guest no matter where it landed. Mouse input now
  only reaches AROS when it lands on the AROS screen itself; the title bar and
  the rest of the window frame behave like any Mac window (a title-bar
  right-click shows the standard macOS menu). Drags that start inside the
  screen still work when the pointer crosses the frame, and button releases
  are never lost.

## 2026-08-06 - Classic applications get their activation event, and ARexx is proven

- **A window that opens active now says so.** The classic way to open a window
  is to create it with no IDCMP events, hang your own port on it, and switch
  events on a moment later. On real hardware the program wins that race; here
  the window was already activated by the time it asked, so the activation
  event was lost and applications waiting for it never finished starting up.
  The bridge now replays exactly that one lost activation, only for the window
  that really is active and only when none was delivered.
- **Self-extracting and overlaid programs load correctly.** A classic program
  that carries no relocation data finds the rest of itself by walking the list
  of blocks the system loaded it into, and that list also records each block's
  size, which unpackers read to know where their packed data ends. Neither the
  list nor the sizes were being provided for the program the system starts
  directly, so such a program followed a bogus link and was reported as
  needing real Amiga hardware. Both are provided now, matching what the
  original system loader writes byte for byte; a packed music program now
  unpacks itself completely and runs, and a translation test that had been
  failing for unrelated-looking reasons passes.
- **Hardware-level programs can be examined instead of only refused.** A
  diagnostic switch runs a program that drives the Amiga chips directly, with
  the chip addresses answering as inert memory. That does not make such a
  program work - sound hardware that does not exist stays silent - but it
  shows what the program is and how far it gets, which is how a mystery file
  was identified as a chip-level music player rather than something this
  layer should have served.
- **An idle 68k program no longer freezes the machine.** When a classic
  application sat waiting for you to click something, the whole system could
  stop responding: no mouse, no keyboard, no screen updates, with no way back.
  It was waiting for input while holding the CPU that the input handling itself
  needed. It now waits the same way every other part of the system waits, so
  everything keeps running while an application idles.
- **Two more system libraries run as real 68k code.** `diskfont.library`, which
  every application that picks a font goes through, now runs above the
  waterline instead of being served natively. It needed one missing piece: a
  structure the program reads back can now carry a file lock the program can
  pass straight to another call. `fd.library` joined the same set.
- **A program started by another program finds its own files.** `PROGDIR:`
  means "the drawer this program was loaded from", and classic applications
  keep their settings and data there. When one 68k program launched another,
  the child was still looking in the launcher's drawer, so it silently started
  up wrong. Each running program now carries its own drawer. TurboCalc, which
  reads `PROGDIR:TurboCalc.data` at startup, now reaches the same state
  launched from a script as it does launched by hand.
- **The ARexx path has a permanent proof.** A small purpose-built host answers
  a scripted `ping` with `PONG:ping` through the real chain: RexxMast, RX, the
  guest rexxsyslib, a published public port, and the reply. It passes, so
  scripting a 68k application is a supported route and any future breakage in
  it shows up as one failing test rather than a mystery inside an application.

## 2026-08-06 - Moonstone has a soundtrack

- **The game plays music and combat sounds.** Moonstone on AROS was silent:
  every other backend had a sound device and this one had none, so the six
  tunes shipped with the game were dead weight. It now streams its own mixer
  into AHI through the host CoreAudio driver, which also makes it the first
  application on this port to stream audio rather than play one canned sample.
- **The soundtrack ships in the release.** The 15 MB of music was excluded from
  the app bundle for as long as nothing could play it; it is now part of the
  embedded game data.
- **Sound never costs you the game.** If no audio mode is available the game
  says so in one line and plays on in silence. `Moonstone audio` plays a tune
  for five seconds and reports whether the sound kept up, and
  `MOONSTONE_AUDIO_FRAMES` trades buffering against latency for slower
  machines.

## 2026-08-05 - Real Amiga system libraries run above the waterline

- **Gadgets work fully with the real m68k gadtools.** Programs that bring
  their own font and programs that use the screen's both build gadgets now,
  the library's own gadget lists are walked correctly, and freeing a gadget
  actually frees it instead of leaking. Programs that misuse the library
  (double free, unsupported field, circular list) are still stopped and told
  exactly what was wrong.
- **A real m68k gadtools.library now runs as guest code end to end.** A test
  program creates its gadget through the guest library's own BOOPSI class,
  the class calls through to the native superclass, and attribute queries
  answer correctly. This is the waterline model working: the original
  library's behavior, byte for byte, with only the bottom system layer
  translated.
- **A crash can no longer take the whole system with it.** The 68k engine's
  fault handling had three ways to wedge the machine at 100% CPU with no
  recovery (a loop in a callback, a false stack alarm, and the crash
  reporter crashing while reporting). All three are fixed: a broken program
  now dies alone, with a report that names the exact native function that
  faulted - which is how a week-old mystery turned out to be one missing
  include line.

## 2026-08-03 - A run becomes evidence

- **Bridge Lab.** Running a classic program now produces a record of what it
  asked the system for - the processes it started, the ports it made, what it
  waited on, where input was routed - and turns that into a verdict per
  capability rather than a wall of output. It says which capabilities were used
  correctly, which are missing, and what would settle an open question. It can
  never quietly widen what the system accepts: a recording is evidence, and only
  a reviewed decision changes what gets served.
- **It immediately found a real one.** Photogenics starts a helper process that
  polls without ever pausing, so the part of the program that owns the mouse and
  menu input never gets a turn. That is a general scheduling question - a task
  that never pauses must not starve the others - and it is now recorded as such,
  with the evidence attached, rather than being a mystery about one program.

## 2026-08-03 - A 1995 commercial paint program draws its interface

- **Photogenics 1.2 runs.** It opens its screen, builds its gadgets, starts its
  helper process and draws its whole interface: the menu bar, the tool palette,
  the images panel, the canvas. No capability gap, nothing refused. It is a
  commercial Amiga application from 1995 running on an ARM Mac, translated
  instruction by instruction, calling the real AROS libraries.
- **68k programs can have more than one process.** A program that asks the
  system to start a second thread of its own code now gets one. Both are 68k and
  everything they share is memory, so they take turns rather than run at the
  same instant: a program that cannot go on hands its turn to the other one and
  picks up where it left off when the answer arrives. That removes the whole
  class of problems running them side by side would have created, and a genuine
  deadlock is reported rather than hung.
- **Message ports work.** Sending a message, taking it, and answering it - the
  shape every interactive Amiga program is written in, and there was none of it
  before. A wait that nothing could ever satisfy says so instead of hanging.
- **A program that builds its own gadgets is understood.** Classic code
  allocates its interface itself and hands it over; that memory is now mirrored
  for the library and kept in step in both directions, with anything the mirror
  cannot carry named rather than quietly dropped.
- **Programs stopped being turned away for no reason.** One instruction in a
  program's startup code used to route the whole thing to "needs a real Amiga",
  which is what had been hiding Personal Paint. The report of what this system
  can serve was also years out of date and read from a list of two libraries
  when there were eleven.

## 2026-08-03 - A 68k program can hand AROS structures it built itself

- **The window says Macaros.** The title bar, the Settings windows and the names
  of saved screenshots and recordings all read "AROS", the name of the system
  running inside, instead of the name of the app running it. The host now titles
  its own chrome from the app itself, so everything the Mac shows reads Macaros
  and the display driver no longer decides what the app is called.
- **A program's own gadgets now cross.** Classic Amiga code allocates its
  `Gadget` list itself and gives it to Intuition, which then keeps and renders
  those structures. Nothing issued a handle for them, so the bridge had nothing
  to resolve and refused. A structure the program owns is now mirrored on the
  native side under its own address, converted in and back out around every
  call: it keeps its identity across calls, a linked family crosses whole, and
  the program reads its own addresses back rather than native pointers. A field
  the mirror cannot carry is named and refused on every crossing, not trusted
  after the first one.
- **A wrong stack-bounds offset is fixed.** A 68k program that asks its own
  task where its stack is was reading two bytes off, and got a pointer to
  nowhere. Those offsets are now derived from the headers and checked against
  the AmigaOS ABI, so the whole class is gone rather than this one case.
- **A hardware verdict says where it came from.** "This program wants the Amiga
  hardware" used to give only an address. It now names the program counter and
  the register carrying it, which is the difference between a diagnosis and a
  guess.
- **The bridge importer learned four things at once**, so whole libraries move
  rather than one call at a time: a structure the program owns can cross by
  value, a pointer stored into a helper's own scratch is not a lifetime
  obligation, whether a call accepts NULL is read from the library's own source,
  and an entry the importer wrote stays derivable so later improvements reach it.
- **The 68k engine decodes `move.l table(pc),(a0)`** and its increment and
  displacement forms, which ordinary code uses to fill a structure it just
  allocated.

## 2026-08-02 - A commercial Amiga paint program gets past its first system call

- **Photogenics 1.2 asks AROS for the system preferences and gets them.** It is
  a 1995 commercial paint program, and the first real call it makes is for the
  whole `Preferences` structure: fonts, key repeat rates, the mouse pointer
  sprite, printer settings. Handing that to a 68k program means rebuilding 232
  bytes field by field, in the byte order and at the offsets a 68k program
  expects. It now gets exactly what a native AROS program gets, checked field
  for field against the same call made natively in the same boot.
- **Three kinds of field that used to be quietly skipped now cross.** Structures
  nested inside structures, arrays of numbers, and a two-byte boolean that had
  been taken for four bytes and would have overwritten the field after it. The
  layout tool now refuses to emit any field it believes is wider than the gap to
  the next one, so that class of mistake fails loudly at build time instead of
  producing plausible garbage at run time.
- **A buffer size is treated as a limit, not a suggestion.** A program that asks
  for part of a structure gets exactly that much filled and not one byte more.
- **Then it got four more calls further in.** It asks for the Workbench screen,
  clears the processor caches, loads its own plug-in library from the package,
  and does floating-point maths. All four now work: 62 more system calls are
  served, including the whole IEEE double-precision maths set, which a paint
  program cannot start without.
- **A program's own bundled libraries are found again.** The search stopped at
  the first file with a matching name, which on a running AROS is AROS's own
  native library, not the 68k one the program shipped. It now keeps looking.
- **Libraries that give every caller its own copy now work.** It is an ordinary
  Amiga pattern for a library to hand each program a private instance of itself,
  and Photogenics's plug-in library does exactly that. Two callers now get two
  working instances, each closable on its own.
- **A library search bug in AROS itself, found by a paint program.** The loader
  took the first file it managed to load and gave up if that file turned out not
  to be a library. Any program shipping its own Amiga-built libraries beside it
  therefore blocked AROS from finding its own, and this is not specific to 68k
  programs: a native AROS program in the same directory hit it too. The search
  now steps over a file with nothing to initialise and keeps looking.
- **A program that asks a question now says what it asked.** Requesters are
  answered "no", because headless there is nobody to ask, but a program that
  then tidies up and exits looked identical to one that failed for no reason.
  The first thing this revealed was Photogenics asking for a library we were not
  offering it, which is now fixed.

## 2026-08-02 - The 68k translator now checks itself against a second implementation

- **A conformance suite covers the addressing modes**, not just whole programs.
  Every translator bug so far was found by a real program dying days later,
  somewhere unrelated to the cause, because a program only exercises whatever
  its compiler happened to emit. There is now one small test per instruction and
  addressing-mode pair, each run through the translator and through an
  independent interpreter, comparing registers, flags and memory. 103 of 104
  agree exactly.
- **It found a crash on its first run.** Any 68k program that read the processor
  status register brought the whole thing down with an illegal instruction. That
  is an ordinary thing for older code to do, and it would have surfaced sooner or
  later as one more unexplained failure.
- **Where the checker cannot check, it says so.** Cases the reference
  implementation does not understand are listed by name rather than counted as
  passes. That list is now down to one.

## 2026-08-02 - A real Amiga archiver compresses and extracts on AROS/aarch64

- **LhA works.** The original 68k LhA 2.15, unmodified, compresses a file into
  an archive, lists it, and extracts it again. The archive it writes is valid
  LHA that other tools read, and the extracted file is byte-identical to the
  original. This is a 1991 Amiga binary doing its actual job on an Apple Silicon
  Mac, not a port and not a rebuild.
- **Two more programs from the test set now run**, and one was being blamed
  unfairly. AddText had been reported as needing a full Amiga emulator because
  it appeared to touch the hardware; it never did, it was being sent to a wrong
  address by the bug above. Any program previously rejected that way is worth
  retrying.
- **Structure layouts are derived from the OS, not typed in.** An AmigaOS
  structure and a native AROS one differ in byte order, pointer width and
  alignment, so anything crossing between a 68k program and AROS has to be
  rebuilt field by field. Those offsets now come out of the AROS headers
  automatically for both layouts, and the generator refuses to emit if it stops
  reproducing layouts that are known facts about the Amiga.

## 2026-08-02 - LhA runs, and a whole class of silent 68k miscompilation is fixed

- **68k programs can read their own data again.** Position-independent code, which
  is to say every Amiga executable, reads its constants and its jump tables
  relative to the program counter. That form of access was being computed from a
  register this Mac does not let us use, so it silently returned the wrong bytes.
  A program would then jump through a garbage table and execute its own data.
  This affected everything that was not compiled a particular way, and it is
  fixed; what is still unsupported now says so by name instead of reading a
  wrong address.
- **A run can no longer hang forever waiting for a dialog nobody can see.** When
  a 68k program asked for a path that could not be resolved, AmigaOS raised its
  usual "please insert volume" requester. Driven headlessly there is nobody to
  answer, and the whole run stopped dead with nothing in the log. Those calls
  now fail normally, which is what the program expects anyway.
- **LhA, the classic Amiga archiver, runs.** It prints its banner, reads its
  arguments, opens files and starts writing an archive. Directory scanning is
  the next piece it needs.
- **exec.RawDoFmt works**, which matters more than it sounds: it is the
  formatting engine behind every Amiga program's printf. It calls back into the
  program's own code once per character, so it now runs inside the emulated
  program rather than being faked from outside.

## 2026-08-02 - The 68k engine finds library calls it used to walk straight past

- **A whole class of silent failure is gone.** A 68k program does not always
  call a library the textbook way. It copies the base into another register,
  hoists the call address out of a loop, or jumps straight to it. The engine
  only recognised some of those spellings, and missing one was silent: it jumped
  into the middle of the library's data and started interpreting it as
  instructions, failing thousands of steps later at an address with no visible
  connection to the program. lha and ADocReader both died this way. Both now get
  through, and every program in the test set that used to fail this way now
  either runs or names exactly what it still needs.
- **Jump tables are understood.** The compiled form of a `switch` statement was
  not recognised as a branch at all, so the decoder ran off the end of the
  function into the table itself.
- **Twenty libraries are bridged instead of seven**, 99 calls instead of 64,
  including the Amiga floating-point libraries, which turn out to need no
  hand-written code at all. Adding another library is now one name on a list.

## 2026-08-02 - 68k programs reach more of AROS, without the calls being written by hand

- **The library bridge is now mostly generated.** When a 68k program calls an
  AmigaOS library, something has to turn its 68k registers into a real native
  AROS call. Those crossings were being written out one at a time. AROS already
  describes every library vector precisely (its C prototype and which 68k
  register each argument arrives in), so they are now *derived* from that
  description instead: 64 crossings across dos, exec, utility, intuition,
  graphics, icon and commodities, none of them hand-written. Programs get
  pattern matching, string and case handling, environment variables, file
  attributes and more, and the list grows when AROS's own does.
- **A generated crossing never overrides a considered one.** Hand-written cases
  run first; the derived table is the fallback. Where a signature is not the
  whole truth, the call is refused by name with the reason recorded rather than
  guessed at. `utility.UDivMod32` is the example worth having: its prototype
  says it returns one value, but it really returns a quotient *and* a remainder
  in a second register, so generating it would have produced silently wrong
  arithmetic with no crash to notice.
- **A guest cannot reach past its sandbox into the machine.** Several
  kernel-level exec calls look trivially simple and would have been swept in
  (rebooting, disabling interrupts, dropping into the debugger, blocking the
  task that hosts the emulator). exec is now allowlisted rather than
  denylisted, so the default for a kernel API is "no".

## 2026-08-02 - Copy and paste work in the shell again, in both directions

- **Paste into the shell works.** Right-Amiga+V (either ⌘ on a Mac keyboard,
  including the right one) puts the Mac clipboard at the prompt. It had been
  silently doing nothing in the boot console: that window opens lazily, and the
  console handler was listening on the window's message port as it stood at
  startup, before the window existed. So no menu entry in that window worked
  either, Copy, Paste, About or Close.
- **Copying out of the shell works.** Mark console text with the mouse and press
  Right-Amiga+C and the text is on the Mac clipboard. Nothing had been writing
  the AROS clipboard from the console at all, so this direction could not work
  however healthy the bridge was.
- **One keypress, one paste.** The chord used to be delivered twice, once to the
  console and once as a menu shortcut, which pasted the clipboard twice and
  copied it twice. It now has a single owner.
- **The Mac clipboard is read more broadly**, so text copied from applications
  that publish it in a less common form still crosses over, instead of the Paste
  menu offering a clip that never arrived.
- **A busy Mac clipboard no longer blocks the other direction.** An application
  that rewrites the Mac clipboard on a timer used to keep AROS-to-Mac copies
  from ever being noticed.

## 2026-08-01 - 68k programs that need the real Amiga hardware now say so

- **A program that drives the Amiga chips gets a clear answer instead of a
  crash.** Some classic software talks straight to the hardware rather than to
  the operating system, and translation cannot serve that. Those programs now
  stop with a plain sentence naming exactly what they wanted, for example the
  custom chip register `$DFF180`, rather than dying mysteriously.
- **It works even when the program hides the address.** If the hardware address
  is worked out while the program runs, so nothing can spot it by inspection
  beforehand, the system still catches the moment it is touched and gives the
  same clear answer.
- **New `scan68k` tool**: point it at a 68k program and it tells you how the
  program would run here and why, without running it. Ordinary programs are
  never mistaken for hardware-bangers.
- Still open: pointing those programs at a real emulator automatically, and
  remembering the choice per program.

## 2026-08-01 - Classic 68k Amiga programs run from the AROS shell

- **Type the name of a 68k Amiga program and it runs.** A real big-endian
  AmigaOS executable, the kind that only ever ran on a 68000, now runs on
  Apple Silicon as an ordinary AROS process: output goes to your console,
  arguments arrive the AmigaDOS way, the exit code comes back, and CTRL-C
  stops it. No emulator window, no separate machine, nothing to configure.
- **It behaves like a program, not an experiment.** Several 68k programs can
  run at once alongside native ones, a program that crashes takes only itself
  down (and leaves a crash report behind), and one that asks for an operating
  system function we have not taught it yet says exactly which one instead of
  failing mysteriously.
- The output is identical, byte for byte, to the same programs run through the
  standalone translator, including hardware floating point and a full
  Dhrystone benchmark run.
- Known limits: this covers system-friendly programs that talk to the OS.
  Programs that drive the Amiga hardware directly are not supported yet, and
  the set of OS functions available to 68k code is still small and growing.

## 2026-08-01 - Macaros 0.2: three applications, on the desktop, in one download

- **The desktop has application icons now.** Macaros boots to Wanderer with
  Zed, Ferail and Moonstone sitting on the backdrop, each with its own
  artwork, each one double-click away. They stay in `C:` as well, so the shell
  keeps working exactly as before.
- **Moonstone ships with the release.** The game and the assets it reads are
  embedded in the bundle, so it runs on a Mac that has never seen the source
  tree. Its music is left out: those files are decoded but nothing plays them
  until AROS has an audio backend for them.
- **Real icons, not the four-colour kind.** A generator turns each project's
  own artwork into a Workbench icon. AROS could already read this icon format
  but threw away the one field that says what the icon *is*, so every such
  icon was mistaken for a document; that is fixed in the OS side.
- **Two reasons an icon launch used to fail, both fixed.** A program started
  from an icon gets its stack from that icon, and the file manager wanted more
  than it was given. And a program with no shell behind it cannot answer a
  system requester, so the editor now reads the error instead of stopping on
  a dialog nobody can dismiss.
- **The editor's settings survive an update.** In the release its home is the
  shared Mac folder, not the volume inside the app bundle, which is read-only
  and replaced wholesale on every install.
- **A crashing application no longer takes Macaros with it.** Quitting the file
  manager ended the whole session: it aborts on exit, and the crash reporter
  then walked a broken frame chain, faulted inside itself, and stopped the
  host before it could say anything. The reporter now distrusts that chain,
  and the release boots with trap containment on, so a fault shows a
  recoverable alert and costs you that one program.

## 2026-07-30 - the terminal grows an interrupt key, and the editor stops guessing about files

- **Ctrl-C stops a running command in the terminal.** The Amiga break
  convention, wired end to end: the terminal consumes the keystroke and
  delivers the break signal to the shell's process, which the running command
  shares. A `Wait 60` dies in the time it takes to press the key. This also
  gives the editor's own stop-a-task buttons real meaning, and required fixing
  a small OS gap on the way: the tag that promises a new shell's CLI number
  has been declared since the nineties and implemented never.
- **The editor notices outside changes in about a second, not half a minute.**
  AROS cannot report file changes, so the editor had been walking the project
  on a timer. It now asks the Mac underneath, which does know, and is told
  about each folder as it changes. Folders on volumes not shared with the Mac
  keep the old behaviour.
- **The window title shows real punctuation.** The title bar speaks Latin-1
  and the editor speaks UTF-8, so the dash between project and filename
  arrived as three stray glyphs. Typographic characters now fold to their
  plain equivalents at the boundary.
- The phantom "threads sidebar" button is gone: it toggled a panel that does
  not exist on AROS, doing nothing except hiding itself.

## 2026-07-26 - A working terminal inside the editor, and a fix for random crashes

- **Zed's terminal panel runs a real AROS shell.** Open it, type a command, see
  its output. AROS has no pseudo-terminal, which was assumed to be a hard
  prerequisite; it turns out not to be. Two pieces were missing. AROS starts two
  kinds of shell and only asks you which if you know to ask: the default runs one
  command and exits, which is what running a command means and is useless as a
  terminal. And with no terminal device, nothing echoes what you type or moves
  the cursor to the start of the next line, so the terminal does both itself.
- Still missing without a pseudo-terminal: the shell is not told how big the
  window is, so full-screen programs have nothing to fit themselves to; there is
  no way to interrupt a running command; and there is no prompt.
- **Rust programs no longer corrupt memory at random.** A background thread was
  given a quarter of the stack space the same code gets everywhere else. AROS
  puts nothing between one task's stack and whatever is below it, so running off
  the end quietly overwrites something else, and the crash lands somewhere
  unrelated minutes later. The same overflow had been showing up as three
  different, equally misleading failures. Threads now get the usual 2 MB.
- **A failed program launch now says why.** Every failure used to come back as
  "command could not be run", whatever had actually gone wrong.
- **The terminal shows a prompt, and output appears as it is written.** Both were
  the same AROS bug: writes to a pipe were held in a buffer until it filled,
  where writes to a screen are sent after every line. So a command like `Dir`
  seemed to do nothing until you typed the next one, and the shell printed no
  prompt, having concluded from its input that nobody was there. Pipes are now
  treated as what they are.
- **The keyboard layout is applied on every boot**, not only when starting the
  desktop. A plain boot came up US whatever the layout was set to, and since
  everything reads the keyboard the same way, so did the editor.
- **The terminal opens on a bare prompt**, the way a shell should. The working
  directory used to be applied by typing a `CD` at the shell, and the editor's
  environment by typing a `Set` for each variable, so a new terminal opened on a
  stack of prompts for commands nobody had typed. AROS can give a new program its
  directory outright, and nothing on AROS reads the variables an editor sets.
- **The editor notices outside changes in about a second, not half a minute.**
  AROS has no way to report file changes, so the editor had been looking for
  them: walking the project on a timer, which is both slow and wasteful. It now
  asks the Mac underneath instead, which does know, and gets told about each
  folder as it changes. A file created on the Mac shows up in the tree in about
  two seconds where it used to take twenty to thirty. Folders on volumes that
  are not shared with the Mac keep the old behaviour.
- **Errors from file operations on worker threads now arrive at all.** The C
  runtime keeps errno per-thread for the editor's own code, but the system
  libraries it calls write a single shared cell that nothing on the thread
  side read -- so any failure they reported was invisible, which is the root
  of every "failed with no reason" symptom this week. The runtime now reads
  both places.
- **The editor now knows the filesystem is case-insensitive.** Its check
  creates a file and then the same name in uppercase, expecting "already
  exists" as the answer on a filesystem like ours. That answer was being
  mistranslated (first into nothing at all, then into "not found" by an
  incomplete translation table of ours), so the check errored and guessed
  wrong. The translation now comes from the system's own table. The wrong
  guess was mostly harmless -- it made the editor treat differently-cased
  names as different files, which the filesystem does not.
- **A file operation that fails now says why.** AROS reports failures through a
  channel that does not reliably reach the one the Rust runtime reads, so asking
  about a file that is not there came back as a failure with no reason attached,
  which a caller cannot tell apart from a broken disk. It matters most to the
  file watcher: the editor's language server rebuilds constantly, creating and
  deleting working directories as it goes, and the watcher regularly asks about
  one that has just been deleted. Told "gone" it steps over it; told nothing it
  abandoned the scan. An external edit now shows up in twenty to thirty seconds
  rather than upwards of a minute, though that is one measurement and the
  polling is still the underlying cost.
- **Changes made to a file outside the editor are picked up** after all: a file
  created on the Mac appears in the tree, and an edit to an open file reloads it.
  This was listed as missing on 2026-07-25 and was fixed, unnoticed, by the path
  handling that went in the same day. It is slow, though: a new file showed up
  within half a minute and a changed one took between half a minute and a
  minute and a half, against a two-second polling interval on a nineteen-file
  project. Why it is that much slower than it asks to be is not yet established.
  AROS cannot report a change, so the editor has to keep looking, and a way for
  it to be told instead is the real fix.

## 2026-07-25 - Programs can talk to programs they start

- A Rust program on AROS can now **run another program and talk to it while it
  runs**: read its output as it appears rather than only after it finishes, send
  it input, and be told when it exits along with its real exit code. Before this,
  starting a program meant waiting for it to end and only then reading what it
  wrote. This is what an editor needs to host a language server or a build tool
  on AROS itself, and it is the groundwork for a real terminal.
- Two things had to be worked around. AROS's shell reports every failure as the
  same generic error, so the actual exit code is now read from where the shell
  keeps it. And a program started without anything to type at it used to inherit
  the console and wait forever for input that was never coming; it now gets an
  empty input instead.


## 2026-07-25 - Zed stops freezing, and shows live diagnostics

- The editor no longer freezes after a minute of use, and **rust-analyzer errors
  now appear** as red underlines with an error count in the status bar. Both were
  the same bug: a lock library used across the Rust ecosystem has no AROS support,
  so when a thread waited for a lock it spun in a tight loop instead of sleeping.
  AROS gives threads of equal priority equal turns, so that one spinning thread
  starved every other one, including the editor's own display thread and the one
  reading the language server's replies. Threads now sleep properly on AROS.
- The editor **window can be resized** by dragging its size gadget, and the layout
  reflows with it. It asked for a size gadget but never declared how small or
  large it could get, and AROS then pins a window to the size it opened at.

## 2026-07-25 - The real Zed editor runs on AROS

- `C:Zed` is now **Zed's own binary**, not a shim built over its editor crates.
  It opens a project and shows the real thing: a file tree you can expand, editor
  tabs, breadcrumbs, syntax highlighting, a status bar, and the side panels.
- Three fixes got it from "links" to "usable". Zed's tokio runtime on AROS has no
  I/O driver, so every HTTP request (telemetry, registry fetches) panicked a
  worker thread seconds after startup and took the editor down; AROS now uses
  Zed's offline HTTP client, which the language server does not go through.
  And the file tree would not expand a single folder: every file reported the
  same file id, which the project scanner reads as a symlink loop. The Rust
  runtime on AROS now reports real file ids, which any program that identifies
  files this way needed anyway.
- Not there yet: the language server still runs on the Mac rather than on AROS.
  (External file changes *are* noticed, contrary to what this entry first said;
  see the 2026-07-27 entry.)

## 2026-07-25 - The window boots at 1366x768

- AROS now comes up at **1366x768** instead of 800x600. The desktop has to be
  idle to change resolution, so the size you boot into is the one you get to
  work in, and 800x600 was too small for a real editor window. The mode ladder
  is unchanged, so dragging the window edge still snaps through all 16 modes.

## 2026-07-25 - Zed: live language-server diagnostics

- The editor now shows **real rust-analyzer diagnostics** on AROS: open a Rust
  file and genuine errors are underlined in the code and marked in the scrollbar,
  answered by a real language server analysing the project. The server runs on
  the Mac and the editor talks to it over a socket, since AROS cannot yet run
  rust-analyzer itself.
- Two fixes made it work. File paths now translate between AROS volumes and the
  host, so the editor and the language server agree on which file is being
  discussed (before this, no server ever started). And a long-standing hazard in
  the Rust runtime's `readlink` was fixed: when the filesystem reported "buffer
  too small" for something that is not a symlink, it kept doubling its buffer
  until memory ran out and the program died. It now gives up cleanly. This one
  affected any Rust program on AROS that inspects paths, not just the editor.

## 2026-07-24 - Async networking runs on AROS

- The async networking stack (`async-io`/`mio`/`tokio`) now drives real sockets
  on hosted AROS, verified with a live async TCP round-trip. This is the
  foundation the editor's networked features sit on (HTTP, language servers over
  the network, the agent panel). Two pieces made it work: a reactor that reports
  socket readiness through `bsdsocket` `WaitSelect`, and a small unified-fd shim
  so the Rust socket crates — which drive sockets as ordinary file descriptors —
  work against AROS, where sockets and files live in separate descriptor spaces.
  See [docs/features/zed-editor/os-requirements.md](docs/features/zed-editor/os-requirements.md).

## 2026-07-24 - Pipes for live tools (PIPE: + readiness)

- `PIPE:` is now mounted on every boot, and its handler gained what an async
  runtime needs to stream a child process's output live: a read-readiness signal
  (tell me when this pipe has data, delivered as a signal the reactor can wait on
  alongside sockets), a non-blocking mode (reads return "would block" instead of
  stalling), and a fix so ordinary reads return as soon as data is available
  instead of hanging until the buffer fills. This is the groundwork for running a
  language server or build tool directly on AROS and for the integrated terminal.
  Verified live end to end.

## 2026-07-24 - Zed workspace: tabs, syntax highlighting, status bar

- `C:ZedAros` now boots the real Zed **Workspace** on AROS, not just a bare
  editor: opening a file (`ZedAros MacRW:foo.rs`) shows it in a pane with an
  editor **tab** and the window title tracking the file, through a real Zed
  `Project` reading the AROS filesystem.
- **Syntax highlighting** works (tree-sitter grammars registered directly, and
  the syntax theme applied to them) — keywords, types, strings, and comments are
  colored.
- The **status bar** renders with a live cursor-position (Ln:Col) item plus an
  AROS marker.
- The **file tree** (project panel) works: `ZedAros MacRW:proj` opens a folder
  and shows its contents in a dock, read live from the AROS filesystem. (Its
  git-status integration is off on AROS, since that path pulled an
  embedded-database dependency needing OS primitives AROS lacks.)
- A minimal **app menu** (File / Edit / Quit) via native Intuition menus
  (right-mouse-button). Networking/wasm/terminal stay stubbed. See
  [docs/features/zed-editor](docs/features/zed-editor/README.md#the-zed-crate-boot-editor-core-on-aros).

## 2026-07-24 - Per-thread errno

- Each thread now has its own `errno`. It used to be shared across the whole
  program, so two threads failing system calls at the same time could read each
  other's error code and misreport what went wrong. This matters for the
  multi-threaded async runtime the editor port is built on. Non-threaded
  programs are unaffected. Verified live.

## 2026-07-24 - Non-blocking sockets for async networking

- Sockets can now run in real non-blocking mode. Setting a socket non-blocking
  (`FIONBIO`) makes a call that would wait return a would-block status right away
  instead of stalling, which is what an async runtime (the reactor behind
  `tokio`/`async-io`) needs to drive many connections at once. Blocking sockets
  are unchanged. This unblocks the networked side of the editor port (HTTP, LSP
  over the network, the agent panel). Verified live end to end.

## 2026-07-24 - Clock reads the real date

- Hosted AROS now boots with the correct date and time. The clock was starting
  at an ~1978 epoch because the boot never seeded it from the Mac, so every file
  timestamp and `Date` was wrong. The boot now runs `SetClock LOAD`, which reads
  the host wall-clock through the existing battclock bridge, so timestamps and
  logs are right from a fresh boot.

## 2026-07-23 - Zed editor-core boots on AROS

- The real GPL Zed `editor` crate now boots on hosted AROS. `C:ZedAros` opens a
  native window rendering an editor buffer with line numbers and the base theme,
  through the gpui_aros CPU backend, with networking, wasm, and terminal
  stubbed. This is the "minimal editor-core" path (a `zed_aros_app` staticlib
  entry over the whole `editor` dependency graph, ~50 crates given AROS arms),
  distinct from the Apache gpui-component editor that already shipped file + LSP
  support. Build and boot it with `hosted/zed/build.sh`. Typed input into the
  window is not wired yet. See
  [docs/features/zed-editor](docs/features/zed-editor/README.md#the-zed-crate-boot-editor-core-on-aros).

## 2026-07-22 - dynamic display resolution

- The Macaros window is resizable and the AROS screen resolution follows in
  both directions. Drag the window edge and AROS snaps to the nearest of 16
  display modes when you let go; pick a mode in ScreenMode Preferences and the
  window resizes to match. Fullscreen letterboxes the mode instead of resizing.
  (cocoametal host ABI v3 `cm_set_mode`; the AROS side registers a mode ladder
  and drives the change through `screenmode.prefs` + IPrefs.)
- Fixed a stuck mouse button (a Wanderer drag rectangle) left after a window
  resize, caused by the resize grab's button-down being delivered to AROS after
  AppKit's modal live-resize loop rather than before it.
- View ▸ Resolution menu: pick a standard size (640×480 up to 2560×1600)
  straight from the menu bar instead of dragging the window; the current size
  is checkmarked.

## 2026-07-21 - host deadlock fix

- Fixed a darwin host-library deadlock: the semaphore host-lock could deadlock
  against host (Metal/libdispatch) threads. Use the Forbid-based lock on darwin,
  and cache the watchdog environment probe that a per-tick `getenv` had been
  arming the deadlock through.

## 2026-07-18 - file-change notifications

- The host filesystem (`EMU:`, where the Mac's files appear inside AROS) now
  implements `StartNotify`/`EndNotify`. Programs are notified when a watched
  file or directory changes through the handler (create, write, delete, rename,
  set-protect/date, make-link), so file-watching applications work.
  Contributed upstream as AROS PR #835.

## 2026-07-17 - stability and upstream contributions

- Merged 70 upstream commits into the fork branch and boot-verified the result.
- Fixes originating from this port, sent upstream: the 64-bit taglist crash
  class (nlist/Zune varargs, PR #826), ScrollRaster/ScrollRasterBF dropping
  negative deltas on aarch64 (PR #822), pthread `timer.device` sharing and
  expired-wait handling, and hosted-darwin forwarding of mis-delivered timer
  ticks (fixes CPU-bound task preemption).
- Small upstream bug/cleanup PRs from the port: keymap `CopyMem` (#830),
  gfx convert-pixels test assertions (#831), RAM disk case-only rename (#832),
  `Run >NIL:` background-CLI noise (#833), console split-CSI reassembly (#834).

## 2026-07-13 - audio, media, keymaps

- AHI CoreAudio is now a first-class darwin/aarch64 build (correct speed and
  pitch).
- ffmpeg: a libavcodec-backed picture datatype (video first-frame previews in
  MultiView and on the desktop), FFView drag-and-drop via an AppWindow, an
  FFProbe media inspector, and an FFThumb headless thumbnailer.
- `kms.library` falls back to `.akmd` text keymap descriptors, so non-default
  keymaps load on aarch64.

## 2026-07-08 - Macaros.app v0.1

- Signed and notarized, self-contained `Macaros.app` DMG built by
  `graft/make-aros-release.sh`; a `RustHello` sample in the bundle; docs refresh.

## 2026-07-07 - GPU compute (gpufx)

- A GPU 2D compute section in the cocoametal shim that shares the display's
  Metal device and command queue, fronted by a native `gpufx.library`. It does
  YUV420 -> RGBA conversion and bilinear scaling on the GPU (measured 5-13x
  faster than the CPU path), wired into FFView's video decode path and the
  present-time scaler.

## 2026-07-06 - input and tooling

- Scroll-wheel events (`CM_EV_WHEEL`), both real host events and injected ones;
  the macOS navigation-key cluster mapped to Amiga rawkeys.
- Stable build-tree locations plus `rebuild-aros.sh` as a recovery tool;
  `aros-ctl wheel` and the `AROS_CTL_DESKTOP_EXTRA` hook.

## 2026-07-05 - initial public release

First public snapshot of the port. State at release:

- Hosted AROS boots to a crash-free Wanderer desktop in a native Cocoa/Metal
  window on Apple Silicon (Macaros.app), with keyboard/mouse, clipboard
  bridge, CoreAudio sound, host BSD sockets (TCP/IP + DNS), a host-volume
  handler, and opt-in crash containment.
- 68k JIT (`run68k`): classic Amiga hunk binaries run via an AArch64
  translator built on the vendored Emu68 decoders (MPL-2.0), with an
  independent interpreter as cross-check oracle; Rust (no_std and most of
  std) runs on it.
- Rust std runs on aarch64 AROS (net/fs/env/args/process/time/thread
  verified live); ffmpeg-based FFView image/video viewer runs natively.
- Control harness `aros-ctl` drives the windowed OS headlessly
  (type/click/screenshot/task-dump) for unattended verification.
- The AROS OS-source changes live on the fork
  ([jonx/AROS](https://github.com/jonx/AROS), branch `aarch64-darwin-graft`);
  this repo carries the host layer, tooling, and documentation.
