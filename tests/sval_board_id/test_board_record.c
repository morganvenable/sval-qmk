// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The board record codec (keyboards/svalboard/board_record.c) on the host.
//   test_board_record VECTOR.bin   also writes a record for test_codec.py to cross-check

#include "board_record.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(x)                                                             \
    do {                                                                     \
        checks++;                                                            \
        if (!(x)) {                                                          \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
        }                                                                    \
    } while (0)

static uint8_t block[BOARD_RECORD_SIZE];
static const uint8_t uid[8] = {0xE4, 0x64, 0x50, 0xD2, 0xA7, 0x09, 0x42, 0x32};
static const uint8_t fields[] = {
    BOARD_TAG_MODEL, 11, 'S', 'v', 'a', 'l', 'M', 'C', 'U', 'M', 'i', 'n', 'i',
    BOARD_TAG_REVISION, 6, 'v', '1', '.', '0', 'b', '0',
    BOARD_TAG_LOT, 4, 'L', 'O', 'T', '1',
};

static void program(uint16_t page, const uint8_t *data) {
    for (uint16_t i = 0; i < BOARD_RECORD_PAGE; i++) block[page * BOARD_RECORD_PAGE + i] &= data[i];
}

static void test_build_check(void) {
    uint8_t page[BOARD_RECORD_PAGE];
    uint8_t strap[4] = {BOARD_STRAP_LOW, 0, 0, 0};
    uint16_t len = board_record_build(page, uid, 1761696000u, strap, fields, sizeof(fields));
    CHECK(len == BOARD_RECORD_HEADER_BYTES + 6 + sizeof(fields) + 4);
    CHECK(board_record_check(page) == len);
    CHECK(page[len] == 0xFF && page[BOARD_RECORD_PAGE - 1] == 0xFF);
    uint8_t n = 0;
    const uint8_t *m = board_record_field(page, BOARD_TAG_MODEL, &n);
    CHECK(m && n == 11 && memcmp(m, "SvalMCUMini", 11) == 0);
    const uint8_t *s = board_record_field(page, BOARD_TAG_STRAP, &n);
    CHECK(s && n == 4 && s[0] == BOARD_STRAP_LOW);
    CHECK(!board_record_field(page, BOARD_TAG_NOTES, &n));
    CHECK(memcmp(page + 8, uid, 8) == 0);
    // Any single bit flip in the record is caught.
    for (uint16_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < 8; b++) {
            page[i] ^= 1u << b;
            if (board_record_check(page)) {
                CHECK(!"a bit flip passed the check");
                i = len;
                break;
            }
            page[i] ^= 1u << b;
        }
    }
    CHECK(board_record_check(page) == len);
    // Too long, or malformed fields.
    static uint8_t big[240];
    memset(big, 0, sizeof(big));
    big[0] = BOARD_TAG_NOTES;
    big[1] = 238;
    CHECK(board_record_build(page, uid, 0, strap, big, sizeof(big)) == 0);
    uint8_t bad[] = {BOARD_TAG_MODEL, 5, 'a'};
    CHECK(board_record_build(page, uid, 0, NULL, bad, sizeof(bad)) == 0);
    // The largest that fits.
    big[1] = (uint8_t)(BOARD_RECORD_PAGE - BOARD_RECORD_HEADER_BYTES - 4 - 2);
    CHECK(board_record_build(page, uid, 0, NULL, big, (uint16_t)(big[1] + 2)) == BOARD_RECORD_PAGE);
    CHECK(board_record_check(page) == BOARD_RECORD_PAGE);
    // A newer version or an unknown magic is not read.
    board_record_build(page, uid, 0, NULL, fields, sizeof(fields));
    page[4] = 2;
    CHECK(!board_record_check(page));
}

static void test_scan_append(void) {
    board_record_scan_t s;
    memset(block, 0xFF, sizeof(block));
    board_record_scan(block, &s);
    CHECK(s.newest == -1 && s.identities == 0 && s.used == 0 && s.damaged == 0);
    CHECK(board_record_append_page(block, &s) == 0);

    uint8_t page[BOARD_RECORD_PAGE];
    board_record_build(page, uid, 1, NULL, fields, sizeof(fields));
    program(0, page);
    board_record_scan(block, &s);
    CHECK(s.newest == 0 && s.identities == 1 && s.used == 1);
    CHECK(board_record_append_page(block, &s) == 1);

    // A torn write on page 1: a prefix programmed, CRC missing.
    uint8_t torn[BOARD_RECORD_PAGE];
    memcpy(torn, page, sizeof(torn));
    memset(torn + 12, 0xFF, sizeof(torn) - 12);
    program(1, torn);
    board_record_scan(block, &s);
    CHECK(s.newest == 0 && s.used == 2 && s.damaged == 1);
    CHECK(board_record_append_page(block, &s) == 2);

    // A newer record after it becomes current; the first stays as history.
    board_record_build(page, uid, 2, NULL, fields, sizeof(fields));
    program(2, page);
    board_record_scan(block, &s);
    CHECK(s.newest == 2 && s.identities == 2 && s.records == 2 && s.used == 3);

    // An unknown kind is counted as a record, not as an identity.
    board_record_build(page, uid, 3, NULL, fields, sizeof(fields));
    page[5] = 9;
    uint32_t crc = board_record_crc32(page, (uint32_t)(page[6] | page[7] << 8) - 4);
    memcpy(page + (page[6] | page[7] << 8) - 4, &crc, 4);
    program(3, page);
    board_record_scan(block, &s);
    CHECK(s.newest == 2 && s.identities == 2 && s.records == 3);

    // A page with stray programmed bytes but an erased first word is skipped by append.
    block[4 * BOARD_RECORD_PAGE + 100] = 0x00;
    board_record_scan(block, &s);
    CHECK(s.used == 4 && board_record_append_page(block, &s) == 5);

    // Full.
    memset(block, 0x00, sizeof(block));
    board_record_scan(block, &s);
    CHECK(s.used == BOARD_RECORD_PAGES && board_record_append_page(block, &s) == -1);
}

static void test_strap(void) {
    CHECK(board_strap_classify(0, 0, 0, 64) == BOARD_STRAP_LOW);
    CHECK(board_strap_classify(64, 64, 64, 64) == BOARD_STRAP_HIGH);
    CHECK(board_strap_classify(64, 0, 0, 64) == BOARD_STRAP_FLOATING);
    CHECK(board_strap_classify(64, 0, 64, 64) == BOARD_STRAP_FLOATING);
    CHECK(board_strap_classify(63, 0, 0, 64) == BOARD_STRAP_UNSTABLE);
    CHECK(board_strap_classify(0, 64, 0, 64) == BOARD_STRAP_UNSTABLE);
    CHECK(board_strap_classify(64, 1, 0, 64) == BOARD_STRAP_UNSTABLE);
}

int main(int argc, char **argv) {
    test_build_check();
    test_scan_append();
    test_strap();
    if (argc > 1) {
        uint8_t page[BOARD_RECORD_PAGE];
        uint8_t strap[4] = {BOARD_STRAP_FLOATING, 64, 0, 17};
        board_record_build(page, uid, 1761696000u, strap, fields, sizeof(fields));
        FILE *f = fopen(argv[1], "wb");
        CHECK(f && fwrite(page, 1, sizeof(page), f) == sizeof(page));
        if (f) fclose(f);
    }
    printf("board record: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
