// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "sval_qmk_settings.h"
#include "quantum.h"
#include "leader.h"

#ifdef LEADER_ENABLE

// Dynamic timeout variable - referenced by LEADER_TIMEOUT macro
uint16_t sval_leader_timeout = 300;

// RAM cache of leader entries for fast matching
static sval_leader_entry_t leader_entries[SVAL_LEADER_ENTRIES];

void sval_reload_leader(void) {
    // Update timeout from settings
    sval_leader_timeout = sval_get_leader_timeout();

    for (uint16_t i = 0; i < SVAL_LEADER_ENTRIES; i++) {
        sval_get_leader(i, &leader_entries[i]);
    }
}

// Per-key timing: reset timer on each keypress if enabled
bool leader_add_user(uint16_t keycode) {
    if (sval_get_leader_per_key_timing()) {
        leader_reset_timer();
    }
    return false; // Don't end sequence early
}

// Override QMK's leader_end_user callback
// Called when leader sequence is complete - search for match and execute
void leader_end_user(void) {
    for (uint16_t i = 0; i < SVAL_LEADER_ENTRIES; i++) {
        sval_leader_entry_t *entry = &leader_entries[i];

        // Skip disabled entries
        if (!(entry->options & sval_leader_enabled)) {
            continue;
        }

        // Count keys in this entry's sequence
        uint8_t entry_len = 0;
        for (uint8_t j = 0; j < 5; j++) {
            if (entry->sequence[j] == 0) break;
            entry_len++;
        }

        // Skip empty sequences
        if (entry_len == 0) continue;

        // Check if sequence matches using leader_sequence_N_keys pattern
        bool match = false;
        switch (entry_len) {
            case 1:
                match = leader_sequence_one_key(entry->sequence[0]);
                break;
            case 2:
                match = leader_sequence_two_keys(entry->sequence[0], entry->sequence[1]);
                break;
            case 3:
                match = leader_sequence_three_keys(entry->sequence[0], entry->sequence[1], entry->sequence[2]);
                break;
            case 4:
                match = leader_sequence_four_keys(entry->sequence[0], entry->sequence[1], entry->sequence[2], entry->sequence[3]);
                break;
            case 5:
                match = leader_sequence_five_keys(entry->sequence[0], entry->sequence[1], entry->sequence[2], entry->sequence[3], entry->sequence[4]);
                break;
        }

        if (match) {
            // Execute the output keycode
            sval_keycode_tap(entry->output);
            return;
        }
    }
}

#else

// Stub when leader is disabled
void sval_reload_leader(void) {}

#endif // LEADER_ENABLE
