# 68k JIT console demo

Run this from `/Users/jkn/Source/Macaros`. It demonstrates an unchanged
68k LhA archive/list/extract cycle, then starts `AMIGAPeek` to show the safe
report for a legacy program that dynamically reaches Amiga hardware.

Prerequisites:

```text
~/68k-corpus/lha
~/68k-corpus/AMIGAPeek
```

## Prepare and launch

```sh
cd /Users/jkn/Source/Macaros

DEMO="$HOME/AROS/Shared/68k-demo"
mkdir -p "$DEMO/unpack"
cp "$HOME/68k-corpus/lha" "$DEMO/lha"
cp "$HOME/68k-corpus/AMIGAPeek" "$DEMO/AMIGAPeek"

printf '%s\n' \
  'Transparent 68k execution demonstration.' \
  'This file should survive an LhA roundtrip byte-for-byte.' \
  > "$DEMO/payload.txt"

cat > /tmp/aros-68k-demo.startup <<'AROS'
FailAt 255
CD MacRW:68k-demo

Echo ""
Echo "============================================================"
Echo " TEST 1: unchanged classic 68k LhA"
Echo "============================================================"

Delete demo.lzh QUIET
Delete unpack/payload.txt QUIET
lha a demo.lzh payload.txt <NIL:

Echo ""
Echo "Archive listing:"
lha l demo.lzh <NIL:

Echo ""
lha x demo.lzh unpack/ <NIL:
Echo "LhA archive/list/extract completed."

Echo ""
Echo "============================================================"
Echo " TEST 2: incompatible hardware-dependent 68k program"
Echo "============================================================"
Echo "Starting AMIGAPeek..."
Echo "Its dynamically computed hardware access should be reported:"
Echo ""
AMIGAPeek

Echo ""
Echo "Demo finished. This console is intentionally left open."
Wait 999999
AROS

./graft/aros-ctl deploy

AROS_CTL_STARTUP_FILE=/tmp/aros-68k-demo.startup \
AROS_HOST_VOLUME="MacRW:$HOME/AROS/Shared;WRITE" \
EMU68K_MAX_SECONDS=20 \
./graft/aros-ctl run
```

## Capture, verify and stop

```sh
./graft/aros-ctl shot /tmp/aros-68k-demo.png

cmp "$DEMO/payload.txt" "$DEMO/unpack/payload.txt" && \
  echo "LhA roundtrip is byte-identical"

./graft/aros-ctl stop
```

The instance is deliberately left at `Wait 999999` so the final console output
can be photographed. Press Ctrl-C in the AROS console to return to a prompt.
