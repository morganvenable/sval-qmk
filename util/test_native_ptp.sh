#!/bin/bash
# Host tests for the native Precision Touchpad (SVAL_NATIVE_PTP). No board
# needed. Run from the repository root: util/test_native_ptp.sh
# The Azoteq driver's contact read has its own gtest:
#   make test:pointing/azoteq_multitouch
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
t=tests/sval_native_ptp
cc=(${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
    -Wno-missing-field-initializers -Ikeyboards/svalboard/azoteq -I$t)
export ASAN_OPTIONS=detect_leaks=0

"${cc[@]}" $t/dump_descriptor.c -o "$work/dump"
"$work/dump" > "$work/descriptor.hex"

# The unifier, feature reports and frame builder (ptp.c).
"${cc[@]}" $t/test_ptp.c -o "$work/test_ptp"
"$work/test_ptp" "$work/trace.txt"

# The contact path and the split link (multitouch.c with ptp.c).
"${cc[@]}" -Wno-unused-function $t/test_multitouch.c -o "$work/test_multitouch"
"$work/test_multitouch"

# The pad read schedule (pad_schedule.h) against a simulated chip and loop.
"${cc[@]}" $t/test_schedule.c -o "$work/test_schedule"
"$work/test_schedule"

py=${PTP_TEST_PYTHON:-python3}
# The descriptor, and every report the C tests sent, decoded with it.
"$py" $t/test_descriptor.py "$work/descriptor.hex" "$work/trace.txt"
# The dev tools' parsers (no board).
"$py" -m unittest discover -s keyboards/svalboard/tools/multitouch -p 'test_*.py'
