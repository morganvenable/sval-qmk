#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/sval_multitouch_split tests/sval_multitouch_split/test_split.c -o "$work/test"
ASAN_OPTIONS=detect_leaks=0 "$work/test"
python3 -m unittest discover -s keyboards/svalboard/tools/multitouch -p 'test_*.py'
