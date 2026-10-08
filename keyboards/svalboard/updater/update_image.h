// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Structure checks on a manifest and on a staged image's first page and vector
// table (R7). Pure functions over bytes: the state machine runs them at
// VERIFYING_MANIFEST, VERIFYING_IMAGE and again at commit step 0, and the host
// tests run them unchanged. kb/tools/make_update.py implements the same rules.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "update_manifest.h"

#define UPDATE_BOOT2_BYTES 256
#define UPDATE_BOOT2_CRC_OFFSET 252 // the ROM checks bytes 0-251 against the LE word here
#define UPDATE_HEAD_BYTES 0x108     // boot2 page, then initial SP and reset vector
#define UPDATE_SP_MIN 0x20000000u
#define UPDATE_SP_MAX 0x20042000u   // top of SRAM5; inclusive (a full descending stack starts here)
#define UPDATE_XIP_BASE 0x10000000u
#define UPDATE_VECTORS_OFFSET 0x100u
#define UPDATE_FLASH_DIE_MAX 0x1000000u // 16 MiB: no image is longer

// What the running build is, for the checks that compare an image with it.
typedef struct {
    uint8_t  hand;           // UPDATE_HAND_*
    uint8_t  pointing_id;    // UPDATE_POINTING_*
    uint16_t security_epoch; // SVAL_UPDATE_SECURITY_EPOCH
    uint8_t  storage_format; // SVAL_UPDATE_STORAGE_FORMAT
    bool     release_build;  // SVAL_UPDATE_RELEASE: refuse the test key and DIAGNOSTIC images
    uint32_t max_image;      // SVAL_UPDATE_MAX_IMAGE
} update_device_t;

// This firmware's own description (firmware builds only).
void update_device_self(update_device_t *dev);

// CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no xorout.
// The _update form continues a CRC over more bytes, starting from
// UPDATE_CRC32_INIT.
#define UPDATE_CRC32_INIT 0xFFFFFFFFu
uint32_t update_crc32_mpeg2(const uint8_t *p, size_t n);
uint32_t update_crc32_mpeg2_update(uint32_t crc, const uint8_t *p, size_t n);

// Whether page 0 carries a boot2 the ROM would run.
bool update_boot2_valid(const uint8_t page0[UPDATE_BOOT2_BYTES]);

// Manifest fields against this build, signature aside. UPDATE_OK or the
// refusal's status; the order of checks is fixed, so a manifest with several
// faults always reports the same one.
update_status_t update_manifest_check(const sval_update_manifest_t *m, const update_device_t *dev);

// The image's first UPDATE_HEAD_BYTES against its manifest length: boot2 CRC,
// initial SP, reset vector. UPDATE_OK or UPDATE_BAD_IMAGE.
update_status_t update_image_head_check(const uint8_t head[UPDATE_HEAD_BYTES], uint32_t image_len);
