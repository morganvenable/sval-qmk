// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "viable.h"
#include "client_wrapper.h"
#include "quantum.h"
#include "via.h"
#include "dynamic_keymap.h"
#include "raw_hid.h"
#include "eeprom.h"
#include <string.h>
#include "print.h"
#include "version.h"
#include "send_string.h"
#include "wait.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 0, 0);

// Magic position for keycode execution
#define VIABLE_MATRIX_MAGIC 240

// Global for keycode override during tap dance execution
uint16_t g_viable_magic_keycode_override;

// Label System v2: Fixed VIABLE_LABEL_SIZE-byte UTF-8 storage arrays
char viable_td_labels[VIABLE_TAP_DANCE_ENTRIES][VIABLE_LABEL_SIZE];
char viable_macro_labels[DYNAMIC_KEYMAP_MACRO_COUNT][VIABLE_LABEL_SIZE];
char viable_layer_labels[DYNAMIC_KEYMAP_LAYER_COUNT][VIABLE_LABEL_SIZE];

// Internal EEPROM access functions - uses eeconfig_kb_datablock
static void viable_read_eeprom(uint16_t offset, void *buf, uint16_t size) {
    eeconfig_read_kb_datablock(buf, offset, size);
}

static void viable_write_eeprom(uint16_t offset, const void *buf, uint16_t size) {
    eeconfig_update_kb_datablock(buf, offset, size);
}

// Magic header for EEPROM validation - derived from QMK_BUILDDATE
// QMK_BUILDDATE format: "2019-11-05-11:29:54"
// Use full timestamp (date + time) so every build gets unique magic
static void viable_get_magic(uint8_t *magic) {
    char *p = QMK_BUILDDATE;
    // Validate string length to prevent out-of-bounds access
    size_t len = strlen(p);
    if (len < 19) {
        memset(magic, 0, VIABLE_MAGIC_SIZE);
        return;
    }
    magic[0] = ((p[2] & 0x0F) << 4) | (p[3] & 0x0F);  // year low 2 digits
    magic[1] = ((p[5] & 0x0F) << 4) | (p[6] & 0x0F);  // month
    magic[2] = ((p[8] & 0x0F) << 4) | (p[9] & 0x0F);  // day
    magic[3] = ((p[11] & 0x0F) << 4) | (p[12] & 0x0F); // hour
    magic[4] = ((p[14] & 0x0F) << 4) | (p[15] & 0x0F); // minute
    magic[5] = ((p[17] & 0x0F) << 4) | (p[18] & 0x0F); // second
}

static bool viable_eeprom_is_valid(void) {
    uint8_t stored[VIABLE_MAGIC_SIZE];
    uint8_t expected[VIABLE_MAGIC_SIZE];
    viable_read_eeprom(VIABLE_MAGIC_OFFSET, stored, VIABLE_MAGIC_SIZE);
    viable_get_magic(expected);
    return memcmp(stored, expected, VIABLE_MAGIC_SIZE) == 0;
}

static void viable_eeprom_set_valid(void) {
    uint8_t magic[VIABLE_MAGIC_SIZE];
    viable_get_magic(magic);
    viable_write_eeprom(VIABLE_MAGIC_OFFSET, magic, VIABLE_MAGIC_SIZE);
}

void viable_init(void) {
    // Initialize client wrapper for multi-client support
    client_wrapper_init();

    // Check if EEPROM data is valid (matches current firmware version)
    if (!viable_eeprom_is_valid()) {
        // Reset all viable data to defaults
        viable_reset();
        viable_qmk_settings_reset();
        // Mark as valid
        viable_eeprom_set_valid();
    }

    viable_reload_tap_dance();
    viable_reload_combo();
    viable_reload_key_override();
    viable_reload_alt_repeat_key();
    viable_reload_leader();
    viable_reload_labels();
    viable_qmk_settings_init();
}

// Weak keyboard post-init hook
__attribute__((weak)) void keyboard_post_init_core_kb(void) {}

// Module hook for post-init (named after directory: viable-kb/core)
void keyboard_post_init_core(void) {
    keyboard_post_init_core_kb();
    viable_init();
}

// Override QMK's get_oneshot_timeout for runtime configuration
// TEMPORARILY DISABLED - may be called before EEPROM ready
// uint16_t get_oneshot_timeout(void) {
//     viable_one_shot_t settings;
//     viable_get_one_shot(&settings);
//     return settings.timeout;
// }

// Get feature flags based on what's enabled
uint8_t viable_get_feature_flags(void) {
    uint8_t flags = 0;
#ifdef CAPS_WORD_ENABLE
    flags |= viable_flag_caps_word;
#endif
#ifdef LAYER_LOCK_ENABLE
    flags |= viable_flag_layer_lock;
#endif
#ifdef ONESHOT_ENABLE
    flags |= viable_flag_oneshot;
#endif
#ifdef LEADER_ENABLE
    flags |= viable_flag_leader;
#endif
    return flags;
}

// Storage functions - Tap Dance
int viable_get_tap_dance(uint8_t index, viable_tap_dance_entry_t *entry) {
    if (index >= VIABLE_TAP_DANCE_ENTRIES) return -1;
    viable_read_eeprom(VIABLE_TAP_DANCE_OFFSET + index * sizeof(viable_tap_dance_entry_t),
                       entry, sizeof(viable_tap_dance_entry_t));
    return 0;
}

int viable_set_tap_dance(uint8_t index, const viable_tap_dance_entry_t *entry) {
    if (index >= VIABLE_TAP_DANCE_ENTRIES) return -1;
    viable_write_eeprom(VIABLE_TAP_DANCE_OFFSET + index * sizeof(viable_tap_dance_entry_t),
                        entry, sizeof(viable_tap_dance_entry_t));
    return 0;
}

// Storage functions - Combo
int viable_get_combo(uint8_t index, viable_combo_entry_t *entry) {
    if (index >= VIABLE_COMBO_ENTRIES) return -1;
    viable_read_eeprom(VIABLE_COMBO_OFFSET + index * sizeof(viable_combo_entry_t),
                       entry, sizeof(viable_combo_entry_t));
    return 0;
}

int viable_set_combo(uint8_t index, const viable_combo_entry_t *entry) {
    if (index >= VIABLE_COMBO_ENTRIES) return -1;
    viable_write_eeprom(VIABLE_COMBO_OFFSET + index * sizeof(viable_combo_entry_t),
                        entry, sizeof(viable_combo_entry_t));
    return 0;
}

// Storage functions - Key Override
int viable_get_key_override(uint8_t index, viable_key_override_entry_t *entry) {
    if (index >= VIABLE_KEY_OVERRIDE_ENTRIES) return -1;
    viable_read_eeprom(VIABLE_KEY_OVERRIDE_OFFSET + index * sizeof(viable_key_override_entry_t),
                       entry, sizeof(viable_key_override_entry_t));
    return 0;
}

int viable_set_key_override(uint8_t index, const viable_key_override_entry_t *entry) {
    if (index >= VIABLE_KEY_OVERRIDE_ENTRIES) return -1;
    viable_write_eeprom(VIABLE_KEY_OVERRIDE_OFFSET + index * sizeof(viable_key_override_entry_t),
                        entry, sizeof(viable_key_override_entry_t));
    return 0;
}

// Storage functions - Alt Repeat Key
int viable_get_alt_repeat_key(uint8_t index, viable_alt_repeat_key_entry_t *entry) {
    if (index >= VIABLE_ALT_REPEAT_KEY_ENTRIES) return -1;
    viable_read_eeprom(VIABLE_ALT_REPEAT_KEY_OFFSET + index * sizeof(viable_alt_repeat_key_entry_t),
                       entry, sizeof(viable_alt_repeat_key_entry_t));
    return 0;
}

int viable_set_alt_repeat_key(uint8_t index, const viable_alt_repeat_key_entry_t *entry) {
    if (index >= VIABLE_ALT_REPEAT_KEY_ENTRIES) return -1;
    viable_write_eeprom(VIABLE_ALT_REPEAT_KEY_OFFSET + index * sizeof(viable_alt_repeat_key_entry_t),
                        entry, sizeof(viable_alt_repeat_key_entry_t));
    return 0;
}

// Storage functions - One-Shot
void viable_get_one_shot(viable_one_shot_t *settings) {
    viable_read_eeprom(VIABLE_ONE_SHOT_OFFSET, settings, sizeof(viable_one_shot_t));
}

void viable_set_one_shot(const viable_one_shot_t *settings) {
    viable_write_eeprom(VIABLE_ONE_SHOT_OFFSET, settings, sizeof(viable_one_shot_t));
}

// Storage functions - Leader
int viable_get_leader(uint8_t index, viable_leader_entry_t *entry) {
    if (index >= VIABLE_LEADER_ENTRIES) return -1;
    viable_read_eeprom(VIABLE_LEADER_OFFSET + index * sizeof(viable_leader_entry_t),
                       entry, sizeof(viable_leader_entry_t));
    return 0;
}

int viable_set_leader(uint8_t index, const viable_leader_entry_t *entry) {
    if (index >= VIABLE_LEADER_ENTRIES) return -1;
    viable_write_eeprom(VIABLE_LEADER_OFFSET + index * sizeof(viable_leader_entry_t),
                        entry, sizeof(viable_leader_entry_t));
    return 0;
}

// Storage functions - Labels (v2: fixed VIABLE_LABEL_SIZE-byte arrays)

// Helper: Get pointer to label array and count for a given type
static char* viable_get_label_array(uint8_t label_type, uint8_t *count, uint16_t *eeprom_offset) {
    switch (label_type) {
        case viable_label_type_layer:
            *count = DYNAMIC_KEYMAP_LAYER_COUNT;
            *eeprom_offset = VIABLE_LAYER_LABEL_OFFSET;
            return (char*)viable_layer_labels;
        case viable_label_type_tap_dance:
            *count = VIABLE_TAP_DANCE_ENTRIES;
            *eeprom_offset = VIABLE_TD_LABEL_OFFSET;
            return (char*)viable_td_labels;
        case viable_label_type_macro:
            *count = DYNAMIC_KEYMAP_MACRO_COUNT;
            *eeprom_offset = VIABLE_MACRO_LABEL_OFFSET;
            return (char*)viable_macro_labels;
        default:
            *count = 0;
            *eeprom_offset = 0;
            return NULL;
    }
}

// Helper: Check if label is empty (all bytes are 0x00)
static bool viable_label_is_empty(const char *label) {
    for (uint8_t i = 0; i < VIABLE_LABEL_SIZE; i++) {
        if (label[i] != 0x00) return false;
    }
    return true;
}

uint8_t viable_get_label(uint8_t label_type, uint8_t index, char *buffer, uint8_t buffer_size) {
    if (!buffer || buffer_size == 0) return 0;

    uint8_t count;
    uint16_t eeprom_offset;
    char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) {
        buffer[0] = '\0';
        return 0;
    }

    // Copy label from RAM array (exactly VIABLE_LABEL_SIZE bytes)
    char *label = labels + (index * VIABLE_LABEL_SIZE);

    // Find actual length (excluding trailing nulls)
    uint8_t len = 0;
    for (uint8_t i = 0; i < VIABLE_LABEL_SIZE; i++) {
        if (label[i] != 0x00) {
            len = i + 1;
        }
    }

    // Copy to buffer (truncate if needed)
    if (len > buffer_size) len = buffer_size;
    if (len > 0) {
        memcpy(buffer, label, len);
    }
    if (len < buffer_size) {
        buffer[len] = '\0';
    }

    return len;
}

int viable_set_label(uint8_t label_type, uint8_t index, const char *string, uint8_t length) {
    if (!string) return -1;

    uint8_t count;
    uint16_t eeprom_offset;
    char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) return -1;

    // Truncate to VIABLE_LABEL_SIZE bytes max
    if (length > VIABLE_LABEL_SIZE) length = VIABLE_LABEL_SIZE;

    // Get pointer to label slot
    char *label = labels + (index * VIABLE_LABEL_SIZE);

    // Copy label data and null-pad remainder
    memcpy(label, string, length);
    if (length < VIABLE_LABEL_SIZE) {
        memset(label + length, 0x00, VIABLE_LABEL_SIZE - length);
    }

    // Write to EEPROM
    viable_write_eeprom(eeprom_offset + (index * VIABLE_LABEL_SIZE), label, VIABLE_LABEL_SIZE);

    return 0;
}

int viable_clear_label(uint8_t label_type, uint8_t index) {
    uint8_t count;
    uint16_t eeprom_offset;
    char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) return -1;

    // Get pointer to label slot and clear it
    char *label = labels + (index * VIABLE_LABEL_SIZE);
    memset(label, 0x00, VIABLE_LABEL_SIZE);

    // Write to EEPROM
    viable_write_eeprom(eeprom_offset + (index * VIABLE_LABEL_SIZE), label, VIABLE_LABEL_SIZE);

    return 0;
}

// Reload all label arrays from EEPROM into RAM
void viable_reload_labels(void) {
    // Load layer labels
    viable_read_eeprom(VIABLE_LAYER_LABEL_OFFSET, viable_layer_labels,
                      DYNAMIC_KEYMAP_LAYER_COUNT * VIABLE_LABEL_SIZE);

    // Load tap dance labels
    viable_read_eeprom(VIABLE_TD_LABEL_OFFSET, viable_td_labels,
                      VIABLE_TAP_DANCE_ENTRIES * VIABLE_LABEL_SIZE);

    // Load macro labels
    viable_read_eeprom(VIABLE_MACRO_LABEL_OFFSET, viable_macro_labels,
                      DYNAMIC_KEYMAP_MACRO_COUNT * VIABLE_LABEL_SIZE);
}

void viable_save(void) {
    // Data is written directly to EEPROM, nothing additional to flush
}

void viable_reset(void) {
    // Zero out all EEPROM storage
    uint8_t zero[16] = {0};
    for (uint16_t i = 0; i < VIABLE_EEPROM_SIZE; i += sizeof(zero)) {
        uint16_t chunk = sizeof(zero);
        if (i + chunk > VIABLE_EEPROM_SIZE) {
            chunk = VIABLE_EEPROM_SIZE - i;
        }
        viable_write_eeprom(i, zero, chunk);
    }
    // Reset VIA dynamic keymap and macros
    dynamic_keymap_reset();
    dynamic_keymap_macro_reset();
    viable_reload_tap_dance();
    viable_reload_combo();
    viable_reload_key_override();
    viable_reload_alt_repeat_key();
    viable_reload_leader();
}

// Keycode execution helpers
void viable_keycode_down(uint16_t keycode) {
    g_viable_magic_keycode_override = keycode;

    if (keycode <= QK_MODS_MAX) {
        register_code16(keycode);
    } else {
        action_exec((keyevent_t){
            .type = KEY_EVENT,
            .key = (keypos_t){.row = VIABLE_MATRIX_MAGIC, .col = VIABLE_MATRIX_MAGIC},
            .pressed = 1,
            .time = (timer_read() | 1)
        });
    }
}

void viable_keycode_up(uint16_t keycode) {
    g_viable_magic_keycode_override = keycode;

    if (keycode <= QK_MODS_MAX) {
        unregister_code16(keycode);
    } else {
        action_exec((keyevent_t){
            .type = KEY_EVENT,
            .key = (keypos_t){.row = VIABLE_MATRIX_MAGIC, .col = VIABLE_MATRIX_MAGIC},
            .pressed = 0,
            .time = (timer_read() | 1)
        });
    }
}

void viable_keycode_tap(uint16_t keycode) {
    viable_keycode_down(keycode);
    wait_ms(TAP_CODE_DELAY);
    viable_keycode_up(keycode);
}

// 0xDF Protocol handler
// This function should be called from via_command_kb() in the keyboard code
bool viable_handle_command(uint8_t *data, uint8_t length) {
    // data[0] = 0xDF (VIABLE_PREFIX) - already verified by caller
    // data[1] = command_id
    // data[2...] = payload

    uint8_t command_id = data[1];

    switch (command_id) {
        case viable_cmd_get_info: {
            // Response: [0xDF] [0x00] [ver0-3] [uid0-7] [flags]
            // Entry counts are now in viable.json (parsed from keyboard definition)
            uint8_t uid[] = VIABLE_KEYBOARD_UID;
            data[2] = VIABLE_PROTOCOL_VERSION & 0xFF;
            data[3] = (VIABLE_PROTOCOL_VERSION >> 8) & 0xFF;
            data[4] = (VIABLE_PROTOCOL_VERSION >> 16) & 0xFF;
            data[5] = (VIABLE_PROTOCOL_VERSION >> 24) & 0xFF;
            memcpy(&data[6], uid, 8);
            data[14] = viable_get_feature_flags();
            break;
        }

        case viable_cmd_tap_dance_get: {
            // Request: [0xDF] [0x01] [index]
            // Response: [0xDF] [0x01] [index] [10 bytes entry]
            uint8_t idx = data[2];
            viable_tap_dance_entry_t entry = {0};
            viable_get_tap_dance(idx, &entry);
            memcpy(&data[3], &entry, sizeof(entry));
            break;
        }

        case viable_cmd_tap_dance_set: {
            // Request: [0xDF] [0x02] [index] [10 bytes entry]
            // Response: [0xDF] [0x02] [status]
            if (length < 13) {
                data[1] = viable_cmd_error;
                return false;
            }
            uint8_t idx = data[2];
            viable_tap_dance_entry_t entry;
            memcpy(&entry, &data[3], sizeof(entry));
            data[2] = viable_set_tap_dance(idx, &entry) == 0 ? 0 : 1;
            viable_reload_tap_dance();
            break;
        }

        case viable_cmd_combo_get: {
            // Request: [0xDF] [0x03] [index]
            // Response: [0xDF] [0x03] [index] [12 bytes entry]
            uint8_t idx = data[2];
            viable_combo_entry_t entry = {0};
            viable_get_combo(idx, &entry);
            memcpy(&data[3], &entry, sizeof(entry));
            break;
        }

        case viable_cmd_combo_set: {
            // Request: [0xDF] [0x04] [index] [12 bytes entry]
            // Response: [0xDF] [0x04] [status]
            if (length < 15) {
                data[1] = viable_cmd_error;
                return false;
            }
            uint8_t idx = data[2];
            viable_combo_entry_t entry;
            memcpy(&entry, &data[3], sizeof(entry));
            data[2] = viable_set_combo(idx, &entry) == 0 ? 0 : 1;
            viable_reload_combo();
            break;
        }

        case viable_cmd_key_override_get: {
            // Request: [0xDF] [0x05] [index]
            // Response: [0xDF] [0x05] [index] [12 bytes entry]
            uint8_t idx = data[2];
            viable_key_override_entry_t entry = {0};
            viable_get_key_override(idx, &entry);
            memcpy(&data[3], &entry, sizeof(entry));
            break;
        }

        case viable_cmd_key_override_set: {
            // Request: [0xDF] [0x06] [index] [12 bytes entry]
            // Response: [0xDF] [0x06] [status]
            if (length < 15) {
                data[1] = viable_cmd_error;
                return false;
            }
            uint8_t idx = data[2];
            viable_key_override_entry_t entry;
            memcpy(&entry, &data[3], sizeof(entry));
            data[2] = viable_set_key_override(idx, &entry) == 0 ? 0 : 1;
            viable_reload_key_override();
            break;
        }

        case viable_cmd_alt_repeat_key_get: {
            // Request: [0xDF] [0x07] [index]
            // Response: [0xDF] [0x07] [index] [6 bytes entry]
            uint8_t idx = data[2];
            viable_alt_repeat_key_entry_t entry = {0};
            viable_get_alt_repeat_key(idx, &entry);
            memcpy(&data[3], &entry, sizeof(entry));
            break;
        }

        case viable_cmd_alt_repeat_key_set: {
            // Request: [0xDF] [0x08] [index] [6 bytes entry]
            // Response: [0xDF] [0x08] [status]
            if (length < 9) {
                data[1] = viable_cmd_error;
                return false;
            }
            uint8_t idx = data[2];
            viable_alt_repeat_key_entry_t entry;
            memcpy(&entry, &data[3], sizeof(entry));
            data[2] = viable_set_alt_repeat_key(idx, &entry) == 0 ? 0 : 1;
            viable_reload_alt_repeat_key();
            break;
        }

        case viable_cmd_one_shot_get: {
            // Request: [0xDF] [0x09]
            // Response: [0xDF] [0x09] [timeout_lo] [timeout_hi] [tap_toggle]
            viable_one_shot_t settings = {0};
            viable_get_one_shot(&settings);
            data[2] = settings.timeout & 0xFF;
            data[3] = (settings.timeout >> 8) & 0xFF;
            data[4] = settings.tap_toggle;
            break;
        }

        case viable_cmd_one_shot_set: {
            // Request: [0xDF] [0x0A] [timeout_lo] [timeout_hi] [tap_toggle]
            // Response: [0xDF] [0x0A]
            viable_one_shot_t settings;
            settings.timeout = data[2] | (data[3] << 8);
            settings.tap_toggle = data[4];
            viable_set_one_shot(&settings);
            break;
        }

        case viable_cmd_save: {
            // Request: [0xDF] [0x0B]
            // Response: [0xDF] [0x0B]
            viable_save();
            break;
        }

        case viable_cmd_reset: {
            // Request: [0xDF] [0x0C]
            // Response: [0xDF] [0x0C]
            viable_reset();
            break;
        }

        case viable_cmd_definition_size: {
            // Request: [0xDF] [0x0D]
            // Response: [0xDF] [0x0D] [size0] [size1] [size2] [size3]
            uint32_t size = viable_get_definition_size();
            data[2] = size & 0xFF;
            data[3] = (size >> 8) & 0xFF;
            data[4] = (size >> 16) & 0xFF;
            data[5] = (size >> 24) & 0xFF;
            break;
        }

        case viable_cmd_definition_chunk: {
            // Request: [0xDF] [0x0E] [offset_lo] [offset_hi] [size]
            // Response: [0xDF] [0x0E] [offset_lo] [offset_hi] [actual_size] [data...]
            uint16_t offset = data[2] | (data[3] << 8);
            uint8_t requested_size = data[4];

            // Clamp to maximum chunk size
            if (requested_size == 0 || requested_size > VIABLE_DEFINITION_CHUNK_SIZE) {
                requested_size = VIABLE_DEFINITION_CHUNK_SIZE;
            }

            // Validate offset to prevent overflow
            uint32_t definition_size = viable_get_definition_size();
            if (offset >= definition_size) {
                data[4] = 0;
                break;
            }

            uint8_t actual_size = viable_get_definition_chunk(offset, &data[5], requested_size);
            data[4] = actual_size;
            break;
        }

        case viable_cmd_qmk_settings_query: {
            // Request: [0xDF] [0x10] [qsid_lo] [qsid_hi]
            // Response: [0xDF] [0x10] [qsid1_lo] [qsid1_hi] [qsid2_lo] ... [0xFF] [0xFF]
            uint16_t qsid_gt = data[2] | (data[3] << 8);
            viable_qmk_settings_query(qsid_gt, &data[2], length - 2);
            break;
        }

        case viable_cmd_qmk_settings_get: {
            // Request: [0xDF] [0x11] [qsid_lo] [qsid_hi]
            // Response: [0xDF] [0x11] [status] [value bytes...]
            uint16_t qsid = data[2] | (data[3] << 8);
            data[2] = viable_qmk_settings_get(qsid, &data[3], length - 3);
            break;
        }

        case viable_cmd_qmk_settings_set: {
            // Request: [0xDF] [0x12] [qsid_lo] [qsid_hi] [value bytes...]
            // Response: [0xDF] [0x12] [status]
            uint16_t qsid = data[2] | (data[3] << 8);
            data[2] = viable_qmk_settings_set(qsid, &data[4], length - 4);
            break;
        }

        case viable_cmd_qmk_settings_reset: {
            // Request: [0xDF] [0x13]
            // Response: [0xDF] [0x13]
            viable_qmk_settings_reset();
            break;
        }

        case viable_cmd_leader_get: {
            // Request: [0xDF] [0x14] [index]
            // Response: [0xDF] [0x14] [index] [14 bytes entry]
            uint8_t idx = data[2];
            viable_leader_entry_t entry = {0};
            viable_get_leader(idx, &entry);
            memcpy(&data[3], &entry, sizeof(entry));
            break;
        }

        case viable_cmd_leader_set: {
            // Request: [0xDF] [0x15] [index] [14 bytes entry]
            // Response: [0xDF] [0x15] [status]
            if (length < 17) {
                data[1] = viable_cmd_error;
                return false;
            }
            uint8_t idx = data[2];
            viable_leader_entry_t entry;
            memcpy(&entry, &data[3], sizeof(entry));
            data[2] = viable_set_leader(idx, &entry) == 0 ? 0 : 1;
            viable_reload_leader();
            break;
        }

        case viable_cmd_layer_state_get: {
            // Request: [0xDF] [0x16]
            // Response: [0xDF] [0x16] [state0] [state1] [state2] [state3]
            uint32_t state = layer_state;
            data[2] = state & 0xFF;
            data[3] = (state >> 8) & 0xFF;
            data[4] = (state >> 16) & 0xFF;
            data[5] = (state >> 24) & 0xFF;
            break;
        }

        case viable_cmd_layer_state_set: {
            // Request: [0xDF] [0x17] [state0] [state1] [state2] [state3]
            // Response: [0xDF] [0x17]
            uint32_t new_state = data[2] |
                                (data[3] << 8) |
                                (data[4] << 16) |
                                (data[5] << 24);
            layer_state_set(new_state);
            break;
        }

        case viable_cmd_fragment_get_hardware:
            return viable_handle_fragment_get_hardware(data, length);

        case viable_cmd_fragment_get_selections:
            return viable_handle_fragment_get_selections(data, length);

        case viable_cmd_fragment_set_selections:
            return viable_handle_fragment_set_selections(data, length);

        case viable_cmd_label_get: {
            // LABEL_GET_BULK with chunking (v2.1)
            // Request: [0xDF] [0x1B] [type] [offset]
            //   offset = starting index to scan from (default 0 for backwards compat)
            // Response: [0xDF] [0x1B] [type] [flags] [bitmap...] [labels...]
            //   flags bit 0: more data available (1 = continue, 0 = done)
            //   bitmap only included when offset == 0
            if (length < 3) {
                data[1] = viable_cmd_error;
                return false;
            }

            uint8_t label_type = data[2];
            uint8_t start_offset = (length > 3) ? data[3] : 0;  // Default 0 for backwards compat
            uint8_t count;
            uint16_t eeprom_offset;
            char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);

            if (!labels) {
                data[1] = viable_cmd_error;
                return false;
            }

            // Clamp offset to valid range
            if (start_offset >= count) {
                start_offset = count;
            }

            // Build bitmap (ceil(count/8) bytes) - only needed on first chunk
            uint8_t bitmap_size = (count + 7) / 8;
            uint8_t bitmap[7] = {0};  // Max 7 bytes for 50 entries

            for (uint8_t i = 0; i < count; i++) {
                if (!viable_label_is_empty(labels + (i * VIABLE_LABEL_SIZE))) {
                    uint8_t byte_idx = i / 8;
                    uint8_t bit_idx = 7 - (i % 8);  // MSB to LSB
                    bitmap[byte_idx] |= (1 << bit_idx);
                }
            }

            // Build response: [0xDF] [0x1B] [type] [flags] [bitmap?] [labels...]
            // Byte 3 reserved for flags (set at end)
            uint8_t resp_offset = 4;

            // Include bitmap only on first chunk (offset == 0)
            if (start_offset == 0) {
                memcpy(&data[resp_offset], bitmap, bitmap_size);
                resp_offset += bitmap_size;
            }

            // Send non-empty labels starting from offset
            uint8_t flags = 0;
            for (uint8_t i = start_offset; i < count; i++) {
                if (!viable_label_is_empty(labels + (i * VIABLE_LABEL_SIZE))) {
                    if (resp_offset + VIABLE_LABEL_SIZE <= length) {
                        memcpy(&data[resp_offset], labels + (i * VIABLE_LABEL_SIZE), VIABLE_LABEL_SIZE);
                        resp_offset += VIABLE_LABEL_SIZE;
                    } else {
                        // Packet full, more data available
                        flags = 0x01;
                        break;
                    }
                }
            }

            data[3] = flags;  // Set continuation flag

            break;
        }

        case viable_cmd_label_set: {
            // LABEL_SET (v2)
            // Request: [0xDF] [0x1C] [type] [index] [VIABLE_LABEL_SIZE-byte label]
            // Response: [0xDF] [0x1C] [status]
            if (length < 4 + VIABLE_LABEL_SIZE) {  // 2 header + 2 params + label bytes
                data[1] = viable_cmd_error;
                return false;
            }

            uint8_t label_type = data[2];
            uint8_t index = data[3];
            const uint8_t *label = &data[4];

            // Validate UTF-8 sequences
            // Accept: valid UTF-8 (including multi-byte), 0x00 as null terminator
            // Reject: C0/C1 overlong encodings, F5+ invalid lead bytes,
            //         invalid continuation bytes, control chars 0x01-0x1F (except tab)
            {
                bool in_null_tail = false;
                uint8_t i = 0;
                while (i < VIABLE_LABEL_SIZE) {
                    uint8_t c = label[i];
                    if (c == 0x00) {
                        // Null byte: everything after must also be null
                        in_null_tail = true;
                        i++;
                        continue;
                    }
                    if (in_null_tail) {
                        // Non-null byte after null — invalid
                        data[2] = 0x03;
                        return true;
                    }
                    if (c >= 0x01 && c <= 0x1F && c != 0x09) {
                        // Control characters (except tab) — reject
                        data[2] = 0x03;
                        return true;
                    }
                    if (c == 0x7F) {
                        // DEL — reject
                        data[2] = 0x03;
                        return true;
                    }
                    if (c <= 0x7F) {
                        // Valid single-byte ASCII (0x20-0x7E, 0x09)
                        i++;
                        continue;
                    }
                    // Multi-byte UTF-8 sequence
                    uint8_t expected_cont = 0;
                    if (c >= 0xC2 && c <= 0xDF) {
                        expected_cont = 1;  // 2-byte sequence
                    } else if (c >= 0xE0 && c <= 0xEF) {
                        expected_cont = 2;  // 3-byte sequence
                    } else if (c >= 0xF0 && c <= 0xF4) {
                        expected_cont = 3;  // 4-byte sequence
                    } else {
                        // Invalid lead byte (0x80-0xC1, 0xF5+)
                        data[2] = 0x03;
                        return true;
                    }
                    // Check continuation bytes exist and are valid (0x80-0xBF)
                    if (i + expected_cont >= VIABLE_LABEL_SIZE) {
                        // Truncated sequence — reject
                        data[2] = 0x03;
                        return true;
                    }
                    for (uint8_t j = 1; j <= expected_cont; j++) {
                        uint8_t cb = label[i + j];
                        if (cb < 0x80 || cb > 0xBF) {
                            data[2] = 0x03;
                            return true;
                        }
                    }
                    // Reject overlong 3-byte sequences (E0 80-9F xx)
                    if (c == 0xE0 && label[i + 1] < 0xA0) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject surrogates (ED A0-BF xx)
                    if (c == 0xED && label[i + 1] > 0x9F) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject overlong 4-byte sequences (F0 80-8F xx xx)
                    if (c == 0xF0 && label[i + 1] < 0x90) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject codepoints above U+10FFFF (F4 90+ xx xx)
                    if (c == 0xF4 && label[i + 1] > 0x8F) {
                        data[2] = 0x03;
                        return true;
                    }
                    i += 1 + expected_cont;
                }
            }

            // Attempt to set label
            int result = viable_set_label(label_type, index, (const char*)label, VIABLE_LABEL_SIZE);

            // Map result to status code
            if (result == 0) {
                data[2] = 0x00;  // Success
            } else {
                // Determine error type by checking parameters
                uint8_t count;
                uint16_t eeprom_offset;
                char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);
                if (!labels) {
                    data[2] = 0x01;  // Invalid type
                } else if (index >= count) {
                    data[2] = 0x02;  // Index out of range
                } else {
                    data[2] = 0x01;  // Generic error
                }
            }
            break;
        }

        case viable_cmd_label_clear: {
            // LABEL_CLEAR (v2)
            // Request: [0xDF] [0x1D] [type] [index]
            // Response: [0xDF] [0x1D] [status]
            if (length < 4) {
                data[1] = viable_cmd_error;
                return false;
            }

            uint8_t label_type = data[2];
            uint8_t index = data[3];

            // Attempt to clear label
            int result = viable_clear_label(label_type, index);

            // Map result to status code
            if (result == 0) {
                data[2] = 0x00;  // Success
            } else {
                // Determine error type by checking parameters
                uint8_t count;
                uint16_t eeprom_offset;
                char *labels = viable_get_label_array(label_type, &count, &eeprom_offset);
                if (!labels) {
                    data[2] = 0x01;  // Invalid type
                } else if (index >= count) {
                    data[2] = 0x02;  // Index out of range
                } else {
                    data[2] = 0x01;  // Generic error
                }
            }
            break;
        }

        default:
            // Unknown command - set error response
            data[1] = viable_cmd_error;
            return false;
    }

    return true;
}

// Override via_command_kb to intercept wrapper and Viable protocol
bool via_command_kb(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case WRAPPER_PREFIX:  // 0xDD - Client ID wrapper
            return client_wrapper_receive(data, length);

        case VIABLE_PREFIX:  // 0xDF - Legacy Viable - REJECTED (wrapper required)
            return true;  // "Handled" by ignoring

        default:
            return false;  // Let VIA handle
    }
}

// Process record hook for Viable features
bool process_record_viable(uint16_t keycode, keyrecord_t *record) {
    if (!process_record_viable_tap_dance(keycode, record)) {
        return false;
    }
    return true;
}

// Override keymap_key_to_keycode to handle magic position for tap dance/combo execution
uint16_t keymap_key_to_keycode(uint8_t layer, keypos_t key) {
    if (key.row == VIABLE_MATRIX_MAGIC && key.col == VIABLE_MATRIX_MAGIC) {
        return g_viable_magic_keycode_override;
    }
    // Use dynamic keymap for normal keys
    return dynamic_keymap_get_keycode(layer, key.row, key.col);
}

// Vial macro extension codes for 16-bit keycodes
#define VIAL_MACRO_EXT_TAP 5
#define VIAL_MACRO_EXT_DOWN 6
#define VIAL_MACRO_EXT_UP 7

static uint16_t decode_keycode(uint16_t kc) {
    // Map 0xFF01 => 0x0100; 0xFF02 => 0x0200, etc.
    if (kc > 0xFF00)
        return (kc & 0xFF) << 8;
    return kc;
}

// Override dynamic_keymap_macro_send to support extended keycodes and binary delay
void dynamic_keymap_macro_send(uint8_t id) {
    if (id >= dynamic_keymap_macro_get_count()) {
        return;
    }

    // Check the last byte of the buffer for validity
    uint16_t macro_size = dynamic_keymap_macro_get_buffer_size();
    uint8_t last_byte;
    dynamic_keymap_macro_get_buffer(macro_size - 1, 1, &last_byte);
    if (last_byte != 0) {
        return;
    }

    // Find the start of macro N by counting null terminators
    uint32_t offset = 0;
    while (id > 0) {
        if (offset >= macro_size) {
            return;
        }
        uint8_t byte;
        dynamic_keymap_macro_get_buffer(offset, 1, &byte);
        if (byte == 0) {
            --id;
        }
        ++offset;
    }

    // Process macro bytes
    char data[4] = {0, 0, 0, 0};
    while (1) {
        if (offset >= macro_size) {
            break;
        }
        memset(data, 0, sizeof(data));
        dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t*)&data[0]);
        if (data[0] == 0) {
            break;
        }
        if (data[0] == SS_QMK_PREFIX) {
            if (offset >= macro_size) break;
            dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t*)&data[1]);
            if (data[1] == 0)
                break;
            if (data[1] == SS_TAP_CODE || data[1] == SS_DOWN_CODE || data[1] == SS_UP_CODE) {
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t*)&data[2]);
                if (data[2] != 0)
                    send_string(data);
            } else if (data[1] == VIAL_MACRO_EXT_TAP || data[1] == VIAL_MACRO_EXT_DOWN || data[1] == VIAL_MACRO_EXT_UP) {
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t*)&data[2]);
                if (data[2] != 0) {
                    if (offset >= macro_size) break;
                    dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t*)&data[3]);
                    if (data[3] != 0) {
                        uint16_t kc;
                        memcpy(&kc, &data[2], sizeof(kc));
                        kc = decode_keycode(kc);
                        switch (data[1]) {
                        case VIAL_MACRO_EXT_TAP:
                            viable_keycode_tap(kc);
                            break;
                        case VIAL_MACRO_EXT_DOWN:
                            viable_keycode_down(kc);
                            break;
                        case VIAL_MACRO_EXT_UP:
                            viable_keycode_up(kc);
                            break;
                        }
                    }
                }
            } else if (data[1] == SS_DELAY_CODE) {
                uint8_t d0, d1;
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, &d0);
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, &d1);
                if (d0 == 0 || d1 == 0)
                    break;
                int ms = (d0 - 1) + (d1 - 1) * 255;
                wait_ms(ms);
            }
        } else {
            send_string_with_delay(data, TAP_CODE_DELAY);
        }
    }
}
