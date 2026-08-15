#!/bin/sh
# Shared launch-time setup for local, bounded Macaros compatibility evidence.
# This file is sourced by both development and packaged launchers.

: "${AROS_RUN_DIR:=$HOME/Library/Application Support/AROS}"
: "${AROS_REPORT_DIR:=$AROS_RUN_DIR/Reports}"

umask 077
for macaros_report_subdir in active ready exported session; do
    mkdir -p "$AROS_REPORT_DIR/$macaros_report_subdir"
    chmod 700 "$AROS_REPORT_DIR/$macaros_report_subdir" 2>/dev/null || true
done
chmod 700 "$AROS_REPORT_DIR" 2>/dev/null || true

# A new trace file per Macaros session avoids appending forever. Each file has a
# hard 4 MiB writer limit; retaining at most 20 means routine structured traces
# remain below 80 MiB. Removal is restricted to the generated session filenames.
macaros_report_session_id="$(date -u '+%Y%m%dT%H%M%SZ')-$$"
: "${EMU68K_BRIDGE_TRACE:=$AROS_REPORT_DIR/session/emu68k-$macaros_report_session_id.jsonl}"
: "${EMU68K_BRIDGE_TRACE_LEVEL:=runtime}"
EMU68K_BRIDGE_TRACE_MAX_BYTES=4194304

# Crash bundles need a writable location outside the sealed app. Raw call traces
# are opt-in, but their line count is never allowed to become unlimited here.
: "${JIT68K_CRASH_DIR:=$AROS_REPORT_DIR/active}"
EMU68K_TRACE_CALLS_MAX=10000

macaros_report_old_files=
if find "$AROS_REPORT_DIR/session" -type f -name 'emu68k-*.jsonl' -print -quit 2>/dev/null |
    grep -q .; then
    macaros_report_old_files=$(
        ls -1t "$AROS_REPORT_DIR/session"/emu68k-*.jsonl 2>/dev/null |
            sed -n '20,$p'
    )
fi
if [ -n "$macaros_report_old_files" ]; then
    printf '%s\n' "$macaros_report_old_files" |
        while IFS= read -r macaros_report_old_file; do
            case "$macaros_report_old_file" in
                "$AROS_REPORT_DIR/session"/emu68k-*.jsonl)
                    rm -f -- "$macaros_report_old_file"
                    ;;
            esac
        done
fi
unset macaros_report_subdir macaros_report_session_id
unset macaros_report_old_files macaros_report_old_file

export AROS_RUN_DIR AROS_REPORT_DIR
export EMU68K_BRIDGE_TRACE EMU68K_BRIDGE_TRACE_LEVEL
export EMU68K_BRIDGE_TRACE_MAX_BYTES EMU68K_TRACE_CALLS_MAX
export JIT68K_CRASH_DIR
