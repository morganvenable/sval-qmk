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
//           [identity records] [flags] [16 bytes of the record from chunk * 16]
// status 0: a record; 1: none; 2: the die is not 16 MiB, so there is no region;
// 3: chunk past the end. flags bit 0: the die's unique ID was compared with the
// record's; bit 1: they match (a record copied from another board's flash would
// not). Older firmware answers 0xFF in byte 1 (unknown command).
#define BOARD_RECORD_SVAL_HEAD 7
#define BOARD_RECORD_SVAL_CHUNK 16
#define BOARD_RECORD_UID_COMPARED 0x01
#define BOARD_RECORD_UID_MATCHES 0x02

void sval_board_record_kb(uint8_t *io, uint8_t length) {
    if (length < BOARD_RECORD_SVAL_HEAD + BOARD_RECORD_SVAL_CHUNK) return;
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
    identity_serial_source_t src;
    const uint8_t           *die = identity_serial(&src);
    if (len && die && src == IDENTITY_SERIAL_FLASH_ID) {
        io[6] = BOARD_RECORD_UID_COMPARED | (memcmp(die, rec + 8, BOARD_RECORD_UID_BYTES) == 0 ? BOARD_RECORD_UID_MATCHES : 0);
    }
    uint32_t from = (uint32_t)chunk * BOARD_RECORD_SVAL_CHUNK;
    if (!len || from >= len) {
        io[0] = len ? 3 : 1;
        return;
    }
    uint32_t n = len - from < BOARD_RECORD_SVAL_CHUNK ? len - from : BOARD_RECORD_SVAL_CHUNK;
    memcpy(io + BOARD_RECORD_SVAL_HEAD, rec + from, n);
}
