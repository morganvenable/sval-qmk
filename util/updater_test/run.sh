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

# Debounce: optical keys straight through, only the thumb Double Down
# debounced (kb/debounce_sval.c), and the chatter log (kb/chatter.c).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all     -DSVAL_DEBOUNCE_HOST_TEST -DSVAL_DD_DEBOUNCE_MS=5 -I"$root/tests/sval_debounce" -I"$kb"     "$root/tests/sval_debounce/test_debounce.c" "$kb/debounce_sval.c" "$kb/chatter.c" -o "$out/test_debounce"
"$out/test_debounce"

# Settings reset on the first boot of a different firmware build (kb/build_reset.c).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DBUILD_RESET_HOST_TEST -I"$kb" "$tests/test_build_reset.c" "$kb/build_reset.c" -o "$out/test_build_reset"
"$out/test_build_reset"

# Raw HID framing (tmk_core/protocol/chibios/usb_packet.h) against ChibiOS's
# real input buffers queue: a short packet never misframes the next report.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer     -I"$root/tests/sval_rawhid" -I"$root/lib/chibios/os/hal/include" -I"$root/tmk_core/protocol/chibios"     "$root/tests/sval_rawhid/test_packet.c" "$root/lib/chibios/os/hal/src/hal_buffers.c"     -o "$out/test_packet"
"$out/test_packet"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_updater"
"$out/test_updater" "$out/cross"

# kb/tools/sval_update.py against the same state machine, through ctypes
# (no sanitizers: they need to be preloaded into python).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -shared -fPIC \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATER_HOST_LIB -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/libupdater_host.so"
python3 -I "$tests/test_sval_update_tool.py" "$out/libupdater_host.so" "$out/tool"

# The commit routine (update_commit.c, included by test_commit.c) against the
# same mock die, with mock registers, the test hooks and power cuts.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_TEST_REAL_COMMIT -DSVAL_UPDATE_TEST_HOOKS -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" \
    "$kb/updater/vendor/sha512.c" \
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

# Release builds: test hooks, keytest, the host bootloader and the bench
# settings wipe cannot be compiled into one (update_build_info.c refuses).
for flags in "-DSVAL_UPDATE_TEST_HOOKS" "-DSVAL_KEYTEST" "-DSVAL_HOST_BOOTLOADER=1" "-DSVAL_TEST_WIPE"; do
    if ${CC:-cc} -std=c11 -O1 -DINIT_EE_HANDS_LEFT -DSVAL_UPDATE_RELEASE $flags -include "$kb/config.h" \
        -I"$kb/updater" -I"$kb/updater/vendor" \
        -c "$kb/updater/update_build_info.c" -o "$out/bi.o" 2>/dev/null; then
        echo "FAIL: a release build compiled with $flags"; exit 1
    fi
done
echo "release builds: refuse test hooks, keytest, the host bootloader and SVAL_TEST_WIPE"

# M3: a release build, against the real update_build_info.c and
# update_image.c. SVAL_RELEASE_SVUPS, if set, adds .svup files to check as a
# release build would before erasing: "a.svup:ok b.svup:refused ...".
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_RELEASE -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_release.c" "$kb/updater/update_build_info.c" "$kb/updater/update_image.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_release"
# shellcheck disable=SC2086 # SVAL_RELEASE_SVUPS is a list of FILE:expect words
"$out/test_release" ${SVAL_RELEASE_SVUPS:-}
