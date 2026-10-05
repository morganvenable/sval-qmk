// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"

#ifdef COMBO_ENABLE

#    include "process_combo.h"

// Bit mask for enabled flag in custom_combo_term
#    define SVAL_COMBO_ENABLED_BIT 0x8000
// Mask for timing value (bits 0-14)
#    define SVAL_COMBO_TIMING_MASK 0x7FFF

// Storage for combo key sequences (4 keys + COMBO_END terminator)
static uint16_t sval_combo_keys[SVAL_COMBO_ENTRIES][5];

// Storage for combo structures
static combo_t sval_combos[SVAL_COMBO_ENTRIES];

// Storage for custom combo terms (0 = use global default)
static uint16_t sval_combo_terms[SVAL_COMBO_ENTRIES];

// Track which combos are enabled
static bool sval_combo_enabled[SVAL_COMBO_ENTRIES];

void sval_reload_combo(void) {
    // Initialize with all keys = COMBO_END
    memset(sval_combo_keys, 0, sizeof(sval_combo_keys));
    memset(sval_combos, 0, sizeof(sval_combos));
    memset(sval_combo_terms, 0, sizeof(sval_combo_terms));
    memset(sval_combo_enabled, 0, sizeof(sval_combo_enabled));

    // Load from EEPROM
    for (size_t i = 0; i < SVAL_COMBO_ENTRIES; ++i) {
        uint16_t *seq       = sval_combo_keys[i];
        sval_combos[i].keys = seq;

        sval_combo_entry_t entry;
        if (sval_get_combo(i, &entry) == 0) {
            // Check if enabled (bit 15 of custom_combo_term)
            sval_combo_enabled[i] = (entry.custom_combo_term & SVAL_COMBO_ENABLED_BIT) != 0;

            if (sval_combo_enabled[i]) {
                memcpy(seq, entry.input, sizeof(entry.input));
                // Ensure null termination
                seq[4]                 = COMBO_END;
                sval_combos[i].keycode = entry.output;

                // Extract custom timing (bits 0-14), 0 means use global default
                sval_combo_terms[i] = entry.custom_combo_term & SVAL_COMBO_TIMING_MASK;
            } else {
                // Disabled combo: empty key sequence
                seq[0]                 = COMBO_END;
                sval_combos[i].keycode = KC_NO;
            }
        }
    }
}

// Override the introspection functions
uint16_t combo_count(void) {
    return SVAL_COMBO_ENTRIES;
}

combo_t *combo_get(uint16_t combo_idx) {
    if (combo_idx >= SVAL_COMBO_ENTRIES) {
        return NULL;
    }
    return &sval_combos[combo_idx];
}

// User hook: override this for custom per-combo timing logic
// Return 0 to use Sval's setting, or a positive value to override
__attribute__((weak)) uint16_t get_combo_term_sval(uint16_t combo_idx, combo_t *combo) {
    return 0; // Default: use Sval's setting
}

// Sval owns this function - user hook is checked FIRST
uint16_t get_combo_term(uint16_t combo_idx, combo_t *combo) {
    // User hook gets first priority
    uint16_t user_term = get_combo_term_sval(combo_idx, combo);
    if (user_term > 0) {
        return user_term;
    }

    // Then check for per-combo custom timing from Sval
    if (combo_idx < SVAL_COMBO_ENTRIES && sval_combo_terms[combo_idx] > 0) {
        return sval_combo_terms[combo_idx];
    }

    // Fall back to Sval's global setting
    return sval_get_combo_term();
}

#else
// Stubs when COMBO_ENABLE is not defined
void sval_reload_combo(void) {}
#endif
