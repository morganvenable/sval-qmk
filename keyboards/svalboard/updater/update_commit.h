// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The commit: copies the verified image from the staging slot over the running
// firmware, from RAM, and resets (plan M1, "Commit routine"; update_commit.c).
// Called only from housekeeping, in COMMITTING, 100 ms after COMMIT was
// acknowledged.

#include <stdbool.h>
#include <stdint.h>
#include "update_manifest.h"

// Watchdog period while the commit runs (D9). The watchdog counter drops 2 per
// microsecond (RP2040 erratum E1), so it is loaded with twice this, and the
// 24-bit LOAD register caps it at 8.39 s. It must be at least twice the worst
// single flash operation the commit makes: a 64 KiB block erase, 2 s at the
// W25Q128JV datasheet maximum.
#define UPDATE_COMMIT_WATCHDOG_US 8000000u

// Whether this build can commit (the die is 16 MiB). While false, COMMIT is
// refused with UPDATE_UNSUPPORTED and nothing else happens.
bool update_commit_available(void);

// Runs the commit for the verified image m describes; crc_body is the
// CRC-32/MPEG-2 over image bytes [0x100, image_len) taken at VERIFYING_IMAGE.
// Returns only when it refused before touching the firmware area, with the
// reason (step 0's checks, or BUSY when a DMA channel is busy at step 1);
// otherwise the board resets, into the new image or, if the copy failed, into
// BOOTSEL.
update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body);

#ifdef SVAL_UPDATE_TEST_HOOKS
// Hardware-test halt points (M1 #8, #9, #10). At the chosen point the commit
// stops for good: it spins with interrupts off, feeding the watchdog (fed: the
// board hangs until it is unplugged) or not (it resets within the watchdog
// period), or, for UPDATE_HALT_FAULT, executes an undefined instruction so the
// HardFault goes through the RAM vector table.
typedef enum {
    UPDATE_HALT_NONE        = 0,
    UPDATE_HALT_INVALIDATED = 1, // N = -1: page 0 zeroed and read back, nothing erased
    UPDATE_HALT_FIRST_ERASE = 2, // N = 0: sector 0 erased
    UPDATE_HALT_MID_PROGRAM = 3, // after programming the middle sector of the image
    UPDATE_HALT_LAST_SECTOR = 4, // after programming the last sector, page 0 still erased
    UPDATE_HALT_PAGE0       = 5, // page 0 written and read back, before the reset
    UPDATE_HALT_FAULT       = 6, // a HardFault straight after the invalidation (M1 #10)
    UPDATE_HALT_COUNT,
} update_halt_t;

// Chooses the halt point for the next commit; UPDATE_HALT_NONE clears it.
// False (and no change) for an unknown point.
bool update_commit_test_halt(uint8_t point, bool fed);
#endif
