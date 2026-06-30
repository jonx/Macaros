#!/bin/sh
# Build and run the x18 host-ABI probe. No AROS needed -- it is a plain macOS
# program that checks whether a value left in x18 survives signal delivery.
set -e
dir="$(cd "$(dirname "$0")" && pwd)"
out="${1:-/tmp/x18probe}"
cc -arch arm64 -O0 -D_XOPEN_SOURCE -Wno-deprecated-declarations -Wno-inline-asm \
   "$dir/x18probe.c" -o "$out"
exec "$out"
