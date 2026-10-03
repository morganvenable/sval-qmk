// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// One-time migration of a user's setup from the Vial firmware shipped as
// svalboard/vial-qmk v2025-11-01 (keymap "vial").
//
// Flashing replaces only the firmware image; the wear-levelled settings area is
// left alone, and both firmwares use the same wear-levelling engine at the same
// flash offset and size. So on the first boot after a Vial board is flashed
// with this firmware, the user's Vial settings are still there, byte for byte,
// in the logical EEPROM. This file recognises that layout, copies it out, and
// rewrites it in this firmware's layout before anything validates or resets the
// stored data.
//
// It runs from keyboard_pre_init_kb(): after eeprom_driver_init(), before
// via_init(), quantum_init() and the module's sval_init().
//
// Safety:
// - It runs only on an exact fingerprint of that release (core magic, keyboard
//   data version, Vial settings version 1-6, VIA magic not mid-reset). Anything else,
//   including other Vial releases, takes the normal path: a clean reset.
// - Its first write breaks the fingerprint and the layout stamps are written
//   last, so an interrupted migration is a clean reset on the next boot, never
//   a half-converted setup that gets misread.
//
// Turn it off with SVAL_MIGRATE_VIAL = no in rules.mk.

#include "svalboard.h"
#include "sval.h"
#include "sval_qmk_settings.h"
#include "eeconfig.h"
#include "eeprom.h"
#include "nvm_eeconfig.h"
#include "nvm_eeprom_eeconfig_internal.h"
#include "dynamic_keymap.h"
#include "via.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

// Logical EEPROM layout of svalboard/vial-qmk v2025-11-01, keymap "vial",
// measured by compiling that tag (every addressable variant shares it).
#define VIAL_KB_DATA_ADDR 37       // svalboard saved_values, 54 bytes
#define VIAL_KB_DATA_SIZE 54
#define VIAL_VIA_MAGIC_ADDR 91     // Vial BUILD_ID, 3 bytes; 0xFFFFFF while resetting
#define VIAL_KEYMAP_ADDR 95        // 16 layers x 10 rows x 6 cols, big-endian u16
#define VIAL_QMK_SETTINGS_ADDR 2015 // qmk_settings_t, 40 bytes
#define VIAL_QMK_SETTINGS_SIZE 40
#define VIAL_TAP_DANCE_ADDR 2055   // 50 x 10
#define VIAL_TAP_DANCE_ENTRIES 50
#define VIAL_COMBO_ADDR 2555       // 50 x 10
#define VIAL_COMBO_ENTRIES 50
#define VIAL_KEY_OVERRIDE_ADDR 3055 // 30 x 10
#define VIAL_KEY_OVERRIDE_ENTRIES 30
#define VIAL_ALT_REPEAT_ADDR 3355  // 32 x 6
#define VIAL_ALT_REPEAT_ENTRIES 32
#define VIAL_MACRO_ADDR 3547       // to the end of the 64 KB logical EEPROM
#define VIAL_MACRO_SIZE 61989
#define VIAL_MACRO_COUNT 50
#define VIAL_LAYERS 16
#define VIAL_ROWS 10
#define VIAL_COLS 6
#define VIAL_KEYMAP_SIZE (VIAL_LAYERS * VIAL_ROWS * VIAL_COLS * 2)

// Svalboard custom keycodes: Vial numbers them from QK_KB_0, this firmware from
// QK_USER_0, in the same order for the 20 Vial had.
#define VIAL_SV_FIRST 0x7E00
#define VIAL_SV_COUNT 20
#define VIAL_KB_LAST 0x7FFF

// Vial macro extension: a 16-bit keycode, low byte 0 encoded as 0xFF00 | high.
#define VIAL_MACRO_EXT_TAP 5
#define VIAL_MACRO_EXT_UP 7

_Static_assert(DYNAMIC_KEYMAP_LAYER_COUNT == VIAL_LAYERS && MATRIX_ROWS == VIAL_ROWS && MATRIX_COLS == VIAL_COLS, "Vial migration assumes the Vial keymap geometry");
_Static_assert(SVAL_QMK_SETTINGS_SIZE >= VIAL_QMK_SETTINGS_SIZE, "Vial QMK settings are a prefix of the Sval settings");

typedef struct __attribute__((packed)) {
    uint8_t          version; // 1..6, upgraded step by step at boot; see vial_upgrade()
    uint8_t          flags;   // bit 0 left_scroll, 1 right_scroll, 2 axis_scroll_lock, 3 auto_mouse
    uint8_t          left_dpi_index;
    uint8_t          right_dpi_index;
    uint8_t          mh_timer_index;
    struct layer_hsv layer_colors[VIAL_LAYERS];
    uint8_t          turbo_scan;
} vial_saved_values_t;
_Static_assert(sizeof(vial_saved_values_t) == VIAL_KB_DATA_SIZE, "Vial saved_values is 54 bytes");

typedef struct __attribute__((packed)) {
    uint16_t on_tap, on_hold, on_double_tap, on_tap_hold, custom_tapping_term;
} vial_tap_dance_t;
typedef struct __attribute__((packed)) {
    uint16_t input[4];
    uint16_t output;
} vial_combo_t;
typedef struct __attribute__((packed)) {
    uint16_t trigger, replacement, layers;
    uint8_t  trigger_mods, negative_mod_mask, suppressed_mods, options;
} vial_key_override_t;
typedef struct __attribute__((packed)) {
    uint16_t keycode, alt_keycode;
    uint8_t  allowed_mods, options;
} vial_alt_repeat_t;

// Diagnostics, kept in watchdog scratch registers 0-3 so they survive a crash
// reset (the bootrom only uses 4-7). Read back with VIA custom value 0xF0.
//   [0] 0x5641xxSS  SS = furthest stage reached (see enum), xx = fingerprint bits
//   [1] keyboard data version dword as read
//   [2] core magic (lo16) | Vial settings version (bits 16-23) | malloc ok (bit 31)
//   [3] VIA magic bytes as read (bits 0-23) | snapshot length >> 8 (bits 24-31)
#define MIGRATE_DIAG ((volatile uint32_t *)0x4005800Cu) // WATCHDOG_BASE + SCRATCH0
enum { STAGE_START = 1, STAGE_NO_MATCH, STAGE_MATCH, STAGE_SNAPSHOT, STAGE_SVAL, STAGE_KEYMAP, STAGE_MACROS, STAGE_SETTINGS, STAGE_DONE };
static void diag_stage(uint8_t stage) {
    MIGRATE_DIAG[0] = (MIGRATE_DIAG[0] & 0xFFFFFF00u) | stage;
}
void sval_migrate_vial_diag(uint8_t *out, uint8_t len) {
    for (uint8_t i = 0; i < len && i < 16; i++) out[i] = (MIGRATE_DIAG[i / 4] >> ((i % 4) * 8)) & 0xFF;
}

static uint16_t translate_keycode(uint16_t kc) {
    if (kc >= VIAL_SV_FIRST && kc < VIAL_SV_FIRST + VIAL_SV_COUNT) {
        return QK_USER_0 + (kc - VIAL_SV_FIRST);
    }
    if (kc >= VIAL_SV_FIRST && kc <= VIAL_KB_LAST) {
        // Unassigned keyboard or user codes in Vial; here they would alias our
        // own custom keycodes, so drop them rather than fire the wrong action.
        return KC_NO;
    }
    return kc;
}

static bool vial_layout_present(void) {
    uint16_t magic   = eeprom_read_word(EECONFIG_MAGIC);
    uint32_t kbver   = eeprom_read_dword(EECONFIG_KEYBOARD);
    uint8_t  version = eeprom_read_byte((void *)VIAL_KB_DATA_ADDR);
    uint8_t  yy = eeprom_read_byte((void *)VIAL_VIA_MAGIC_ADDR + 0), mm = eeprom_read_byte((void *)VIAL_VIA_MAGIC_ADDR + 1), dd = eeprom_read_byte((void *)VIAL_VIA_MAGIC_ADDR + 2);
    // Vial's VIA magic is its per-build BUILD_ID, not QMK's build date, so it
    // cannot be predicted; it only has to differ from the 0xFFFFFF that Vial
    // writes while it is part-way through resetting the keymap.
    uint8_t  bits = (magic == EECONFIG_MAGIC_NUMBER) << 0 | (kbver == VIAL_KB_DATA_SIZE) << 1 | (version >= 1 && version <= 6) << 2 |
                   !(yy == 0xFF && mm == 0xFF && dd == 0xFF) << 3;
    MIGRATE_DIAG[0] = 0x56410000u | (bits << 8);
    MIGRATE_DIAG[1] = kbver;
    MIGRATE_DIAG[2] = magic | ((uint32_t)version << 16);
    MIGRATE_DIAG[3] = yy | (mm << 8) | ((uint32_t)dd << 16);
    return bits == 0x0F;
}

// Vial upgrades saved_values one version at a time when it boots, and the step
// to 6 does not save, so stored boards carry any version from 1 to 6. Replay the
// same chain so every board migrates with the values Vial itself would show.
static void vial_upgrade(vial_saved_values_t *v) {
    if (v->version < 2) {
        v->mh_timer_index = 3;
        v->flags |= 0x01; // left_scroll
    }
    if (v->version < 3) {
        static const uint32_t colors[VIAL_LAYERS] = {0x55FFFF, 0x15FFFF, 0x95FFFF, 0x0BB0FF, 0x2BFFFF, 0x80FF80, 0x00FFFF, 0x00FFFF,
                                                     0xEAFFFF, 0xBFFF80, 0x0BB0FF, 0x6AFFFF, 0x80FF80, 0x80FFFF, 0x2BFFFF, 0xD5FFFF};
        for (uint8_t i = 0; i < VIAL_LAYERS; i++) {
            v->layer_colors[i] = (struct layer_hsv){(colors[i] >> 16) & 0xFF, (colors[i] >> 8) & 0xFF, colors[i] & 0xFF};
        }
    }
    if (v->version < 4) v->flags |= 0x08; // auto_mouse
    if (v->version < 5) v->flags |= 0x04; // axis_scroll_lock
    if (v->version < 6) v->turbo_scan = 0;
    v->version = 6;
}

// Length of the macro buffer through the last macro's terminator.
static uint32_t vial_macro_length(void) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < VIAL_MACRO_SIZE; i++) {
        if (eeprom_read_byte((void *)(VIAL_MACRO_ADDR + i)) == 0 && ++seen == VIAL_MACRO_COUNT) {
            return i + 1;
        }
    }
    return VIAL_MACRO_SIZE;
}

// Rewrite custom keycodes inside macro extension actions, in place. The
// extension carries two non-zero bytes, so the encoding never changes length.
static void translate_macros(uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != SS_QMK_PREFIX || i + 1 >= len) continue;
        uint8_t code = buf[i + 1];
        if (code == SS_TAP_CODE || code == SS_DOWN_CODE || code == SS_UP_CODE) {
            i += 2;
        } else if (code == SS_DELAY_CODE) {
            i += 3;
        } else if (code >= VIAL_MACRO_EXT_TAP && code <= VIAL_MACRO_EXT_UP && i + 3 < len) {
            uint16_t kc = buf[i + 2] | (buf[i + 3] << 8);
            if (kc > 0xFF00) kc = (kc & 0xFF) << 8;
            kc = translate_keycode(kc);
            if ((kc & 0xFF) == 0) kc = 0xFF00 | (kc >> 8);
            buf[i + 2] = kc & 0xFF;
            buf[i + 3] = kc >> 8;
            i += 3;
        }
    }
}

// Cut at the last whole macro that fits, so a macro is never half-kept.
static uint32_t fit_macros(const uint8_t *buf, uint32_t len, uint32_t room) {
    if (len <= room) return len;
    uint32_t end = 0;
    for (uint32_t i = 0; i < room; i++) {
        if (buf[i] == 0) end = i + 1;
    }
    return end;
}

void sval_migrate_vial(void) {
    if (!vial_layout_present()) {
        diag_stage(STAGE_NO_MATCH);
        return;
    }
    diag_stage(STAGE_MATCH);

    // Snapshot everything we need before the first write: the keyboard data
    // block through the end of the used macros.
    uint32_t macro_len = vial_macro_length();
    uint32_t snap_len  = (VIAL_MACRO_ADDR - VIAL_KB_DATA_ADDR) + macro_len;
    uint8_t *snap      = malloc(snap_len);
    MIGRATE_DIAG[3] = (MIGRATE_DIAG[3] & 0x00FFFFFFu) | ((snap_len >> 8) << 24);
    if (!snap) return; // falls through to the normal clean reset
    MIGRATE_DIAG[2] |= 0x80000000u;
    diag_stage(STAGE_SNAPSHOT);
    eeprom_read_block(snap, (void *)VIAL_KB_DATA_ADDR, snap_len);
#define AT(addr) (snap + ((addr) - VIAL_KB_DATA_ADDR))

    // First write breaks the Vial fingerprint (the keyboard data version), so
    // an interruption from here on can never re-run on half-converted data.
    eeconfig_init_kb_datablock();
    via_eeprom_set_valid(false);

    diag_stage(STAGE_SVAL);
    // Sval data block: QMK settings, tap dance, combos, overrides, alt-repeat.
    sval_qmk_settings_reset();
    eeconfig_update_kb_datablock(AT(VIAL_QMK_SETTINGS_ADDR), SVAL_QMK_SETTINGS_OFFSET, VIAL_QMK_SETTINGS_SIZE);

    for (uint8_t i = 0; i < MIN(VIAL_TAP_DANCE_ENTRIES, SVAL_TAP_DANCE_ENTRIES); i++) {
        vial_tap_dance_t       v;
        sval_tap_dance_entry_t e;
        memcpy(&v, AT(VIAL_TAP_DANCE_ADDR) + i * sizeof(v), sizeof(v));
        e.on_tap              = translate_keycode(v.on_tap);
        e.on_hold             = translate_keycode(v.on_hold);
        e.on_double_tap       = translate_keycode(v.on_double_tap);
        e.on_tap_hold         = translate_keycode(v.on_tap_hold);
        bool used             = e.on_tap || e.on_hold || e.on_double_tap || e.on_tap_hold;
        e.custom_tapping_term = (v.custom_tapping_term & 0x7FFF) | (used ? 0x8000 : 0);
        eeconfig_update_kb_datablock(&e, SVAL_TAP_DANCE_OFFSET + i * sizeof(e), sizeof(e));
    }
    for (uint8_t i = 0; i < MIN(VIAL_COMBO_ENTRIES, SVAL_COMBO_ENTRIES); i++) {
        vial_combo_t       v;
        sval_combo_entry_t e;
        memcpy(&v, AT(VIAL_COMBO_ADDR) + i * sizeof(v), sizeof(v));
        for (uint8_t k = 0; k < 4; k++) e.input[k] = translate_keycode(v.input[k]);
        e.output            = translate_keycode(v.output);
        e.custom_combo_term = e.input[0] ? 0x8000 : 0; // enabled, default timing
        eeconfig_update_kb_datablock(&e, SVAL_COMBO_OFFSET + i * sizeof(e), sizeof(e));
    }
    for (uint8_t i = 0; i < MIN(VIAL_KEY_OVERRIDE_ENTRIES, SVAL_KEY_OVERRIDE_ENTRIES); i++) {
        vial_key_override_t       v;
        sval_key_override_entry_t e;
        memcpy(&v, AT(VIAL_KEY_OVERRIDE_ADDR) + i * sizeof(v), sizeof(v));
        e.trigger           = translate_keycode(v.trigger);
        e.replacement       = translate_keycode(v.replacement);
        e.layers            = v.layers;
        e.trigger_mods      = v.trigger_mods;
        e.negative_mod_mask = v.negative_mod_mask;
        e.suppressed_mods   = v.suppressed_mods;
        e.options           = v.options;
        eeconfig_update_kb_datablock(&e, SVAL_KEY_OVERRIDE_OFFSET + i * sizeof(e), sizeof(e));
    }
    for (uint8_t i = 0; i < MIN(VIAL_ALT_REPEAT_ENTRIES, SVAL_ALT_REPEAT_KEY_ENTRIES); i++) {
        vial_alt_repeat_t           v;
        sval_alt_repeat_key_entry_t e;
        memcpy(&v, AT(VIAL_ALT_REPEAT_ADDR) + i * sizeof(v), sizeof(v));
        e.keycode      = translate_keycode(v.keycode);
        e.alt_keycode  = translate_keycode(v.alt_keycode);
        e.allowed_mods = v.allowed_mods;
        e.options      = v.options;
        eeconfig_update_kb_datablock(&e, SVAL_ALT_REPEAT_KEY_OFFSET + i * sizeof(e), sizeof(e));
    }

    diag_stage(STAGE_KEYMAP);
    // Keymap: same geometry, keycodes stored big-endian.
    uint8_t *keymap = AT(VIAL_KEYMAP_ADDR);
    for (uint16_t i = 0; i < VIAL_KEYMAP_SIZE; i += 2) {
        uint16_t kc   = translate_keycode((keymap[i] << 8) | keymap[i + 1]);
        keymap[i]     = kc >> 8;
        keymap[i + 1] = kc & 0xFF;
    }
    dynamic_keymap_set_buffer(0, VIAL_KEYMAP_SIZE, keymap);

    diag_stage(STAGE_MACROS);
    // Macros: identical encoding apart from custom keycodes.
    uint8_t *macros = AT(VIAL_MACRO_ADDR);
    translate_macros(macros, macro_len);
    dynamic_keymap_macro_reset();
    dynamic_keymap_macro_set_buffer(0, fit_macros(macros, macro_len, dynamic_keymap_macro_get_buffer_size()), macros);

    diag_stage(STAGE_SETTINGS);
    // Keyboard settings: this firmware's defaults with Vial's values on top.
    vial_saved_values_t v;
    memcpy(&v, AT(VIAL_KB_DATA_ADDR), sizeof(v));
    vial_upgrade(&v);
    svalboard_saved_values_defaults();
    global_saved_values.left_scroll      = v.flags & 0x01;
    global_saved_values.right_scroll     = v.flags & 0x02;
    global_saved_values.axis_scroll_lock = v.flags & 0x04;
    global_saved_values.auto_mouse       = v.flags & 0x08;
    global_saved_values.left_dpi_index   = v.left_dpi_index;
    global_saved_values.right_dpi_index  = v.right_dpi_index;
    global_saved_values.mh_timer_index   = v.mh_timer_index;
    global_saved_values.turbo_scan       = v.turbo_scan;
    memcpy(global_saved_values.layer_colors, v.layer_colors, sizeof(v.layer_colors));
    write_eeprom_kb();

#undef AT
    free(snap);

    // Stamps last: only a fully written setup is ever trusted.
    sval_eeprom_set_valid();
    svalboard_eeprom_set_valid();
    via_eeprom_set_valid(true);
    diag_stage(STAGE_DONE);
}
