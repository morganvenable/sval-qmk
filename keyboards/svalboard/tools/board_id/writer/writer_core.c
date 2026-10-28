// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The ID-writer's logic, the same on the board and in the host tests.

#include "writer.h"

#define JEDEC_CAPACITY_16MB 0x18 // 2^24 bytes

static uint8_t page_buf[BOARD_RECORD_PAGE] __attribute__((aligned(4)));

static void finish(writer_report_t *r, uint8_t status) {
    r->status = status;
    r->crc    = board_record_crc32((const uint8_t *)r, WRITER_REPORT_BYTES - 4);
}

static bool page_erased(const uint8_t *page) {
    for (uint16_t i = 0; i < BOARD_RECORD_PAGE; i++)
        if (page[i] != 0xFF) return false;
    return true;
}

void writer_run(const writer_params_t *p, writer_report_t *r) {
    uint8_t *raw = (uint8_t *)r;
    for (uint16_t i = 0; i < WRITER_REPORT_BYTES; i++) raw[i] = 0;
    r->magic          = WRITER_REPORT_MAGIC;
    r->version        = 1;
    r->writer_version = WRITER_VERSION;
    r->strap          = BOARD_STRAP_NOT_READ;
    r->page           = -1;

    if (p->magic != WRITER_PARAMS_MAGIC || p->version != 1 || p->crc != board_record_crc32((const uint8_t *)p, WRITER_PARAMS_BYTES - 4) || p->tlv_len > WRITER_TLV_MAX || !board_record_tlv_ok(p->tlv, p->tlv_len)) {
        finish(r, WRITER_BAD_PARAMS);
        return;
    }
    r->nonce = p->nonce;

    // The flash: 16 MiB, or the block's addresses would wrap onto the firmware.
    r->jedec_capacity = hal_jedec_capacity();
    hal_unique_id(r->uid);
    if (r->jedec_capacity != JEDEC_CAPACITY_16MB) {
        finish(r, WRITER_NOT_16MB);
        return;
    }
    bool all00 = true, allFF = true;
    for (uint8_t i = 0; i < BOARD_RECORD_UID_BYTES; i++) {
        all00 &= r->uid[i] == 0x00;
        allFF &= r->uid[i] == 0xFF;
    }
    if (all00 || allFF) {
        finish(r, WRITER_NO_UID);
        return;
    }

    // The strap.
    r->samples = WRITER_STRAP_SAMPLES;
    hal_strap_sample(&r->pu_high, &r->pd_high, &r->np_high, r->samples);
    r->strap = board_strap_classify(r->pu_high, r->pd_high, r->np_high, r->samples);

    // What the block holds already.
    const uint8_t      *block = hal_block();
    board_record_scan_t s;
    board_record_scan(block, &s);
    r->identities = s.identities;
    r->used       = s.used;
    r->damaged    = s.damaged;
    r->page       = s.newest;
    if (s.newest >= 0) r->record_len = board_record_check(block + (uint32_t)s.newest * BOARD_RECORD_PAGE);

    if (p->expect != WRITER_EXPECT_ANY) {
        if (r->strap == BOARD_STRAP_UNSTABLE) {
            finish(r, WRITER_STRAP_UNSTABLE);
            return;
        }
        if (r->strap != p->expect) {
            finish(r, WRITER_STRAP_MISMATCH);
            return;
        }
    }
    if (s.identities && !(p->flags & WRITER_FLAG_FORCE)) {
        finish(r, WRITER_HAS_RECORD);
        return;
    }

    uint8_t  strap[4] = {r->strap, r->pu_high, r->pd_high, r->np_high};
    uint16_t len      = board_record_build(page_buf, r->uid, p->written, strap, p->tlv, p->tlv_len);
    if (!len) {
        finish(r, WRITER_TOO_LONG);
        return;
    }
    int16_t page = board_record_append_page(block, &s);
    if (page < 0) {
        finish(r, WRITER_FULL);
        return;
    }
    if (p->flags & WRITER_FLAG_DRY) {
        r->page       = page; // where it would go
        r->record_len = len;
        finish(r, WRITER_DRY_OK);
        return;
    }

    // Program into an erased page only; never erase.
    const uint8_t *target = block + (uint32_t)page * BOARD_RECORD_PAGE;
    if (!page_erased(target)) {
        finish(r, WRITER_VERIFY_FAILED);
        return;
    }
    hal_program_page((uint16_t)page, page_buf);
    target = hal_block() + (uint32_t)page * BOARD_RECORD_PAGE;
    for (uint16_t i = 0; i < BOARD_RECORD_PAGE; i++) {
        if (target[i] != page_buf[i]) {
            r->page = page;
            finish(r, WRITER_VERIFY_FAILED);
            return;
        }
    }
    r->page       = page;
    r->record_len = board_record_check(target);
    finish(r, r->record_len == len ? WRITER_WRITTEN : WRITER_VERIFY_FAILED);
}
