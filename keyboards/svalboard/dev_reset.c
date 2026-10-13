// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Dev builds that reset stored settings once (see dev_reset.h).

#include <stddef.h>
#include "dev_reset.h"

bool dev_reset_done_for(const dev_reset_record_t *rec, uint32_t token) {
    return rec && rec->magic == DEV_RESET_MAGIC && rec->token == token && rec->check == (token ^ ~DEV_RESET_MAGIC);
}

void dev_reset_record_make(dev_reset_record_t *rec, uint32_t token) {
    rec->magic = DEV_RESET_MAGIC;
    rec->token = token;
    rec->check = token ^ ~DEV_RESET_MAGIC;
}

#ifndef DEV_RESET_HOST_TEST
#    if !defined(SVAL_DEV_RESET_TOKEN)
#        error "dev_reset.c is built only with SVAL_DEV_RESET_TOKEN (rules.mk)"
#    endif
#    ifdef SVAL_UPDATE_RELEASE
#        error "release builds cannot have SVAL_DEV_RESET_TOKEN (dev builds only)"
#    endif
#    include "eeconfig.h"
#    include "identity.h"
#    include "hardware/regs/addressmap.h"

_Static_assert(SVAL_DEV_RESET_TOKEN != 0, "SVAL_DEV_RESET_TOKEN is not 0");
_Static_assert(DEV_RESET_SECTOR + 4096u == IDENTITY_SECTOR_A, "the dev reset sector is just below the identity sectors");
#    ifdef SVAL_UPDATE_BASE
_Static_assert(SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE <= DEV_RESET_SECTOR, "the staging slot ends below the dev reset sector");
#    endif

// Kept in the ELF so check_release_elf.py finds it.
__attribute__((used)) const uint32_t sval_dev_reset_token = SVAL_DEV_RESET_TOKEN;

static bool pending;

void dev_reset_pre_init(void) {
    // On a smaller die the sector does not exist (its address would wrap onto
    // the firmware), so a dev build there never resets.
    if (!identity_available()) return;
    const dev_reset_record_t *rec = (const dev_reset_record_t *)(XIP_NOCACHE_NOALLOC_BASE + DEV_RESET_SECTOR);
    pending = !dev_reset_done_for(rec, sval_dev_reset_token);
    // quantum_init() then finds the settings invalid and resets them all, as
    // for a new EECONFIG_MAGIC_NUMBER.
    if (pending) eeconfig_disable();
}

void dev_reset_post_init(void) {
    if (!pending) return;
    dev_reset_record_t rec;
    dev_reset_record_make(&rec, sval_dev_reset_token);
    // A failed write only means the next boot resets again.
    if (identity_dev_reset_write(&rec, sizeof(rec))) pending = false;
}
#endif
