// SPDX-License-Identifier: GPL-2.0-or-later
#include "../test_runtime_tap.cpp"
TEST_F(RuntimeTap, AutoShiftReleaseUsesRuntimeDelayWithZeroCompileTimeDefault) {
    TestDriver driver;
    auto       key = KeymapKey(0, 0, 0, KC_A);
    set_keymap({key});
    for (uint16_t delay : {0, 37, 300}) {
        code_delay = delay;
        expect_timed_tap(driver, KC_A, delay, [&] { tap_key(key, 30); });
        idle_for(300);
    }
}
