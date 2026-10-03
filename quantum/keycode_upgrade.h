// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keycodes.h"

// Upgrading stored keycodes across QMK keycode versions.
//
// Stored keymaps record the keycode version that wrote them. When QMK renumbers
// keycodes, the stored values are translated instead of the whole keymap being
// thrown away. Every translation here must be idempotent: an upgrade that is
// interrupted is simply run again on the next boot, so applying it to a value it
// already upgraded must leave that value alone.
//
// A stored version older than KEYCODE_UPGRADE_OLDEST has no table and is reset.

_Static_assert(QMK_KEYCODES_VERSION_MAJOR == 0 && QMK_KEYCODES_VERSION_MINOR == 0, "the stored keycode version is the patch number only");

#define KEYCODE_UPGRADE_CURRENT ((uint8_t)QMK_KEYCODES_VERSION_PATCH)
#define KEYCODE_UPGRADE_OLDEST 7 // 0.0.7, the Vial firmware shipped on Svalboards

static inline bool keycode_upgrade_supported(uint8_t from) {
    return from >= KEYCODE_UPGRADE_OLDEST && from <= KEYCODE_UPGRADE_CURRENT;
}

static inline uint16_t keycode_upgrade(uint16_t kc, uint8_t from) {
    if (from < 9) {
        // 0.0.9 rebuilt the steno range: the mode keys and the combined-map
        // chords moved up, and their old values now name other steno keys.
        if (kc == 0x74F0) return QK_STENO_MODE_BOLT;
        if (kc == 0x74F1) return QK_STENO_MODE_GEMINI;
        if (kc >= 0x74F2 && kc <= 0x74FC) return QK_STENO_S3 + (kc - 0x74F2); // S3 .. EU
    }
    return kc;
}
