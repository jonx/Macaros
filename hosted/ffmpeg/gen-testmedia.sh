#!/bin/bash
# gen-testmedia.sh -- create sample media for FFView in ~/AROS/Shared (= MacRW:),
# using the host's ffmpeg. Run once, then inside AROS: FFView MacRW:test.avi
#   - test.jpg : a still image (mjpeg)
#   - test.avi : MJPEG-in-AVI video, 320x240, 12fps, 4s (full-range 4:4:4)
#   - test.m4v : MPEG-4 video, 320x240, 15fps, 4s (limited-range 4:2:0)
set -euo pipefail

DEST="${1:-$HOME/AROS/Shared}"
mkdir -p "$DEST"
command -v ffmpeg >/dev/null || { echo "host ffmpeg not found (brew install ffmpeg)" >&2; exit 1; }

ffmpeg -v error -y -f lavfi -i testsrc=size=320x240:rate=1:duration=1 -frames:v 1 -q:v 4 "$DEST/test.jpg"
ffmpeg -v error -y -f lavfi -i testsrc=size=320x240:rate=12:duration=4 -c:v mjpeg -q:v 5 "$DEST/test.avi"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x240:rate=15:duration=4 -c:v mpeg4 -q:v 4 "$DEST/test.m4v"

echo "wrote into $DEST (= MacRW: in AROS):"
for f in test.jpg test.avi test.m4v; do printf '  %-10s %s bytes\n' "$f" "$(wc -c < "$DEST/$f")"; done
