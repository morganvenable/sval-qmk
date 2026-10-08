// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Update signing keys (see update_keys.h).

#include <stddef.h>
#include "update_keys.h"
#include "optional/monocypher-ed25519.h"

#ifndef SVAL_UPDATE_RELEASE
// TEST ONLY: the public half of keyboards/svalboard/tools/sval_update_TEST_ONLY.key,
// whose seed is in the repository. The host tests check the two match.
static const uint8_t test_key[UPDATE_PUBKEY_BYTES] = {
    0x63, 0x25, 0x77, 0x01, 0x08, 0x2d, 0x7e, 0x63, 0x64, 0x07, 0x2b, 0xee, 0x9f, 0x4a, 0x13, 0x7c,
    0x8c, 0x0c, 0xb5, 0x62, 0x1e, 0x5f, 0x9c, 0x78, 0x00, 0xfd, 0x02, 0xab, 0xb6, 0x40, 0xd1, 0xa4,
};
#elif defined(SVAL_UPDATE_TEST_HOOKS)
#    error "release updater builds cannot have test hooks"
#endif

const uint8_t *update_key(uint8_t key_id) {
#ifndef SVAL_UPDATE_RELEASE
    if (key_id == UPDATE_KEY_TEST) return test_key;
#endif
    (void)key_id;
    return NULL; // release keys 1 and 2: M3
}

update_status_t update_manifest_signature(const uint8_t signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES]) {
    const sval_update_manifest_t *m   = (const sval_update_manifest_t *)signed_manifest;
    const uint8_t                *key = update_key(m->key_id);
    if (!key) return UPDATE_BAD_SIG;
    // crypto_ed25519_check, never crypto_eddsa_check (BLAKE2b, not Ed25519).
    return crypto_ed25519_check(signed_manifest + UPDATE_MANIFEST_BYTES, key, signed_manifest, UPDATE_MANIFEST_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_SIG;
}
