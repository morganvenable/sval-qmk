#!/usr/bin/env bash
# Offline tests for the board record (keyboards/svalboard/board_record.*) and
# the board ID station (keyboards/svalboard/tools/board_id). Needs cc and
# python3; with arm-none-eabi-gcc on PATH it also rebuilds the ID-writer and
# checks that the committed writer.uf2 matches.
#
#   tests/sval_board_id/run.sh [REAL.uf2 ...]   optional QMK builds to check
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
kb="$root/keyboards/svalboard"
tests="$root/tests/sval_board_id"
writer="$kb/tools/board_id/writer"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

san="-fsanitize=address,undefined -fno-sanitize-recover=all"
# shellcheck disable=SC2086
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g $san -I"$kb" \
    "$tests/test_board_record.c" "$kb/board_record.c" -o "$out/test_board_record"
"$out/test_board_record" "$out/vector.bin"
python3 -I "$tests/test_codec.py" "$out/vector.bin"

make -s -C "$writer" host OUT="$out"
uf2="$kb/tools/board_id/writer.uf2"
if command -v arm-none-eabi-gcc >/dev/null; then
    make -s -C "$writer" OUT="$out"
    arm-none-eabi-size "$out/writer.elf"
    if ! cmp -s "$out/writer.uf2" "$uf2"; then
        echo "FAIL: tools/board_id/writer.uf2 is not the build of writer/ (make -C $writer OUT=..; copy writer.uf2)"; exit 1
    fi
    echo "writer.uf2 matches a fresh build"
fi
python3 -I "$tests/test_station.py" "$out/writer_host" "$uf2" "$out/station"
python3 -I "$tests/test_flash_guard.py" "$@"
echo "board ID: all tests passed"
