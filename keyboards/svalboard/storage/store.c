// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "store.h"
#include <string.h>

#define BANK_MAGIC 0x31565353u // SSV1
#define LOG_MAGIC 0x314C5653u  // SVL1
#define RETRIES 3

typedef struct {
    uint32_t magic, generation, size, image_crc;
    uint8_t  reserved[236];
    uint32_t crc;
} bank_header_t;
typedef struct {
    uint32_t magic, address, length, ordinal;
    uint8_t  data[SVAL_STORE_DATA];
    uint32_t crc;
} log_page_t;
_Static_assert(sizeof(bank_header_t) == SVAL_STORE_PAGE, "header page size");
_Static_assert(sizeof(log_page_t) == SVAL_STORE_PAGE, "journal page size");
_Static_assert(SVAL_STORE_BASE >= SVAL_STORE_LEGACY_BASE + SVAL_STORE_LEGACY_SIZE, "preserve old EEPROM");
_Static_assert(SVAL_STORE_BASE + 2 * SVAL_STORE_BANK_SIZE <= 0xFFE000u, "preserve board identity");

static uint8_t             cache[SVAL_STORE_SIZE];
static int                 active;
static uint32_t            generation, next;
static bool                writable, needs_checkpoint, dirty;
static bool                checkpoint_pending;
static int                 checkpoint_target;
static uint32_t            checkpoint_at;
static sval_store_status_t health;

static uint32_t crc32(const void *data, size_t length) {
    const uint8_t *p   = data;
    uint32_t       crc = 0xFFFFFFFFu;
    while (length--) {
        crc ^= *p++;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static uint32_t base(int bank) {
    return SVAL_STORE_BASE + (uint32_t)bank * SVAL_STORE_BANK_SIZE;
}
static bool blank(const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--)
        if (*p++ != 0xFF) return false;
    return true;
}
static bool range_blank(uint32_t offset, uint32_t end) {
    uint8_t page[SVAL_STORE_PAGE];
    while (offset < end) {
        if (!sval_store_flash_read(offset, page, sizeof(page)) || !blank(page, sizeof(page))) return false;
        offset += sizeof(page);
    }
    return true;
}
static bool read_header(int bank, bank_header_t *h) {
    for (unsigned i = 0; i < RETRIES; ++i) {
        if (sval_store_flash_read(base(bank), h, sizeof(*h)) && h->magic == BANK_MAGIC && h->size == SVAL_STORE_SIZE && h->crc == crc32(h, offsetof(bank_header_t, crc))) return true;
    }
    return false;
}
static bool newer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

// Written before the first post-import edit. Once present, old settings must
// never be imported again. A power cut during the initial snapshot can retry
// from the untouched legacy store while this page is still erased.
static bool witness(void) {
    bank_header_t h;
    memset(&h, 0xFF, sizeof(h));
    h.magic      = BANK_MAGIC;
    h.generation = 0;
    h.size       = SVAL_STORE_SIZE;
    h.image_crc  = 0;
    h.crc        = crc32(&h, offsetof(bank_header_t, crc));
    bank_header_t stored;
    for (unsigned i = 0; i < RETRIES; ++i) {
        if (sval_store_flash_read(SVAL_STORE_WITNESS, &stored, sizeof(stored)) && !memcmp(&h, &stored, sizeof(h))) return true;
    }
    // Only called with a committed bank available, so interrupted witness
    // repair cannot lose configuration or make a stale import necessary.
    if (!sval_store_flash_erase(SVAL_STORE_WITNESS, SVAL_STORE_ERASE) || !sval_store_flash_program(SVAL_STORE_WITNESS, &h)) return false;
    return sval_store_flash_read(SVAL_STORE_WITNESS, &stored, sizeof(stored)) && !memcmp(&h, &stored, sizeof(h));
}

// A CRC-invalid final page may be a torn append: keep the verified prefix and
// retire the bank before writing again. Invalid data with later records is not
// a torn tail; try the other bank instead. Retry before making either decision.
static bool load(int bank, const bank_header_t *h) {
    for (unsigned attempt = 0; attempt < RETRIES; ++attempt) {
        if (!sval_store_flash_read(base(bank) + SVAL_STORE_IMAGE, cache, sizeof(cache)) || crc32(cache, sizeof(cache)) != h->image_crc) continue;
        next             = SVAL_STORE_LOG;
        needs_checkpoint = false;
        bool failed      = false;
        while (next < SVAL_STORE_BANK_SIZE) {
            log_page_t p;
            if (!sval_store_flash_read(base(bank) + next, &p, sizeof(p))) {
                failed = true;
                break;
            }
            if (blank(&p, sizeof(p))) {
                // An erased hole followed by data is corruption, not end of log.
                if (!range_blank(base(bank) + next + sizeof(p), base(bank) + SVAL_STORE_BANK_SIZE)) failed = true;
                break;
            }
            if (p.magic != LOG_MAGIC || p.ordinal != (next - SVAL_STORE_LOG) / SVAL_STORE_PAGE || !p.length || p.length > SVAL_STORE_DATA || p.address > SVAL_STORE_SIZE - p.length || p.crc != crc32(&p, offsetof(log_page_t, crc))) {
                if (attempt + 1 == RETRIES && range_blank(base(bank) + next + sizeof(p), base(bank) + SVAL_STORE_BANK_SIZE)) {
                    needs_checkpoint = true;
                    break;
                }
                failed = true;
                break;
            }
            memcpy(cache + p.address, p.data, p.length);
            next += sizeof(p);
        }
        if (!failed) return true;
    }
    return false;
}

static bool program_checked(uint32_t offset, const void *page) {
    uint8_t verify[SVAL_STORE_PAGE];
    if (!sval_store_flash_program(offset, page)) return false;
    for (unsigned i = 0; i < RETRIES; ++i) {
        if (sval_store_flash_read(offset, verify, sizeof(verify)) && memcmp(page, verify, sizeof(verify)) == 0) return true;
    }
    return false;
}

// Never erase the active bank. Header is the commit record and is written LAST,
// after snapshot readback and erased journal verification. Every flash operation
// is sector/page sized so interrupts can run between operations.
static void checkpoint_start(void) {
    checkpoint_target  = active == 0 ? 1 : 0;
    checkpoint_at      = 0;
    checkpoint_pending = true;
}
static bool checkpoint_step(bool background) {
    if (checkpoint_at < SVAL_STORE_BANK_SIZE) {
        uint32_t at     = base(checkpoint_target) + checkpoint_at;
        uint32_t length = background ? SVAL_STORE_SECTOR : SVAL_STORE_ERASE;
        if (!sval_store_flash_erase(at, length) || !range_blank(at, at + length)) return false;
        checkpoint_at += length;
        return true;
    }
    uint32_t image_at = checkpoint_at - SVAL_STORE_BANK_SIZE;
    if (image_at < SVAL_STORE_SIZE) {
        if (!program_checked(base(checkpoint_target) + SVAL_STORE_IMAGE + image_at, cache + image_at)) return false;
        checkpoint_at += SVAL_STORE_PAGE;
        return true;
    }
    bank_header_t h;
    memset(&h, 0xFF, sizeof(h));
    h.magic      = BANK_MAGIC;
    h.generation = generation + 1;
    h.size       = SVAL_STORE_SIZE;
    h.image_crc  = crc32(cache, sizeof(cache));
    h.crc        = crc32(&h, offsetof(bank_header_t, crc));
    if (!program_checked(base(checkpoint_target), &h)) return false;
    active             = checkpoint_target;
    generation         = h.generation;
    next               = SVAL_STORE_LOG;
    needs_checkpoint   = false;
    dirty              = false;
    checkpoint_pending = false;
    return true;
}
static bool checkpoint(void) {
    checkpoint_start();
    while (checkpoint_pending)
        if (!checkpoint_step(false)) {
            checkpoint_pending = false;
            return false;
        }
    return true;
}

void sval_store_init(void) {
    active             = -1;
    generation         = 0;
    writable           = false;
    needs_checkpoint   = false;
    dirty              = false;
    checkpoint_pending = false;
    health             = SVAL_STORE_READ_ONLY;
    memset(cache, 0, sizeof(cache));
    if (!sval_store_flash_init()) return;
    bank_header_t h[2];
    bool          valid[2] = {read_header(0, &h[0]), read_header(1, &h[1])};
    int           first    = valid[1] && (!valid[0] || newer(h[1].generation, h[0].generation)) ? 1 : 0;
    for (int n = 0; n < 2; ++n) {
        int bank = first ^ n;
        if (valid[bank] && load(bank, &h[bank])) {
            active     = bank;
            generation = h[bank].generation;
            writable   = witness();
            health     = n || !valid[bank ^ 1] || needs_checkpoint ? SVAL_STORE_RECOVERED : SVAL_STORE_OK;
            if (!writable) health = SVAL_STORE_READ_ONLY;
            dirty = next != SVAL_STORE_LOG || needs_checkpoint || n || !valid[bank ^ 1];
            return;
        }
    }
    // Retry an interrupted first import, but never resurrect the frozen old
    // store after the new store has accepted edits (witness present).
    memset(cache, 0, sizeof(cache));
    bool unimported = true;
    for (unsigned i = 0; i < RETRIES; ++i)
        unimported &= range_blank(SVAL_STORE_WITNESS, SVAL_STORE_WITNESS + SVAL_STORE_ERASE);
    if (unimported && sval_store_import(cache)) {
        writable = true;
        if (checkpoint() && checkpoint() && witness()) {
            health = SVAL_STORE_IMPORTED;
            return;
        }
        writable = false;
    }
    // QMK can initialize volatile defaults, but cannot overwrite forensic data.
    memset(cache, 0, sizeof(cache));
}

void sval_store_read(uint32_t address, void *data, size_t length) {
    if (address <= SVAL_STORE_SIZE && length <= SVAL_STORE_SIZE - address)
        memcpy(data, cache + address, length);
    else
        memset(data, 0, length);
}
bool sval_store_write(uint32_t address, const void *data, size_t length) {
    if (address > SVAL_STORE_SIZE || length > SVAL_STORE_SIZE - address) return false;
    const uint8_t *p = data;
    if (!writable) {
        memcpy(cache + address, p, length);
        return false;
    }
    while (length) {
        size_t n = length > SVAL_STORE_DATA ? SVAL_STORE_DATA : length;
        if (memcmp(cache + address, p, n)) {
            // The background snapshot may contain pages from before this edit.
            // Abandon it; its header is uncommitted and the active bank is safe.
            checkpoint_pending = false;
            if ((needs_checkpoint || next == SVAL_STORE_BANK_SIZE) && !checkpoint()) goto failed;
            log_page_t record;
            memset(&record, 0xFF, sizeof(record));
            record.magic   = LOG_MAGIC;
            record.address = address;
            record.length  = n;
            record.ordinal = (next - SVAL_STORE_LOG) / SVAL_STORE_PAGE;
            memcpy(record.data, p, n);
            record.crc = crc32(&record, offsetof(log_page_t, crc));
            if (!program_checked(base(active) + next, &record)) goto failed;
            memcpy(cache + address, p, n);
            next += sizeof(record);
            dirty = true;
        }
        address += n;
        p += n;
        length -= n;
    }
    return true;
failed:
    // The append might be partial. Never program over it or acknowledge it in RAM.
    writable = false;
    health   = SVAL_STORE_READ_ONLY;
    return false;
}
bool sval_store_clear(void) {
    // QMK uses this for automatic initialization as well as explicit reset.
    // Never let the automatic path authorize erasing an unreadable store.
    memset(cache, 0, sizeof(cache));
    checkpoint_pending = false;
    if (!writable) return false;
    if (checkpoint()) return true;
    writable = false;
    health   = SVAL_STORE_READ_ONLY;
    return false;
}
sval_store_status_t sval_store_status(void) {
    return health;
}
bool sval_store_flush(void) {
    if (!writable) return false;
    if (!dirty) return true;
    if (checkpoint()) return true;
    writable = false;
    health   = SVAL_STORE_READ_ONLY;
    return false;
}
bool sval_store_flush_pending(void) {
    return checkpoint_pending;
}
bool sval_store_flush_step(void) {
    if (!writable) return false;
    if (!dirty) return true;
    if (!checkpoint_pending) checkpoint_start();
    if (checkpoint_step(true)) return true;
    checkpoint_pending = false;
    writable           = false;
    health             = SVAL_STORE_READ_ONLY;
    return false;
}
bool sval_store_prepare_reset(void) {
    if (writable) return true;
    if (!sval_store_flash_init()) return false;
    writable = true;
    if (sval_store_clear() && witness()) {
        health = SVAL_STORE_OK;
        return true;
    }
    writable = false;
    health   = SVAL_STORE_READ_ONLY;
    return false;
}
