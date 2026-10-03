// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Include generated config from sval.json (defines SVAL_*_ENTRIES)
#include "sval_config.h"

// Defaults for features not in sval.json (0 = disabled)
#ifndef SVAL_TAP_DANCE_ENTRIES
#    define SVAL_TAP_DANCE_ENTRIES 0
#endif

#ifndef SVAL_COMBO_ENTRIES
#    define SVAL_COMBO_ENTRIES 0
#endif

#ifndef SVAL_KEY_OVERRIDE_ENTRIES
#    define SVAL_KEY_OVERRIDE_ENTRIES 0
#endif

#ifndef SVAL_ALT_REPEAT_KEY_ENTRIES
#    define SVAL_ALT_REPEAT_KEY_ENTRIES 0
#endif

#ifndef SVAL_LEADER_ENTRIES
#    define SVAL_LEADER_ENTRIES 0
#endif

// USB serial. Hosts detect a Sval board by the "sval:" magic in the serial string
// (sval-gui util.py), so when a build drops the fixed SERIAL_NUMBER literal in favour
// of the chip's hardware ID, the magic is carried over as a prefix. The result is
// "sval:<hardware id>": still matched by every existing host, but unique per board and
// unchanged by a reflash, so a browser's WebHID grant survives a firmware update.
#ifndef SERIAL_NUMBER_PREFIX
#    define SERIAL_NUMBER_PREFIX "sval:"
#endif

// QMK settings storage size (sval_qmk_settings_t)
#define SVAL_QMK_SETTINGS_SIZE 44

// Dynamic leader timeout - Sval controls this variable
#if defined(LEADER_ENABLE) && !defined(__ASSEMBLER__)
#include <stdint.h>
extern uint16_t sval_leader_timeout;
#define LEADER_TIMEOUT (sval_leader_timeout)
#endif

// Defaults for dynamic keymap counts (needed for label storage calculation)
#ifndef DYNAMIC_KEYMAP_LAYER_COUNT
#    define DYNAMIC_KEYMAP_LAYER_COUNT 4
#endif

#ifndef DYNAMIC_KEYMAP_MACRO_COUNT
#    define DYNAMIC_KEYMAP_MACRO_COUNT 16
#endif

// Total size: tap_dance*10 + combo*12 + key_override*12 + alt_repeat*6 + one_shot(3) + leader*14 + magic(6) + qmk_settings(44) + fragments(21) + labels_v2(td*16 + macro*16 + layer*16)
#define SVAL_EEPROM_SIZE_CALC ( \
    (SVAL_TAP_DANCE_ENTRIES * 10) + \
    (SVAL_COMBO_ENTRIES * 12) + \
    (SVAL_KEY_OVERRIDE_ENTRIES * 12) + \
    (SVAL_ALT_REPEAT_KEY_ENTRIES * 6) + \
    3 + \
    (SVAL_LEADER_ENTRIES * 14) + \
    6 + SVAL_QMK_SETTINGS_SIZE + 21 + \
    (SVAL_TAP_DANCE_ENTRIES * 16) + \
    (DYNAMIC_KEYMAP_MACRO_COUNT * 16) + \
    (DYNAMIC_KEYMAP_LAYER_COUNT * 16))

#ifndef EECONFIG_KB_DATA_SIZE
#    define EECONFIG_KB_DATA_SIZE SVAL_EEPROM_SIZE_CALC
#endif
