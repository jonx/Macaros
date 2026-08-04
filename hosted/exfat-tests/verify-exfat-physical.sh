#!/bin/sh
# Read-only final verification for the physical USB partition after Unit14
# has been written back. Run from an interactive macOS terminal because the
# Codex process may not have Files and Folders access to /Volumes.
set -eu

device=${EXFAT_PHYSICAL_DEVICE:-disk9s1}
expected_size=536870912

info=$(diskutil info -plist "$device")
mount_point=$(printf '%s' "$info" | plutil -extract MountPoint raw -o - -)
size=$(printf '%s' "$info" | plutil -extract Size raw -o - -)
filesystem=$(printf '%s' "$info" | plutil -extract FilesystemType raw -o - -)

[ "$size" = "$expected_size" ] || {
    echo "FAIL: $device is $size bytes, expected $expected_size" >&2
    exit 1
}
[ "$filesystem" = exfat ] || {
    echo "FAIL: $device is $filesystem, expected exfat" >&2
    exit 1
}
[ -d "$mount_point/AROSUSB" ] || {
    echo "FAIL: no AROSUSB directory at $mount_point" >&2
    exit 1
}

check_hash()
{
    file=$1
    expected=$2
    actual=$(shasum -a 256 "$mount_point/AROSUSB/$file" | awk '{print $1}')
    [ "$actual" = "$expected" ] || {
        echo "FAIL: $file has SHA-256 $actual" >&2
        exit 1
    }
    echo "$actual  $mount_point/AROSUSB/$file"
}

check_hash Renamed.txt \
    03b7b484ab8deb50fbcdc87dd06e0b97ed9b530b9df6eadc3c310852d9a76d92
check_hash Handler.bin \
    0dbc16c01be5dca6c30799ef8cedb56e2b1f8774feae170321c313ba109f792b
check_hash AROS-Written.txt \
    5d23dbb1dc0c320bb5a6f41c201f2ebb435318c3bf075730738cd463640decfa

[ ! -e "$mount_point/AROSUSB/Handover.txt" ] || {
    echo "FAIL: old source name Handover.txt remains" >&2
    exit 1
}
[ ! -e "$mount_point/AROSUSB/Nested/Source.c" ] || {
    echo "FAIL: deleted Nested/Source.c remains" >&2
    exit 1
}

diskutil verifyVolume "$device"
echo "PASS: physical $device contains the verified AROS exFAT mutations"
