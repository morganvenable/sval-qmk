// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Update signing keys and the build-info record (see update_keys.h).

#include <stddef.h>
#include "update_keys.h"
#include "update_release_keys.h"
#include "updater.h" // SVAL_FW_VERSION
#include "optional/monocypher-ed25519.h"

#if defined(SVAL_UPDATE_TEST_KEY) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot accept the TEST-ONLY key"
#elif defined(SVAL_UPDATE_TEST_HOOKS) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have test hooks"
#elif defined(SVAL_KEYTEST) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have SVAL_KEYTEST"
#endif
#if defined(SVAL_UPDATE_RELEASE) && defined(SVAL_HOST_BOOTLOADER)
#    if SVAL_HOST_BOOTLOADER
#        error "release updater builds cannot have SVAL_HOST_BOOTLOADER"
#    endif
#endif

#ifdef SVAL_UPDATE_TEST_KEY
// TEST ONLY: the public half of keyboards/svalboard/tools/sval_update_TEST_ONLY.key,
// whose seed is in the repository. Only builds made with SVAL_UPDATE_TEST_KEY=yes
// (or SVAL_UPDATE_TEST_HOOKS=yes) carry it. The host tests check the two match.
static const uint8_t test_key[UPDATE_PUBKEY_BYTES] = {
    0x63, 0x25, 0x77, 0x01, 0x08, 0x2d, 0x7e, 0x63, 0x64, 0x07, 0x2b, 0xee, 0x9f, 0x4a, 0x13, 0x7c,
    0x8c, 0x0c, 0xb5, 0x62, 0x1e, 0x5f, 0x9c, 0x78, 0x00, 0xfd, 0x02, 0xab, 0xb6, 0x40, 0xd1, 0xa4,
};
#endif

// key_id 1 and 2 (update_release_keys.h): in every updater build, release or
// not, so a test board can take a signed release image too (docs/updater.md,
// "Release signing").
static const uint8_t release_keys[2][UPDATE_PUBKEY_BYTES] = {SVAL_UPDATE_RELEASE_KEY_1, SVAL_UPDATE_RELEASE_KEY_2};

// ---- build info --------------------------------------------------------------------

#ifndef SVAL_FW_VERSION_STRING
#    define SVAL_FW_VERSION_STRING ""
#endif
_Static_assert(sizeof(SVAL_FW_VERSION_STRING) <= UPDATE_VERSION_CHARS + 1, "SVAL_FW_VERSION_STRING is longer than 16 characters");
_Static_assert(sizeof(update_build_info_t) == 28, "update_build_info_t is 28 bytes");

#if defined(SVAL_HOST_BOOTLOADER)
#    if SVAL_HOST_BOOTLOADER
#        define BI_HOST_BOOTLOADER UPDATE_BUILD_HOST_BOOTLOADER
#    endif
#endif
#ifndef BI_HOST_BOOTLOADER
#    define BI_HOST_BOOTLOADER 0
#endif

const update_build_info_t sval_update_build_info = {
    .magic = {'S', 'V', 'B', 'I'},
    .info_ver = UPDATE_BUILD_INFO_VERSION,
    .flags = 0
#ifdef SVAL_UPDATE_RELEASE
             | UPDATE_BUILD_RELEASE
#endif
#ifdef SVAL_UPDATE_TEST_KEY
             | UPDATE_BUILD_TEST_KEY
#endif
#ifdef SVAL_UPDATE_TEST_HOOKS
             | UPDATE_BUILD_TEST_HOOKS
#endif
#ifdef SVAL_KEYTEST
             | UPDATE_BUILD_KEYTEST
#endif
             | BI_HOST_BOOTLOADER
#if SVAL_UPDATE_RELEASE_KEYS_DRY_RUN
             | UPDATE_BUILD_KEYS_DRY_RUN
#endif
    ,
    .release_keys = 2,
    .updater_proto = UPDATE_PROTOCOL_VERSION,
    .fw_version = SVAL_FW_VERSION,
    .version = SVAL_FW_VERSION_STRING,
};

// ---- keys ----------------------------------------------------------------------------

const uint8_t *update_key(uint8_t key_id) {
#ifdef SVAL_UPDATE_TEST_KEY
    if (key_id == UPDATE_KEY_TEST) return test_key;
#endif
    if (key_id == UPDATE_KEY_RELEASE_1) return release_keys[0];
    if (key_id == UPDATE_KEY_RELEASE_2) return release_keys[1];
    return NULL;
}

update_status_t update_manifest_signature(const uint8_t signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES]) {
    const sval_update_manifest_t *m   = (const sval_update_manifest_t *)signed_manifest;
    const uint8_t                *key = update_key(m->key_id);
    if (!key) return UPDATE_BAD_SIG;
    // crypto_ed25519_check, never crypto_eddsa_check (BLAKE2b, not Ed25519).
    return crypto_ed25519_check(signed_manifest + UPDATE_MANIFEST_BYTES, key, signed_manifest, UPDATE_MANIFEST_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_SIG;
}
