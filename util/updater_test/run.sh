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
python3 -I "$tests/test_release_version.py" "$out/release_version"
python3 -I "$tests/test_release_artifacts.py" "$out/release_artifacts"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_TEST_KEY -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_keys.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
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
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
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
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
    "$kb/updater/vendor/monocypher.c" "$kb/updater/vendor/optional/monocypher-ed25519.c" \
    -o "$out/test_commit"
"$out/test_commit"

# M2: the split pause (kb/split_pause.c), the KEYBOARD_UPDATE wire format and
# page assembly. The other half's session and the relay (update_split.c) are
# tested in the builds above, by test_relay.c, against the mock die.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" \
    "$tests/test_split.c" "$kb/split_pause.c" "$kb/updater/update_split_wire.c" \
    -o "$out/test_split"
"$out/test_split"

# The TEST-ONLY public key is compiled in only with SVAL_UPDATE_TEST_KEY (make
# SVAL_UPDATE_TEST_KEY=yes, or SVAL_UPDATE_TEST_HOOKS=yes), and never together
# with SVAL_UPDATE_RELEASE.
key_hex="$(python3 -I "$kb/tools/make_update.py" --print-public --signer pure)"
release_hex="$(python3 -I "$kb/tools/make_update.py" --print-release-keys)"
for flags in "" "-DSVAL_UPDATE_TEST_KEY" "-DSVAL_UPDATE_RELEASE"; do
    ${CC:-cc} -std=c11 -O1 $flags -DINIT_EE_HANDS_LEFT -include "$kb/config.h" -I"$kb/updater" -I"$kb/updater/vendor" \
        -c "$kb/updater/update_keys.c" -o "$out/keys.o"
    found=no
    od -An -v -tx1 "$out/keys.o" | tr -d ' \n' | grep -q "$key_hex" && found=yes
    want=no; [ "$flags" = "-DSVAL_UPDATE_TEST_KEY" ] && want=yes
    if [ "$found" != "$want" ]; then echo "FAIL: test key present=$found with '${flags:-plain updater build}'"; exit 1; fi
    # M3: both release keys (update_release_keys.h) are in every updater build.
    for rk in $release_hex; do
        if ! od -An -v -tx1 "$out/keys.o" | tr -d ' \n' | grep -q "$rk"; then
            echo "FAIL: release key $rk missing with '${flags:-plain updater build}'"; exit 1
        fi
    done
done
if ${CC:-cc} -std=c11 -O1 -DINIT_EE_HANDS_LEFT -DSVAL_UPDATE_TEST_KEY -DSVAL_UPDATE_RELEASE -include "$kb/config.h" -I"$kb/updater" -I"$kb/updater/vendor" \
    -c "$kb/updater/update_keys.c" -o "$out/keys.o" 2>/dev/null; then
    echo "FAIL: a release build compiled with the test key"; exit 1
fi
echo "test key: only in SVAL_UPDATE_TEST_KEY builds; absent from plain and release builds; refused with release"
echo "release keys: both in plain, test-key and release builds"

# M3: a release build's key set (no test key, the two release keys), against
# the real update_keys.c and update_image.c. Anything the TEST-ONLY key signs
# is refused. SVAL_RELEASE_SVUPS, if set, adds .svup files to check as a
# release build would before erasing: "a.svup:ok b.svup:refused ...".
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_RELEASE -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_release.c" "$kb/updater/update_keys.c" "$kb/updater/update_image.c" \
    "$kb/updater/vendor/monocypher.c" "$kb/updater/vendor/optional/monocypher-ed25519.c" \
    -o "$out/test_release"
# shellcheck disable=SC2086 # SVAL_RELEASE_SVUPS is a list of FILE:expect words
"$out/test_release" "$kb/tools/sval_update_TEST_ONLY.key" ${SVAL_RELEASE_SVUPS:-}
