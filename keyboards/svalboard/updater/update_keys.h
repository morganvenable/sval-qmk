// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Update signing keys and the manifest signature check (D6, D7, D30), and the
// build-info record the release tools read back out of an image.

#include <stdint.h>
#include "update_manifest.h"

// The public key for key_id, or NULL when this build accepts no such key.
// key_id 1 and 2 are the release keys (update_release_keys.h), in every
// updater build. The TEST-ONLY key (key_id 0) is present only when
// SVAL_UPDATE_TEST_KEY is defined (make SVAL_UPDATE_TEST_KEY=yes, implied by
// SVAL_UPDATE_TEST_HOOKS=yes), never in release builds.
const uint8_t *update_key(uint8_t key_id);

// Checks the Ed25519 signature that follows the manifest in a signed manifest
// (UPDATE_SIGNED_MANIFEST_BYTES) against the key its key_id names. UPDATE_OK
// or UPDATE_BAD_SIG. Uses about 1.9 KiB of stack.
update_status_t update_manifest_signature(const uint8_t signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES]);

// ---- build info (M3) ------------------------------------------------------------------
// One record in .rodata of every updater build, so the image itself says what
// kind of build it is. make_update.py takes fw_version and the version string
// for the manifest from it (they cannot disagree with what the firmware
// reports), its signer refuses an image whose record is not a release build
// or whose side, pointing device or keymap is not the manifest's, and
// tools/check_release_elf.py lints it. INFO page 1 returns the version.
// Version 2 added hand, pointing_id and keymap_id.

#define UPDATE_BUILD_INFO_VERSION 2

#define UPDATE_BUILD_RELEASE 0x01         // SVAL_UPDATE_RELEASE
#define UPDATE_BUILD_TEST_KEY 0x02        // SVAL_UPDATE_TEST_KEY: the TEST-ONLY key is accepted
#define UPDATE_BUILD_TEST_HOOKS 0x04      // SVAL_UPDATE_TEST_HOOKS
#define UPDATE_BUILD_KEYTEST 0x08         // SVAL_KEYTEST (never with the updater; listed for the lint)
#define UPDATE_BUILD_HOST_BOOTLOADER 0x10 // SVAL_HOST_BOOTLOADER (Scan Lab reboot into BOOTSEL)
#define UPDATE_BUILD_KEYS_DRY_RUN 0x20    // the release keys are the M3 DRY RUN keys (update_release_keys.h)

typedef struct __attribute__((packed)) {
    char     magic[4];       // "SVBI"
    uint8_t  info_ver;       // UPDATE_BUILD_INFO_VERSION
    uint8_t  flags;          // UPDATE_BUILD_*
    uint8_t  release_keys;   // release key slots compiled in: 2
    uint8_t  updater_proto;  // UPDATE_PROTOCOL_VERSION
    uint32_t fw_version;     // SVAL_FW_VERSION (D17, D32)
    char     version[UPDATE_VERSION_CHARS]; // SVAL_FW_VERSION_STRING, NUL-padded (the release tag)
    uint8_t  hand;           // UPDATE_HAND_* of this build (update_self.h); 0xFF in host tests
    uint8_t  pointing_id;    // UPDATE_POINTING_* of this build; 0xFF in host tests
    uint8_t  keymap_id;      // SVAL_UPDATE_KEYMAP_ID (rules.mk): 1 sval, 2 blank, 0 any other keymap
    uint8_t  reserved;       // 0
} update_build_info_t;       // 32 B

extern const update_build_info_t sval_update_build_info;
