// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// STUB. The RAM commit routine arrives with the next stage of M1. Until then
// this build cannot commit: COMMIT is refused, and if anything called
// update_commit_run() regardless it returns at once without touching flash or
// jumping anywhere.

#include "update_commit.h"

bool update_commit_available(void) {
    return false;
}

update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body) {
    (void)m;
    (void)crc_body;
    return UPDATE_UNSUPPORTED;
}
