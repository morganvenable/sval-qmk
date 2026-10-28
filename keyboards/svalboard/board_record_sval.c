// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The board record in the firmware: read-only. The firmware never writes,
// erases or caches anything about the record except which page holds it, and
// it reads the region only when a host asks (Sval BOARD_RECORD, 0x3A), so a
// board without a record runs exactly as before. Format: board_record.h.

#include "board_record.h"
#include "identity.h"    // identity_available(): the die is 16 MB
#include "build_reset.h" // BUILD_RESET_SECTOR
#include "side.h"        // SVAL_SIDE_SECTOR_A, _B
#include "config.h"
#include <string.h>

#include "hardware/regs/addressmap.h"

// ---- the flash map (docs/board-record.md) --------------------------------------
// The record block shares no 64 KiB block with anything else that writes flash.
#define BR_BLOCK(x) ((x) & ~0xFFFFu)
#define SETTINGS_END_ (WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES)
_Static_assert(BOARD_RECORD_BASE % 0x10000u == 0 && BOARD_RECORD_SIZE == 0x10000u, "the board record is one aligned 64 KiB block");
_Static_assert(BOARD_RECORD_END <= FLASH_LEN, "the board record is on the 16 MiB die");
_Static_assert(0x160000u <= BOARD_RECORD_BASE, "the firmware area (flash_reservation.ld) is below the board record");
_Static_assert(SETTINGS_END_ <= BOARD_RECORD_BASE, "the settings are below the board record");
_Static_assert(BR_BLOCK(BUILD_RESET_SECTOR) >= BOARD_RECORD_END, "the build reset sector is above the board record block");
_Static_assert(BR_BLOCK(IDENTITY_SECTOR_A) >= BOARD_RECORD_END, "identity is above the board record block");
_Static_assert(BR_BLOCK(SVAL_SIDE_SECTOR_A) >= BOARD_RECORD_END, "the side records are above the board record block");
// The staging slot, the commit's range and the pair record: updater/update_flash.c.

// ---- reading -------------------------------------------------------------------

static const uint8_t *block(void) {
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + BOARD_RECORD_BASE);
}

// -2 not looked yet, -1 none, else the page of the newest identity record.
static int16_t  newest = -2;
static uint16_t identities;

static int16_t find(void) {
    if (newest == -2) {
        board_record_scan_t s;
        board_record_scan(block(), &s);
        newest     = s.newest;
        identities = s.identities;
    }
    return newest;
}

// Request:  [0xDF] [0x3A] [chunk]
// Response: [0xDF] [0x3A] [status] [format=1] [len lo] [len hi] [chunk]
//           [identity records] [16 bytes of the record from chunk * 16]
// status 0: a record; 1: none; 2: the die is not 16 MiB, so there is no region;
// 3: chunk past the end. Older firmware answers 0xFF in byte 1 (unknown command).
#define BOARD_RECORD_SVAL_CHUNK 16

void sval_board_record_kb(uint8_t *io, uint8_t length) {
    if (length < 6 + BOARD_RECORD_SVAL_CHUNK) return;
    uint8_t chunk = io[0];
    memset(io, 0, length);
    io[1] = 1;
    io[4] = chunk;
    if (!identity_available()) {
        io[0] = 2;
        return;
    }
    int16_t page = find();
    if (page < 0) {
        io[0] = 1;
        return;
    }
    const uint8_t *rec = block() + (uint32_t)page * BOARD_RECORD_PAGE;
    uint16_t       len = board_record_check(rec);
    io[2]              = (uint8_t)len;
    io[3]              = (uint8_t)(len >> 8);
    io[5]              = (uint8_t)identities;
    uint32_t from      = (uint32_t)chunk * BOARD_RECORD_SVAL_CHUNK;
    if (!len || from >= len) {
        io[0] = len ? 3 : 1;
        return;
    }
    uint32_t n = len - from < BOARD_RECORD_SVAL_CHUNK ? len - from : BOARD_RECORD_SVAL_CHUNK;
    memcpy(io + 6, rec + from, n);
}
