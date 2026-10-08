#!/usr/bin/env bash
# Host tests for the Svalboard in-firmware updater (keyboards/svalboard/updater).
# Standalone: needs gcc (or cc) and python3, no QMK build.
#
#   util/updater_test/run.sh [REAL.uf2 ...]
#
# REAL.uf2: optional QMK builds (with a .bin beside each) to run make_update.py on.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
kb="$root/keyboards/svalboard"
tests="$root/tests/sval_updater"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

python3 -I "$tests/test_make_update.py" "$out/cross" "$@"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_TEST_KEY -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_keys.c" \
    "$kb/updater/vendor/monocypher.c" "$kb/updater/vendor/optional/monocypher-ed25519.c" \
    -o "$out/test_updater"
"$out/test_updater" "$out/cross"

# kb/tools/sval_update.py against the same state machine, through ctypes
# (no sanitizers: they need to be preloaded into python).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -shared -fPIC \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATER_HOST_LIB -DSVAL_UPDATE_TEST_KEY -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_keys.c" \
    "$kb/updater/vendor/monocypher.c" "$kb/updater/vendor/optional/monocypher-ed25519.c" \
    -o "$out/libupdater_host.so"
python3 -I "$tests/test_sval_update_tool.py" "$out/libupdater_host.so" "$out/tool"

# The commit routine (update_commit.c, included by test_commit.c) against the
# same mock die, with mock registers, the test hooks and power cuts.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_TEST_REAL_COMMIT -DSVAL_UPDATE_TEST_HOOKS -DSVAL_UPDATE_TEST_KEY -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_keys.c" \
    "$kb/updater/vendor/monocypher.c" "$kb/updater/vendor/optional/monocypher-ed25519.c" \
    -o "$out/test_commit"
"$out/test_commit"

# The TEST-ONLY public key is compiled in only with SVAL_UPDATE_TEST_KEY (make
# SVAL_UPDATE_TEST_KEY=yes, or SVAL_UPDATE_TEST_HOOKS=yes), and never together
# with SVAL_UPDATE_RELEASE.
key_hex="$(python3 -I "$kb/tools/make_update.py" --print-public --signer pure)"
for flags in "" "-DSVAL_UPDATE_TEST_KEY" "-DSVAL_UPDATE_RELEASE"; do
    ${CC:-cc} -std=c11 -O1 $flags -include "$kb/config.h" -I"$kb/updater" -I"$kb/updater/vendor" \
        -c "$kb/updater/update_keys.c" -o "$out/keys.o"
    found=no
    od -An -v -tx1 "$out/keys.o" | tr -d ' \n' | grep -q "$key_hex" && found=yes
    want=no; [ "$flags" = "-DSVAL_UPDATE_TEST_KEY" ] && want=yes
    if [ "$found" != "$want" ]; then echo "FAIL: test key present=$found with '${flags:-plain updater build}'"; exit 1; fi
done
if ${CC:-cc} -std=c11 -O1 -DSVAL_UPDATE_TEST_KEY -DSVAL_UPDATE_RELEASE -include "$kb/config.h" -I"$kb/updater" -I"$kb/updater/vendor" \
    -c "$kb/updater/update_keys.c" -o "$out/keys.o" 2>/dev/null; then
    echo "FAIL: a release build compiled with the test key"; exit 1
fi
echo "test key: only in SVAL_UPDATE_TEST_KEY builds; absent from plain and release builds; refused with release"
