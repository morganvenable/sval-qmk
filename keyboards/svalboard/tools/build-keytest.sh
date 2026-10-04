#!/usr/bin/env bash
# Build the normal keymap with opt-in action instrumentation, and preserve a
# distinctly named image so a later production build cannot overwrite it.
set -euo pipefail
keyboard="${1:-svalboard/left}"
keymap="${2:-sval}"
case "$keyboard" in svalboard/*) ;; *) echo 'expected a svalboard/... target' >&2; exit 2 ;; esac
qmk compile -kb "$keyboard" -km "$keymap" -e SVAL_KEYTEST=yes
mkdir -p .build/keytest
image="${keyboard//\//_}_${keymap}"
cp ".build/${image}.uf2" ".build/keytest/${image}_keytest.uf2"
echo ".build/keytest/${image}_keytest.uf2"
