// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The ID-writer: a RAM-only RP2040 program the station loads into a blank
// board in BOOTSEL (picotool load -x). It reads the flash unique ID and the
// GP3 strap, appends the board record (board_record.h) and leaves a report in
// RAM for the station to read back. It never erases flash and writes nothing
// outside the record block. Layout of both blocks: board_record.py.

#include <stdbool.h>
#include <stdint.h>
#include "board_record.h"

#define WRITER_VERSION 1

// Parameters, patched into the image by the station before loading.
#define WRITER_PARAMS_ADDR 0x20000100u
#define WRITER_PARAMS_MAGIC 0x50425653u // "SVBP"
#define WRITER_PARAMS_BYTES 256
#define WRITER_TLV_MAX 232
#define WRITER_FLAG_FORCE 0x01 // append an identity record although one exists
#define WRITER_FLAG_DRY 0x02   // read and report only; write nothing
#define WRITER_EXPECT_ANY 0xFF // do not check the strap

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version; // 1
    uint8_t  flags;   // WRITER_FLAG_*
    uint8_t  expect;  // BOARD_STRAP_* or WRITER_EXPECT_ANY
    uint8_t  reserved0;
    uint32_t nonce;   // echoed in the report
    uint32_t written; // Unix seconds
    uint16_t tlv_len;
    uint16_t reserved1;
    uint8_t  tlv[WRITER_TLV_MAX];
    uint32_t crc; // CRC-32 over the 252 bytes before it
} writer_params_t;
_Static_assert(sizeof(writer_params_t) == WRITER_PARAMS_BYTES, "params are 256 bytes");

// The report, in RAM outside the image.
#define WRITER_REPORT_ADDR 0x20010000u
#define WRITER_REPORT_MAGIC 0x4D425653u // "SVBM"
#define WRITER_REPORT_BYTES 40

enum writer_status {
    WRITER_WRITTEN        = 0,  // a record was appended and read back
    WRITER_HAS_RECORD     = 1,  // an identity record exists and FORCE was not given
    WRITER_STRAP_MISMATCH = 2,  // the strap is not the class the batch expects
    WRITER_STRAP_UNSTABLE = 3,  // the strap gave no clear class
    WRITER_NOT_16MB       = 4,  // the flash is not 16 MiB: the block does not exist
    WRITER_NO_UID         = 5,  // the flash returned no unique ID
    WRITER_FULL           = 6,  // no erased page left in the block
    WRITER_VERIFY_FAILED  = 7,  // the page did not read back as written
    WRITER_BAD_PARAMS     = 8,  // the station's parameters are missing or damaged
    WRITER_DRY_OK         = 9,  // dry run: everything read, nothing written
    WRITER_TOO_LONG       = 10, // the fields do not fit one page
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version; // 1
    uint8_t  status;  // enum writer_status
    uint8_t  strap;   // BOARD_STRAP_*
    uint8_t  jedec_capacity;
    uint32_t nonce;
    uint8_t  uid[BOARD_RECORD_UID_BYTES];
    uint8_t  pu_high, pd_high, np_high, samples;
    int16_t  page; // the page written, or the newest identity record's, or -1
    uint16_t record_len;
    uint16_t identities; // identity records before this run
    uint16_t used;       // pages in use before this run
    uint16_t damaged;
    uint16_t writer_version;
    uint32_t crc; // CRC-32 over the 36 bytes before it
} writer_report_t;
_Static_assert(sizeof(writer_report_t) == WRITER_REPORT_BYTES, "the report is 40 bytes");

#define WRITER_STRAP_SAMPLES 64

// Hardware access (hal_rp2040.c on the board, hal_host.c in the tests).
uint8_t        hal_jedec_capacity(void);
void           hal_unique_id(uint8_t id[BOARD_RECORD_UID_BYTES]);
void           hal_strap_sample(uint8_t *pu_high, uint8_t *pd_high, uint8_t *np_high, uint8_t samples);
const uint8_t *hal_block(void); // the record block, readable
// Programs one 256-byte page of the block (page < BOARD_RECORD_PAGES). Never erases.
void hal_program_page(uint16_t page, const uint8_t *data);

void writer_run(const writer_params_t *p, writer_report_t *r);
