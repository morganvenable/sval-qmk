#!/bin/bash
# Host tests for the native Precision Touchpad experiment (SVAL_NATIVE_PTP).
# No board needed. Run from the repository root: util/test_native_ptp.sh
set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
t=tests/sval_native_ptp
cc=(cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined
    -Wno-missing-field-initializers -Ikeyboards/svalboard/azoteq -I$t)
export ASAN_OPTIONS=detect_leaks=0

"${cc[@]}" $t/dump_descriptor.c -o "$work/dump"
"$work/dump" > "$work/descriptor.hex"

"${cc[@]}" $t/test_ptp.c -o "$work/test_ptp"
"$work/test_ptp" "$work/trace.txt"

"${cc[@]}" -Itests/sval_multitouch_split -Wno-unused-function \
  $t/test_owner.c -o "$work/test_owner"
"$work/test_owner"

# Pad read schedule (pad_schedule.h) against a simulated chip and loop.
"${cc[@]}" $t/test_schedule.c -o "$work/test_schedule"
"$work/test_schedule"

# The other half's pad fetched only on news (multitouch.c with ptp.c).
"${cc[@]}" -Itests/sval_multitouch_split -Wno-unused-function   $t/test_remote_gate.c -o "$work/test_remote_gate"
"$work/test_remote_gate"

py=${PTP_TEST_PYTHON:-python3}
"$py" $t/test_descriptor.py "$work/descriptor.hex" "$work/trace.txt"

# The existing split test must still pass with the shared multitouch.c.
bash util/test_multitouch_split.sh
