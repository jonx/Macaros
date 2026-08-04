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
#   hostvolume <path>                -> AROS_HOST_VOLUME as MacRO:/MacRW:
#   hostvolume <Name:path[;WRITE]>   -> AROS_HOST_VOLUME as a literal mount spec
#   memory     <MB>                  -> AROS_HOST_MEMORY (the launcher applies it to the
#                                       AROSBootstrap.conf `memory` line)
#   keymap     <name>                -> AROS_CTL_KEYMAP (startup runs SetKeyboard)
# Unknown keys are ignored; the file stays the source of truth for both GUI and CLI.

aros_host_conf_load() {
    _conf="${AROS_HOST_CONF:-$HOME/Library/Application Support/AROS/aros-host.conf}"
    [ -f "$_conf" ] || return 0
    _val() { awk -v k="$1" '$1==k { $1=""; sub(/^[ \t]+/, ""); print; exit }' "$_conf"; }
    _expand_path() {
        case "$1" in
            "~") printf '%s\n' "$HOME" ;;
            "~/"*) printf '%s\n' "$HOME/${1#\~/}" ;;
            *) printf '%s\n' "$1" ;;
        esac
    }
    _volume_spec() {
        case "$1" in
            *:*) printf '%s\n' "$1" ;;
            *)
                _path="$(_expand_path "$1")"
                printf 'MacRO:%s\nMacRW:%s;WRITE\n' "$_path" "$_path"
                ;;
        esac
    }
    # Explicit env beats the conf file (standard precedence): the documented
    # `AROS_HOST_MEMORY=1280 aros-ctl run` recipe must keep working — the conf
    # used to clobber it, silently booting 256 MB guests (Rust apps then die
    # of OOM panics that read as spontaneous reboots).
    _hv="$(_val hostvolume)"; [ -n "$_hv" ] && [ -z "${AROS_HOST_VOLUME:-}" ] && export AROS_HOST_VOLUME="$(_volume_spec "$_hv")"
    _mem="$(_val memory)";    [ -n "$_mem" ] && [ -z "${AROS_HOST_MEMORY:-}" ] && export AROS_HOST_MEMORY="$_mem"
    _keymap="$(_val keymap)"; [ -n "$_keymap" ] && [ -z "${AROS_CTL_KEYMAP:-}" ] && export AROS_CTL_KEYMAP="$_keymap"
    : "${AROS_CTL_KEYMAP:=pc105_f}"; export AROS_CTL_KEYMAP   # default to French AZERTY when unset
    return 0
}

aros_host_conf_load

# A partial AROS build can replace AROSBootstrap.conf with an empty or incomplete
# file.  The launchers used to append their optional resident modules to that
# file, producing a plausible-looking configuration with no kernel, DOS, or
# filesystem resources.  Hosted AROS then exits before emitting any useful
# diagnostic.  Reconstruct the standard base module set from the runnable tree
# when (and only when) the kernel entry is absent.  Existing non-module settings
# and non-standard module entries are retained, with exact duplicates removed.
aros_bootstrap_conf_ensure() {
    _bootd="$1"
    _aros="$2"
    _conf="$_bootd/AROSBootstrap.conf"

    [ -f "$_conf" ] || return 1
    grep -Eq '^module[[:space:]]+(.*/)?kernel$' "$_conf" && return 0
    [ -f "$_bootd/kernel" ] || {
        echo "aros-host-conf: $_conf has no kernel entry and $_bootd/kernel is missing" >&2
        return 1
    }

    _old="$_conf.pre-repair.$$"
    _base="$_conf.base.$$"
    _new="$_conf.new.$$"
    cp "$_conf" "$_old" || return 1
    : > "$_base" || { rm -f "$_old"; return 1; }

    for _module in \
        "$_bootd/kernel" \
        "$_bootd/Devs/hostlib.resource" \
        "$_bootd/Devs/Drivers/unixio.hidd" \
        "$_bootd/L/emul-handler" \
        "$_bootd/Libs/expansion.library" \
        "$_bootd/Devs/processor.resource" \
        "$_bootd/Devs/battclock.resource" \
        "$_bootd/Devs/timer.device" \
        "$_bootd/Libs/debug.library" \
        "$_aros/Devs/bootloader.resource" \
        "$_aros/Devs/entropy.resource" \
        "$_aros/Devs/FileSystem.resource" \
        "$_aros/Devs/console.device" \
        "$_aros/Devs/dosboot.resource" \
        "$_aros/Devs/gameport.device" \
        "$_aros/Devs/lddemon.resource" \
        "$_aros/Devs/input.device" \
        "$_aros/Devs/keyboard.device" \
        "$_aros/Devs/Drivers/gfx.hidd" \
        "$_aros/Devs/Drivers/hiddclass.hidd" \
        "$_aros/Devs/Drivers/inputclass.hidd" \
        "$_aros/Devs/Drivers/keyboard.hidd" \
        "$_aros/Devs/Drivers/mouse.hidd" \
        "$_aros/Libs/aros.library" \
        "$_aros/Libs/dos.library" \
        "$_aros/Libs/gadtools.library" \
        "$_aros/Libs/graphics.library" \
        "$_aros/Libs/intuition.library" \
        "$_aros/Libs/keymap.library" \
        "$_aros/Libs/layers.library" \
        "$_aros/Libs/oop.library" \
        "$_aros/Libs/utility.library" \
        "$_aros/L/con-handler" \
        "$_aros/L/ram-handler"
    do
        [ -e "$_module" ] && printf 'module %s\n' "$_module" >> "$_base"
    done

    if awk '/^module[[:space:]]+/ { if (seen[$0]++) next } { print }' \
        "$_base" "$_old" > "$_new" && mv "$_new" "$_conf"; then
        rm -f "$_old" "$_base"
        echo "aros-host-conf: restored mandatory modules in $_conf" >&2
        return 0
    fi

    rm -f "$_base" "$_new"
    mv "$_old" "$_conf" 2>/dev/null || true
    return 1
}

# When EXECUTED (not sourced), print what it would export — handy for --check / debug.
case "${0##*/}" in
    aros-host-conf.sh)
        echo "AROS_HOST_CONF=${AROS_HOST_CONF:-$HOME/Library/Application Support/AROS/aros-host.conf}"
        echo "AROS_HOST_VOLUME=${AROS_HOST_VOLUME:-<unset>}"
        echo "AROS_HOST_MEMORY=${AROS_HOST_MEMORY:-<unset>}"
        echo "AROS_CTL_KEYMAP=${AROS_CTL_KEYMAP:-<unset>}"
        ;;
esac
