// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The commit: copies the verified image from the staging slot over the running
// firmware, from RAM, and resets (plan M1, "Commit routine"). Called only from
// housekeeping, in COMMITTING, 100 ms after COMMIT was acknowledged.

#include <stdbool.h>
#include <stdint.h>
#include "update_manifest.h"

// Whether this build can commit. While false, COMMIT is refused with
// UPDATE_UNSUPPORTED and nothing else happens.
bool update_commit_available(void);

// Runs the commit for the verified image m describes; crc_body is the
// CRC-32/MPEG-2 over image bytes [0x100, image_len) taken at VERIFYING_IMAGE.
// Returns only when it refused before touching the firmware area, with the
// reason; otherwise the board resets.
update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body);
