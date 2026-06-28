#!/bin/sh
#
# demo-showcase.sh — record a guided tour of hosted AROS on Apple Silicon.
#
# Boots the windowed AROS headlessly via aros-ctl, records the whole session to a
# .mov in run/darwin-aarch64/ (AVFoundation — no Screen-Recording/TCC, real-time
# playback), and walks through the things we built in hosted/:
#
#   · cocoametal  — the live Cocoa/Metal window + the AmigaDOS shell (you're seeing it)
#   · CPUInfo     — AROS reporting the REAL Apple Silicon CPU (hostlib -> sysctl)
#   · bsdsocket   — real TCP/IP: a localhost round-trip + WaitSelect, an internet
#                   fetch from 1.1.1.1, and a DNS lookup (nettest)
#   · hostvolume  — a Mac folder mounted as an AROS volume (MacRW:)
#
# ...and finishes by launching the Wanderer desktop with a Clock.
#
# Not in this in-window tour (they're host-side / in progress, not driven from the
# AROS shell): the 68k->AArch64 JIT (jit68k/run68k, a host CLI: build/run68k …) and
# the CoreAudio audio driver. The clipboard bridge is interactive (Cmd-C/V) and is
# left out of the scripted flow.
#
# Usage:  graft/demo-showcase.sh [output.mov]
set -u

# --- relocatable paths -> repo root + the harness --------------------------
SELF="$0"
while [ -h "$SELF" ]; do
    d="$(cd "$(dirname "$SELF")" && pwd)"
    SELF="$(readlink "$SELF")"; case "$SELF" in /*) ;; *) SELF="$d/$SELF" ;; esac
done
HERE="$(cd "$(dirname "$SELF")" && pwd)"          # .../graft
ROOT="$(cd "$HERE/.." && pwd)"                     # repo root
CTL="$HERE/aros-ctl"

OUT="${1:-$ROOT/run/darwin-aarch64/hosted-showcase.mov}"
mkdir -p "$(dirname "$OUT")"

ECHO_BIN="${TMPDIR:-/tmp}/demo-echo-server"
STARTUP="$(mktemp -t demo-startup 2>/dev/null || echo "${TMPDIR:-/tmp}/demo-startup.$$")"
ESRV=""

cleanup() {
    [ -n "$ESRV" ] && kill "$ESRV" 2>/dev/null
    "$CTL" stop >/dev/null 2>&1
    rm -f "$STARTUP"
}
trap cleanup EXIT INT TERM

# --- shell-driver helpers --------------------------------------------------
# say  CAPTION        -> Echo a heading at the prompt (a caption in the video)
# run  CMD [SECS]     -> type CMD, Return, then let it run/render for SECS (default 2)
say() { "$CTL" type "Echo $*" >/dev/null 2>&1; "$CTL" enter >/dev/null 2>&1; "$CTL" wait 1 >/dev/null 2>&1; }
run() { "$CTL" type "$1"      >/dev/null 2>&1; "$CTL" enter >/dev/null 2>&1; "$CTL" wait "${2:-2}" >/dev/null 2>&1; }

# --- props -----------------------------------------------------------------
# (1) a localhost echo server so nettest's round-trip + WaitSelect checks pass
#     (the [N6] internet fetch + [DNS] lookup hit the real network and need no prop)
if clang -O2 "$ROOT/hosted/bsdsocket/n5b/echo_server.c" -o "$ECHO_BIN" 2>/dev/null; then
    "$ECHO_BIN" >/dev/null 2>&1 &
    ESRV="$!"
    sleep 1
else
    echo ">> note: echo server didn't build; nettest's [WS] round-trip may show FAIL" >&2
fi

# (2) a file in the shared Mac folder so the host-volume tour shows real content
#     (aros-ctl mounts ~/AROS/Shared as MacRO:/MacRW: when AROS_HOST_VOLUME is unset)
SHARED="${AROS_CTL_HOST_FOLDER:-$HOME/AROS/Shared}"
mkdir -p "$SHARED"
printf 'Hello from macOS!  This file lives on the Mac, and AROS sees it as MacRW:\n' \
    > "$SHARED/from-the-mac.txt"

# (3) a custom Startup-Sequence: the desktop ASSIGNS (so Wanderer can launch later)
#     but WITHOUT auto-starting Wanderer/EndCLI — so the boot lands at an interactive
#     CLI we drive for the tour, then we open Wanderer + the Clock at the end.
cat > "$STARTUP" <<'SEQ'
Version
FailAt 21
If NOT EXISTS "RAM:T"
    MakeDir "RAM:T"
EndIf
If NOT EXISTS "RAM:ENV"
    MakeDir "RAM:ENV"
    Assign "ENV:" "RAM:ENV"
EndIf
Assign "T:" "RAM:T"
Assign "CLIPS:" "SYS:clips"
Assign "KEYMAPS:" "DEVS:Keymaps"
Assign "LOCALE:" "SYS:Locale"
Assign "LIBS:" "SYS:Classes" ADD
Assign "HELP:" "LOCALE:Help" DEFER
Assign "IMAGES:" "SYS:System/Images" DEFER
Assign "WANDERER:" "SYS:System/Wanderer" DEFER
Assign "THEMES:" "SYS:Prefs/Presets/Themes" >NIL:
Assign "THEME:" "THEMES:AROSDefault"
If EXISTS "THEME:Images"
    Assign "IMAGES:" "THEME:Images" PREPEND
EndIf
Path "C:" "SYS:System" "S:" "SYS:Prefs" QUIET
If EXISTS "SYS:Fonts"
    Assign "FONTS:" "SYS:Fonts"
EndIf
If EXISTS "SYS:Utilities"
    Path "SYS:Utilities" QUIET ADD
EndIf
If EXISTS "C:IPrefs"
    IPrefs
EndIf
Run <NIL: >NIL: QUIET ConClip
SEQ
export AROS_CTL_STARTUP_FILE="$STARTUP"

# --- boot + record ---------------------------------------------------------
echo ">> booting AROS (windowed, headless via aros-ctl)…"
"$CTL" run
"$CTL" wait 16                               # reach the "1>" prompt

echo ">> recording -> $OUT"
"$CTL" record start "$OUT" 30
"$CTL" wait 2

# --- the tour --------------------------------------------------------------
say "AROS on Apple Silicon, hosted on macOS"
run "version full" 3

say "The real host CPU, via hostlib and sysctl"
run "CPUInfo" 4

say "Real TCP IP, DNS and an internet fetch -- bsdsocket.library"
run "nettest" 9

say "A Mac folder mounted as an AROS volume"
run "dir MacRW:" 2
run "type MacRW:from-the-mac.txt" 3

say "Now the Wanderer desktop and a clock"
run "Run WANDERER:Wanderer" 7
run "Run SYS:Utilities/Clock" 6

# --- finalize --------------------------------------------------------------
"$CTL" record stop
"$CTL" wait 2
# (cleanup trap stops AROS + the echo server + removes the temp startup)

echo ">> done."
echo ">> movie: $OUT"
echo ">> open it:  open \"$OUT\""
