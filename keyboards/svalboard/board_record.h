// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The board record: a factory identity for each main board (RP2040 MCU board),
// written once at the bench by tools/board_id and never erased by anything but
// a whole-chip erase. Format and flash map: docs/board-record.md.
//
// This file and board_record.c are plain C with no QMK or SDK dependency: the
// firmware, the ID-writer (tools/board_id/writer) and the host tests all build
// them unchanged.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One whole 64 KiB erase block of its own, at the 15 MiB mark. Nothing else
// shares a 4 KiB sector or a 64 KiB block with it (static checks:
// board_record_sval.c, updater/update_flash.c; host check:
// tests/sval_board_id/test_flash_guard.py).
#define BOARD_RECORD_BASE 0xF00000u
#define BOARD_RECORD_SIZE 0x10000u
#define BOARD_RECORD_END (BOARD_RECORD_BASE + BOARD_RECORD_SIZE)
#define BOARD_RECORD_PAGE 256u
#define BOARD_RECORD_PAGES (BOARD_RECORD_SIZE / BOARD_RECORD_PAGE) // 256

// Append-only: one record per 256-byte page, written in page order into erased
// pages and never erased. The newest valid identity record is the board's
// current identity; older ones stay as history.
#define BOARD_RECORD_MAGIC 0x52425653u // "SVBR" as bytes in flash
#define BOARD_RECORD_VERSION 1
#define BOARD_RECORD_HEADER_BYTES 20
#define BOARD_RECORD_CRC_BYTES 4
#define BOARD_RECORD_MAX_BYTES BOARD_RECORD_PAGE
#define BOARD_RECORD_UID_BYTES 8

// Record kinds. A reader skips kinds it does not know.
#define BOARD_RECORD_KIND_IDENTITY 1 // the factory identity

// Header, little-endian, then TLV fields, then a CRC-32 (IEEE, as identity.c)
// over every byte before it.
//   0  u32 magic      BOARD_RECORD_MAGIC
//   4  u8  version    BOARD_RECORD_VERSION
//   5  u8  kind       BOARD_RECORD_KIND_*
//   6  u16 length     whole record, header to CRC inclusive
//   8  u8  uid[8]     the flash die's 64-bit unique ID, as the writer read it
//   16 u32 written    programming time, Unix seconds (UTC), from the station
//   20 TLV...         [tag u8][len u8][len bytes]
//   length-4 u32 crc
//
// TLV tags. Text is UTF-8 with no terminator. Unknown tags are skipped, so new
// fields need no version change; version changes only if the header does.
#define BOARD_TAG_MODEL 0x01    // text: the board model, e.g. "SvalMCUMini"
#define BOARD_TAG_REVISION 0x02 // text: the PCB revision, e.g. "v1.0b0"
#define BOARD_TAG_LOT 0x03      // text: the batch or lot
#define BOARD_TAG_STATION 0x04  // text: station name and tool version
#define BOARD_TAG_STRAP 0x05    // 4 bytes: class, highs under pull-up, pull-down, no pull (of 64)
#define BOARD_TAG_TESTS 0x06    // 8 bytes: u32 tests run, u32 tests passed (bit masks)
#define BOARD_TAG_NOTES 0x07    // text: free-form notes
#define BOARD_TAG_EXPECTED 0x08 // 1 byte: the strap class the batch expected (BOARD_STRAP_*)

// GP3 strap classes (handoff/flipfet-id: low on every Mini; driven high on
// V3.1 and V3.2; floating on V3.0).
#define BOARD_STRAP_LOW 0       // low under pull-up and pull-down
#define BOARD_STRAP_HIGH 1      // high under pull-up and pull-down
#define BOARD_STRAP_FLOATING 2  // follows the pull
#define BOARD_STRAP_UNSTABLE 3  // anything else
#define BOARD_STRAP_NOT_READ 0xFF

uint32_t board_record_crc32(const uint8_t *p, size_t n);

// The record at the start of page, or 0 if it is not a valid record of a
// version this code reads. Returns its length.
uint16_t board_record_check(const uint8_t *page);

typedef struct {
    int16_t newest;      // page of the newest valid identity record, or -1
    uint16_t identities; // valid identity records
    uint16_t records;    // valid records of any kind
    uint16_t used;       // pages up to and including the last one in use
    uint16_t damaged;    // pages in use that hold no valid record (torn writes)
} board_record_scan_t;

// Scans the block (BOARD_RECORD_SIZE bytes, memory-mapped or a copy). A page is
// in use when its first word is not erased: a page is programmed from its start,
// so an interrupted write leaves that word written. Stops at the first unused page.
void board_record_scan(const uint8_t *block, board_record_scan_t *out);

// The page an append should go to, or -1 when the block is full: the first page
// after the last one in use whose 256 bytes are all erased.
int16_t board_record_append_page(const uint8_t *block, const board_record_scan_t *scan);

// Writes an identity record into page (BOARD_RECORD_PAGE bytes; the rest is set
// to 0xFF). tlv holds the station's fields, already encoded. A strap TLV is
// added when strap_class is not BOARD_STRAP_NOT_READ. Returns the length, or 0
// if it does not fit or tlv is malformed.
uint16_t board_record_build(uint8_t *page, const uint8_t uid[BOARD_RECORD_UID_BYTES], uint32_t written, const uint8_t strap[4], const uint8_t *tlv, uint16_t tlv_len);

// True when tlv is a well-formed run of TLV fields exactly tlv_len long.
bool board_record_tlv_ok(const uint8_t *tlv, uint16_t tlv_len);

// The first field with tag in a valid record, or NULL. *len receives its length.
const uint8_t *board_record_field(const uint8_t *rec, uint8_t tag, uint8_t *len);

// The strap class from 64-sample counts of high reads under each pull.
uint8_t board_strap_classify(uint8_t pu_high, uint8_t pd_high, uint8_t np_high, uint8_t samples);
