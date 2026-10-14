#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/sval_multitouch_split tests/sval_multitouch_split/test_split.c -o "$work/test"
ASAN_OPTIONS=detect_leaks=0 "$work/test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined   tests/sval_touch_gestures/test_touch_gestures.c -o "$work/gestures"
ASAN_OPTIONS=detect_leaks=0 "$work/gestures"
python3 tests/sval_multitouch_split/test_i2c_abort.py
python3 -m unittest discover -s keyboards/svalboard/tools/multitouch -p 'test_*.py'
