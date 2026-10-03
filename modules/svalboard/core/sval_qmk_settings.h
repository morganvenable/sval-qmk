// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialize QMK settings (load from EEPROM and apply)
void sval_qmk_settings_init(void);

// Query supported QSIDs greater than qsid_gt
void sval_qmk_settings_query(uint16_t qsid_gt, uint8_t *buffer, uint8_t length);

// Get a setting value (returns 0 on success)
int sval_qmk_settings_get(uint16_t qsid, uint8_t *buffer, uint8_t length);

// Set a setting value (returns 0 on success)
int sval_qmk_settings_set(uint16_t qsid, const uint8_t *data, uint8_t length);

// Reset all settings to defaults
void sval_qmk_settings_reset(void);

// Getters for global settings (used by other sval modules as fallback)
uint16_t sval_get_tapping_term(void);
uint16_t sval_get_combo_term(void);
uint16_t sval_get_leader_timeout(void);
bool sval_get_leader_per_key_timing(void);
