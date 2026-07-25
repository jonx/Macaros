#!/bin/sh
# run.sh -- start the LSP bridge with rust-analyzer resolved to its real
# binary (the `rust-analyzer` on PATH is a rustup proxy whose toolchain
# depends on cwd; resolve it explicitly so the bridge works from anywhere).
# The AROS editor connects to 127.0.0.1:9257 over the bsdsocket bridge.
set -eu
PORT="${LSP_PORT:-9257}"
RA="$(rustup which rust-analyzer 2>/dev/null || command -v rust-analyzer)"
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$DIR/target/debug/host-lsp-bridge" --port "$PORT" -- "$RA"
