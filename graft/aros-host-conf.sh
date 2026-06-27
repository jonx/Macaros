#!/bin/sh
# aros-host-conf.sh — translate the host app shell's aros-host.conf into launch env.
#
# The cocoametal host shell (Settings window) writes aros-host.conf (a line-oriented
# "keyword value" file it edits IN PLACE). This helper reads it and exports the env a
# launcher needs, so the GUI's choices drive the next boot. It is ADDITIVE and
# non-colliding: SOURCE it from any launcher (run-window.sh, the .app, aros-ctl):
#
#     AROS_HOST_CONF=/path/to/aros-host.conf . "$(dirname "$0")/aros-host-conf.sh"
#
# Default conf location matches the dylib's (cocoametal_settings_schema.m):
#   $AROS_HOST_CONF, else ~/Library/Application Support/AROS/aros-host.conf
#
# Keys honored:
#   hostvolume <Name:path[;WRITE]>  -> AROS_HOST_VOLUME (mount a host folder as a volume)
#   memory     <MB>                 -> AROS_HOST_MEMORY (the launcher applies it to the
#                                      AROSBootstrap.conf `memory` line)
# Unknown keys are ignored; the file stays the source of truth for both GUI and CLI.

aros_host_conf_load() {
    _conf="${AROS_HOST_CONF:-$HOME/Library/Application Support/AROS/aros-host.conf}"
    [ -f "$_conf" ] || return 0
    _val() { awk -v k="$1" '$1==k { $1=""; sub(/^[ \t]+/, ""); print; exit }' "$_conf"; }
    _hv="$(_val hostvolume)"; [ -n "$_hv" ]  && export AROS_HOST_VOLUME="$_hv"
    _mem="$(_val memory)";    [ -n "$_mem" ] && export AROS_HOST_MEMORY="$_mem"
    return 0
}

aros_host_conf_load

# When EXECUTED (not sourced), print what it would export — handy for --check / debug.
case "${0##*/}" in
    aros-host-conf.sh)
        echo "AROS_HOST_CONF=${AROS_HOST_CONF:-$HOME/Library/Application Support/AROS/aros-host.conf}"
        echo "AROS_HOST_VOLUME=${AROS_HOST_VOLUME:-<unset>}"
        echo "AROS_HOST_MEMORY=${AROS_HOST_MEMORY:-<unset>}"
        ;;
esac
