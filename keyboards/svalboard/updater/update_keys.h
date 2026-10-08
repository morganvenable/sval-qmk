// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Update signing keys and the manifest signature check (D6, D7).

#include <stdint.h>
#include "update_manifest.h"

// The public key for key_id, or NULL when this build accepts no such key. The
// TEST-ONLY key (key_id 0) is present only when SVAL_UPDATE_TEST_KEY is defined
// (make SVAL_UPDATE_TEST_KEY=yes, implied by SVAL_UPDATE_TEST_HOOKS=yes), never
// in release builds; release keys arrive in M3.
const uint8_t *update_key(uint8_t key_id);

// Checks the Ed25519 signature that follows the manifest in a signed manifest
// (UPDATE_SIGNED_MANIFEST_BYTES) against the key its key_id names. UPDATE_OK
// or UPDATE_BAD_SIG. Uses about 1.9 KiB of stack.
update_status_t update_manifest_signature(const uint8_t signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES]);
