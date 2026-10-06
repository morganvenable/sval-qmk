// SPDX-License-Identifier: GPL-2.0-or-later
#include "quantum.h"
const uint16_t keys[]       = {KC_A, KC_B, COMBO_END};
combo_t        key_combos[] = {COMBO(keys, KC_ESC)};
// Sval normally supplies these from its EEPROM-backed combo table.
uint16_t combo_count(void) {
    return 1;
}
combo_t *combo_get(uint16_t index) {
    return index == 0 ? &key_combos[0] : NULL;
}
