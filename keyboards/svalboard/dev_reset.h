// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Dev builds that reset stored settings once (SVAL_DEV_RESET_TOKEN, rules.mk;
// never in a release).
//
// The build carries a token. On a boot that finds a different token (or none)
// in DEV_RESET_SECTOR, every stored setting is reset to the build's defaults,
// and once the board is up the token is written there. The sector lies outside
// the settings (and below the identity sectors), so the reset never erases the
// record, and builds without a token never look at it: whatever was flashed
// before, a new token resets exactly once, and the next build without one
// keeps the settings. Flashing the same token build again does not reset.

#include <stdbool.h>
#include <stdint.h>

#define DEV_RESET_SECTOR 0xFFD000u  // the 4 KB sector below IDENTITY_SECTOR_A (identity.h)
#define DEV_RESET_MAGIC 0x52445653u // "SVDR"

typedef struct __attribute__((packed)) {
    uint32_t magic; // DEV_RESET_MAGIC
    uint32_t token; // the last token a reset was done for
    uint32_t check; // token ^ ~DEV_RESET_MAGIC
} dev_reset_record_t;

// Whether rec (NULL: none) says the reset for token was already done.
bool dev_reset_done_for(const dev_reset_record_t *rec, uint32_t token);
// The record that says the reset for token is done.
void dev_reset_record_make(dev_reset_record_t *rec, uint32_t token);

#ifdef SVAL_DEV_RESET_TOKEN
void dev_reset_pre_init(void);  // keyboard_pre_init_kb: marks stored settings invalid when due
void dev_reset_post_init(void); // keyboard_post_init_kb: records the token after the reset
#endif
