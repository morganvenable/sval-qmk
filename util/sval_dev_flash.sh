#!/usr/bin/env bash
# Build no-chord dev firmware and flash it with the in-firmware updater, with
# no prompts and no chord (docs/updater.md, "Dev builds"). Fork only: these
# builds never go into a release.
#
#   util/sval_dev_flash.sh [-n] SERIAL [left|right|both] [keep|claussen|blank]
#
# Run in WSL from a worktree. SERIAL is the board's USB serial (sval:...); the
# updater talks only to it. -n builds without flashing (SERIAL may be -).
#
# Modes (default keep):
#   keep      the private claussen keymap; stored settings are kept
#   claussen  the same, and stored settings reset once to the claussen layout
#   blank     the blank keymap, and stored settings reset once to it
# A reset happens on the first boot of that build only (dev_reset.c): the next
# keep flash keeps the settings again.
#
# Files live under $SVAL_HOME (default /mnt/c/Users/$USER/Svalboard):
#   layouts/private/claussen/keymap/  the private keymap (gen_keymap.py), never
#                                     committed: copied in at build time
#   firmware/dev/                     the .uf2 and .svup of each build
#   tools/                            sval_update.py and make_update.py
# Flashing runs sval_update.py with Windows Python ($SVAL_PY), which has hidapi.
# Both halves need a no-chord build running for a flash with no chord (the
# first one needs the chord, or a .uf2 copied in BOOTSEL).
set -euo pipefail

build_only=0
if [ "${1:-}" = "-n" ]; then build_only=1; shift; fi
serial=${1:-}
side=${2:-both}
mode=${3:-keep}
usage="usage: $0 [-n] SERIAL [left|right|both] [keep|claussen|blank]"
case "$serial" in
    sval:?*) ;;
    -) [ "$build_only" = 1 ] || { echo "$usage" >&2; exit 2; } ;;
    *) echo "$usage" >&2; exit 2 ;;
esac
case "$side" in left|right) sides=$side ;; both) sides="left right" ;;
    *) echo "$usage" >&2; exit 2 ;; esac
case "$mode" in keep|claussen) km=claussen ;; blank) km=blank ;;
    *) echo "$usage" >&2; exit 2 ;; esac

root=$(cd "$(dirname "$0")/.." && pwd)
home=${SVAL_HOME:-/mnt/c/Users/$USER/Svalboard}
out=$home/firmware/dev
tools=$home/tools
private=$home/layouts/private/claussen
mkdir -p "$out" "$tools"
cd "$root"
tc=$(ls -d ~/arm-toolchain/*/bin | head -1)
export PATH="$HOME/.venvs/sval-qmk/bin:$tc:$PATH"

# The private keymap: copied in, never tracked.
if [ "$km" = claussen ]; then
    [ -f "$private/keymap/keymap.c" ] || {
        echo "no $private/keymap: run gen_keymap.py" >&2; exit 1; }
    exclude=$(git rev-parse --git-path info/exclude)
    rel=/keyboards/svalboard/keymaps/claussen/
    grep -qxF "$rel" "$exclude" 2>/dev/null || echo "$rel" >> "$exclude"
    rm -rf keyboards/svalboard/keymaps/claussen
    cp -r "$private/keymap" keyboards/svalboard/keymaps/claussen
fi

commit=$(git rev-parse --short=10 HEAD)
flags=(SVAL_UPDATER=yes SVAL_UPDATE_NO_CHORD=yes)
flags+=(SVAL_FW_VERSION_STRING="dev-$commit")
if [ "$mode" != keep ]; then
    token=00000000
    while [ "$token" = 00000000 ]; do
        token=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
    done
    flags+=(SVAL_DEV_RESET_TOKEN="$token")
    echo "reset token $token"
fi

svups=()
for s in $sides; do
    log=${TMPDIR:-/tmp}/sval_dev_build_$s.log
    make -j"$(nproc)" "svalboard/$s:$km" "${flags[@]}" > "$log" 2>&1 || {
        tail -20 "$log" >&2; exit 1; }
    name=$s-$mode-$commit
    cp "svalboard_${s}_$km.uf2" "$out/$name.uf2"
    python3 -I keyboards/svalboard/tools/make_update.py \
        "svalboard_${s}_$km.uf2" -o "$out/$name.svup" \
        --kb "svalboard/$s" --keymap "$km" >/dev/null
    if [ "$km" = claussen ]; then
        python3 -I "$private/verify_keymap.py" keyboards/svalboard/tools \
            ".build/svalboard_${s}_$km.elf"
    fi
    echo "$out/$name.uf2"
    svups+=("$(wslpath -w "$out/$name.svup")")
done
[ "$build_only" = 1 ] && exit 0

cp keyboards/svalboard/tools/sval_update.py \
    keyboards/svalboard/tools/make_update.py "$tools/"
py=${SVAL_PY:-$(ls /mnt/c/Users/"$USER"/AppData/Local/Programs/Python/Python3*/python.exe | tail -1)}
up=$(wslpath -w "$tools/sval_update.py")
if [ "${#svups[@]}" = 2 ]; then
    "$py" -u "$up" --serial "$serial" pair "${svups[@]}" --yes
else
    "$py" -u "$up" --serial "$serial" update "${svups[0]}" --yes
fi
