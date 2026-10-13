// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Stored settings reset on the first boot of a different firmware build (D39).
//
// At boot the firmware hashes its own image in flash and compares the hash with
// the record in BUILD_RESET_SECTOR. When they differ, or there is no valid
// record (Vial, vLaunch2 and older Svalboard QMK never write one), every stored
// setting is reset to this build's defaults, through the same path as a new
// EECONFIG_MAGIC_NUMBER; once the board is up the hash is recorded, with a
// notice for the host (GET_INFO byte 18 bit 2) until it sends 0x2A. Flashing
// the identical image again finds its own hash and keeps the settings.
//
// The sector lies outside the settings and below the identity sectors, so a
// settings reset never erases the record. On a die smaller than 16 MB the
// sector does not exist: there is nowhere to keep the record, so the build
// keeps settings (an incompatible store is still reset by the EECONFIG magic
// and the Sval storage stamp). When the record cannot be written, settings
// reset again at the next boot, never mid-session, and GET_INFO byte 18 bit 1
// tells the host that a change could not be saved.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUILD_RESET_SECTOR 0xFFD000u  // the 4 KB sector below IDENTITY_SECTOR_A (identity.h)
#define BUILD_RESET_MAGIC 0x52425653u // "SVBR"
#define BUILD_RESET_NOTICE 0x01u      // the host has not acknowledged the reset yet

typedef struct __attribute__((packed)) {
    uint32_t magic; // BUILD_RESET_MAGIC
    uint32_t image; // hash of the image the settings were last reset for
    uint32_t flags; // BUILD_RESET_NOTICE
    uint32_t check; // ~(magic ^ image ^ flags)
} build_reset_record_t;

// FNV-1a over the image, 32-bit words; a trailing partial word is padded with 0xFF.
uint32_t build_reset_hash(const uint8_t *image, size_t len);
// Whether rec (NULL: none) holds a valid record for image.
bool build_reset_matches(const build_reset_record_t *rec, uint32_t image);
// Whether a boot of image must reset settings. available: the sector exists.
bool build_reset_due(bool available, const build_reset_record_t *rec, uint32_t image);
// Whether rec says the host has yet to acknowledge the reset for image.
bool build_reset_notice(const build_reset_record_t *rec, uint32_t image);
void build_reset_record_make(build_reset_record_t *rec, uint32_t image, uint32_t flags);

#ifndef BUILD_RESET_HOST_TEST
void build_reset_pre_init(void);  // keyboard_pre_init_kb: marks stored settings invalid when due
void build_reset_post_init(void); // keyboard_post_init_kb: records the image after the reset
bool build_reset_notice_pending(void);
bool build_reset_record_failed(void);
void build_reset_notice_clear(void); // Sval 0x2A
#endif
