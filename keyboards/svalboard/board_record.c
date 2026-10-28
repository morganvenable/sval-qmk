// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The board record format (board_record.h). Pure functions over bytes: no
// flash access, no allocation, no library calls, so the RAM-only ID-writer can
// use it as it is.

#include "board_record.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t board_record_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

bool board_record_tlv_ok(const uint8_t *tlv, uint16_t tlv_len) {
    uint16_t i = 0;
    while (i < tlv_len) {
        if (tlv_len - i < 2) return false;
        uint16_t n = tlv[i + 1];
        if (tlv_len - i - 2 < n) return false;
        i += 2 + n;
    }
    return true;
}

uint16_t board_record_check(const uint8_t *page) {
    if (rd32(page) != BOARD_RECORD_MAGIC || page[4] != BOARD_RECORD_VERSION) return 0;
    uint16_t len = (uint16_t)(page[6] | page[7] << 8);
    if (len < BOARD_RECORD_HEADER_BYTES + BOARD_RECORD_CRC_BYTES || len > BOARD_RECORD_MAX_BYTES) return 0;
    if (rd32(page + len - BOARD_RECORD_CRC_BYTES) != board_record_crc32(page, len - BOARD_RECORD_CRC_BYTES)) return 0;
    if (!board_record_tlv_ok(page + BOARD_RECORD_HEADER_BYTES, len - BOARD_RECORD_HEADER_BYTES - BOARD_RECORD_CRC_BYTES)) return 0;
    return len;
}

static bool page_used(const uint8_t *page) {
    return rd32(page) != 0xFFFFFFFFu;
}

void board_record_scan(const uint8_t *block, board_record_scan_t *out) {
    out->newest     = -1;
    out->identities = 0;
    out->records    = 0;
    out->used       = 0;
    out->damaged    = 0;
    for (uint16_t i = 0; i < BOARD_RECORD_PAGES; i++) {
        const uint8_t *page = block + (uint32_t)i * BOARD_RECORD_PAGE;
        if (!page_used(page)) break;
        out->used = i + 1;
        if (!board_record_check(page)) {
            out->damaged++;
            continue;
        }
        out->records++;
        if (page[5] == BOARD_RECORD_KIND_IDENTITY) {
            out->identities++;
            out->newest = (int16_t)i;
        }
    }
}

int16_t board_record_append_page(const uint8_t *block, const board_record_scan_t *scan) {
    for (uint16_t i = scan->used; i < BOARD_RECORD_PAGES; i++) {
        const uint8_t *page   = block + (uint32_t)i * BOARD_RECORD_PAGE;
        bool           erased = true;
        for (uint16_t j = 0; j < BOARD_RECORD_PAGE && erased; j++) erased = page[j] == 0xFF;
        if (erased) return (int16_t)i;
    }
    return -1;
}

uint16_t board_record_build(uint8_t *page, const uint8_t uid[BOARD_RECORD_UID_BYTES], uint32_t written, const uint8_t strap[4], const uint8_t *tlv, uint16_t tlv_len) {
    for (uint16_t i = 0; i < BOARD_RECORD_PAGE; i++) page[i] = 0xFF;
    if (!board_record_tlv_ok(tlv, tlv_len)) return 0;
    uint16_t strap_len = strap ? 6 : 0;
    uint32_t len       = BOARD_RECORD_HEADER_BYTES + strap_len + tlv_len + BOARD_RECORD_CRC_BYTES;
    if (len > BOARD_RECORD_MAX_BYTES) return 0;

    wr32(page, BOARD_RECORD_MAGIC);
    page[4] = BOARD_RECORD_VERSION;
    page[5] = BOARD_RECORD_KIND_IDENTITY;
    page[6] = (uint8_t)len;
    page[7] = (uint8_t)(len >> 8);
    for (uint8_t i = 0; i < BOARD_RECORD_UID_BYTES; i++) page[8 + i] = uid[i];
    wr32(page + 16, written);
    uint16_t at = BOARD_RECORD_HEADER_BYTES;
    if (strap) {
        page[at++] = BOARD_TAG_STRAP;
        page[at++] = 4;
        for (uint8_t i = 0; i < 4; i++) page[at++] = strap[i];
    }
    for (uint16_t i = 0; i < tlv_len; i++) page[at++] = tlv[i];
    wr32(page + at, board_record_crc32(page, at));
    return (uint16_t)len;
}

const uint8_t *board_record_field(const uint8_t *rec, uint8_t tag, uint8_t *len) {
    uint16_t total = board_record_check(rec);
    if (!total) return NULL;
    uint16_t end = total - BOARD_RECORD_CRC_BYTES;
    for (uint16_t i = BOARD_RECORD_HEADER_BYTES; i + 2 <= end; i += 2 + rec[i + 1]) {
        if (rec[i] == tag) {
            *len = rec[i + 1];
            return rec + i + 2;
        }
    }
    return NULL;
}

uint8_t board_strap_classify(uint8_t pu_high, uint8_t pd_high, uint8_t np_high, uint8_t samples) {
    (void)np_high; // reported, not needed to classify
    if (pu_high == 0 && pd_high == 0) return BOARD_STRAP_LOW;
    if (pu_high == samples && pd_high == samples) return BOARD_STRAP_HIGH;
    if (pu_high == samples && pd_high == 0) return BOARD_STRAP_FLOATING;
    return BOARD_STRAP_UNSTABLE;
}
