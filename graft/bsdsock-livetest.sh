#!/bin/sh
#
# bsdsock-livetest.sh — exercise the host-passthrough bsdsocket.library end to end.
#
# Two tiers:
#   1. HOST UNIT TESTS (instant, no boot) — the kqueue pump, the dlopen ABI, and the
#      Darwin->AmiTCP errno table. These print definitive PASS/FAIL text.
#   2. LIVE on booted AROS — boots the windowed AROS via aros-ctl, starts a localhost
#      echo server, runs the `nettest` C: command (WaitSelect + a raw-IP HTTP fetch +
#      a DNS-resolved fetch), and screenshots the verdicts. The echo server's log is
#      the host-side oracle for the localhost round-trip.
#
# The live tier needs: libbsdsockhost.dylib in ~/lib (this script deploys it), a
# booted AROS that reaches the "1>" CLI prompt, and (for the internet tests) host
# outbound network. Doc: docs/features/bsdsocket-net/README.md
#
# Usage:  graft/bsdsock-livetest.sh [--host-only]
set -u

# --- relocatable paths (resolve our own dir -> repo root) ------------------
SELF="$0"
while [ -h "$SELF" ]; do
    d=$(cd "$(dirname "$SELF")" && pwd); SELF=$(readlink "$SELF")
    case "$SELF" in /*) ;; *) SELF="$d/$SELF";; esac
done
HERE=$(cd "$(dirname "$SELF")" && pwd)     # .../graft
ROOT=$(cd "$HERE/.." && pwd)               # repo root
cd "$ROOT" || exit 1

pass=0; fail=0
note() { printf '\n=== %s ===\n' "$1"; }
verdict() { if echo "$2" | grep -q 'PASS'; then pass=$((pass+1)); printf '  [PASS] %s\n' "$1";
            else fail=$((fail+1)); printf '  [FAIL] %s\n' "$1"; fi; }

# --- 1. HOST UNIT TESTS ----------------------------------------------------
note "host unit tests (host clang, no boot)"
for t in hosted-bsdsocket bsdsock-abi bsdsock-errno; do
    out=$(make "$t" 2>&1)
    verdict "$t" "$(echo "$out" | grep -E 'result=PASS|] PASS|PASS ')"
done

if [ "${1:-}" = "--host-only" ]; then
    note "summary"; printf '  host tests: %d passed, %d failed\n' "$pass" "$fail"
    exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
fi

# --- 2. LIVE on booted AROS ------------------------------------------------
note "deploy the host pump dylib to ~/lib"
make bsdsock-dylib >/dev/null 2>&1 && mkdir -p "$HOME/lib" \
    && cp -f build/libbsdsockhost.dylib "$HOME/lib/libbsdsockhost.dylib" \
    && echo "  ~/lib/libbsdsockhost.dylib $(shasum -a256 "$HOME/lib/libbsdsockhost.dylib" | cut -c1-12)"

ECHO_BIN="${TMPDIR:-/tmp}/bsdsock-echo"
SHOT="${TMPDIR:-/tmp}/bsdsock-nettest.png"
clang -O2 hosted/bsdsocket/n5b/echo_server.c -o "$ECHO_BIN" 2>/dev/null || { echo "  echo_server build failed"; exit 1; }

note "live: boot AROS, run nettest (WaitSelect + raw-IP fetch + DNS)"
"$ECHO_BIN" >"${TMPDIR:-/tmp}/bsdsock-echo.log" 2>&1 &  ESRV=$!
sleep 1
"$HERE/aros-ctl" run >/dev/null 2>&1
"$HERE/aros-ctl" wait 16 >/dev/null 2>&1
if "$HERE/aros-ctl" libs >/dev/null 2>&1; then
    "$HERE/aros-ctl" type "nettest" >/dev/null 2>&1
    "$HERE/aros-ctl" enter >/dev/null 2>&1
    "$HERE/aros-ctl" wait 16 >/dev/null 2>&1       # DNS + two HTTP fetches
    "$HERE/aros-ctl" shot "$SHOT" >/dev/null 2>&1
    crash=$("$HERE/aros-ctl" crash 2>&1 | tail -3)
    "$HERE/aros-ctl" stop >/dev/null 2>&1
else
    echo "  AROS did not come up (windowed boot needed; see README gotchas)"
fi
kill "$ESRV" 2>/dev/null

note "live results"
echo "  echo server (localhost round-trip oracle):"
sed 's/^/    /' "${TMPDIR:-/tmp}/bsdsock-echo.log" 2>/dev/null
[ -n "${crash:-}" ] && printf '  trap/alert lines:\n%s\n' "$crash"
echo "  verdict screenshot ([WS]/[N6]/[DNS]): $SHOT"
[ -f "$SHOT" ] && echo "    open it:  open $SHOT"

note "summary"
printf '  host tests: %d passed, %d failed; live verdicts are in the screenshot above.\n' "$pass" "$fail"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
