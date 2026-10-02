#!/usr/bin/env bash
# Copy a UF2 onto a Svalboard half that is sitting in the RP2040 bootloader
# (the RPI-RP2 drive), then wait for it to reboot. Put the half into the
# bootloader first: Scan Lab > Firmware > Reboot into bootloader (needs a build
# with SVAL_HOST_BOOTLOADER, e.g. the scanlab keymap), or hold BOOTSEL while
# plugging in. Works from WSL (through PowerShell), Linux and macOS.
#
#   keyboards/svalboard/tools/flash.sh <image.uf2> [timeout-seconds]
set -euo pipefail

uf2="${1:?usage: flash.sh <image.uf2> [timeout-seconds]}"
timeout="${2:-60}"
[ -f "$uf2" ] || { echo "no such file: $uf2" >&2; exit 1; }

label="RPI-RP2"
ps="/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
is_wsl=0; [ -x "$ps" ] && grep -qi microsoft /proc/version 2>/dev/null && is_wsl=1

drive_letter() {  # WSL: letter of the RPI-RP2 volume, or empty
    "$ps" -NoProfile -Command "(Get-Volume | Where-Object { \$_.FileSystemLabel -eq '$label' } | Select-Object -First 1).DriveLetter" 2>/dev/null | tr -d '\r\n '
}
mount_point() {   # Linux / macOS: mounted RPI-RP2 path, or empty
    if [ "$(uname)" = "Darwin" ]; then [ -d "/Volumes/$label" ] && echo "/Volumes/$label"; return 0; fi
    local dev; dev="/dev/disk/by-label/$label"
    if [ -e "$dev" ]; then
        local mp; mp=$(lsblk -no MOUNTPOINT "$(readlink -f "$dev")" 2>/dev/null | head -1)
        if [ -z "$mp" ] && command -v udisksctl >/dev/null; then
            udisksctl mount -b "$(readlink -f "$dev")" >/dev/null 2>&1 || true
            mp=$(lsblk -no MOUNTPOINT "$(readlink -f "$dev")" 2>/dev/null | head -1)
        fi
        [ -n "$mp" ] && echo "$mp"
    fi
    return 0
}

echo "waiting up to ${timeout}s for the $label drive..."
deadline=$(( $(date +%s) + timeout ))
target=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    if [ $is_wsl -eq 1 ]; then target=$(drive_letter); else target=$(mount_point); fi
    [ -n "$target" ] && break
    sleep 1
done
[ -n "$target" ] || { echo "no $label drive appeared; is the board in bootloader mode?" >&2; exit 2; }

echo "flashing $(basename "$uf2") -> $target"
if [ $is_wsl -eq 1 ]; then
    win_src=$(wslpath -w "$uf2")
    "$ps" -NoProfile -Command "Copy-Item -LiteralPath '$win_src' -Destination '${target}:\\' " >/dev/null
else
    cp "$uf2" "$target/" && sync
fi

echo "waiting for the board to reboot..."
for _ in $(seq 1 30); do
    sleep 1
    if [ $is_wsl -eq 1 ]; then [ -z "$(drive_letter)" ] && { echo "done"; exit 0; }
    else [ -z "$(mount_point)" ] && { echo "done"; exit 0; }; fi
done
echo "the drive is still present; the copy may not have completed" >&2
exit 3
