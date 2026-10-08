// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The commit stub (keyboards/svalboard/updater/update_commit.c) must refuse:
// this build cannot commit, and calling it anyway does nothing.

#include <stdio.h>
#include <string.h>
#include "update_commit.h"

int main(void) {
    sval_update_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.image_len = 0x18000;
    int ok      = !update_commit_available() && update_commit_run(&m, 0x12345678u) == UPDATE_UNSUPPORTED;
    printf("commit stub: %s\n", ok ? "refuses" : "FAIL: does not refuse");
    return ok ? 0 : 1;
}
