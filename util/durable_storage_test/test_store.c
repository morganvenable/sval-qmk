// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "store.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[16 * 1024 * 1024], baseline[sizeof(flash)];
static int     operations, cut_at = -1, corrupt_read = -1;
static bool    capacity_ok = true;
static jmp_buf power_cut;
static void    mutate(uint32_t offset, const uint8_t *data, size_t length, bool erase) {
    assert(offset >= SVAL_STORE_BASE && offset + length <= SVAL_STORE_WITNESS + SVAL_STORE_ERASE);
    bool   cut = operations++ == cut_at;
    size_t n   = cut ? length / 2 : length;
    for (size_t i = 0; i < n; ++i) {
        if (erase)
            flash[offset + i] = 0xFF;
        else {
            assert((flash[offset + i] & data[i]) == data[i]); // real NOR: 1 -> 0 only
            flash[offset + i] &= data[i];
        }
    }
    if (cut) longjmp(power_cut, 1);
}
bool sval_store_flash_init(void) {
    return capacity_ok;
}
bool sval_store_flash_read(uint32_t offset, void *data, size_t length) {
    assert(offset + length <= sizeof(flash));
    memcpy(data, flash + offset, length);
    if (corrupt_read >= 0 && (uint32_t)corrupt_read >= offset && (uint32_t)corrupt_read - offset < length) {
        ((uint8_t *)data)[corrupt_read - offset] ^= 1;
        corrupt_read = -1; // one transient read error, not a persistent bit flip
    }
    return true;
}
bool sval_store_flash_erase(uint32_t offset, uint32_t length) {
    assert((length == SVAL_STORE_SECTOR || length == SVAL_STORE_ERASE) && offset % length == 0);
    mutate(offset, NULL, length, true);
    return true;
}
bool sval_store_flash_program(uint32_t offset, const void *page) {
    assert(offset % SVAL_STORE_PAGE == 0);
    mutate(offset, page, SVAL_STORE_PAGE, false);
    return true;
}
static void reboot(void) {
    cut_at     = -1;
    operations = 0;
    sval_store_init();
}
static uint8_t read_byte(uint32_t address) {
    uint8_t v;
    sval_store_read(address, &v, 1);
    return v;
}
static void write_byte(uint32_t address, uint8_t v) {
    assert(sval_store_write(address, &v, 1));
}
static void old_write(uint32_t at, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; ++i)
        flash[SVAL_STORE_LEGACY_BASE + at + i] = ~p[i];
}
static void old_fixture(void) {
    memset(flash, 0xFF, sizeof(flash));
    static uint8_t image[SVAL_STORE_SIZE];
    for (uint32_t i = 0; i < sizeof(image); ++i)
        image[i] = (i * 7u) ^ (i >> 8);
    old_write(0, image, sizeof(image));
    uint64_t fnv = UINT64_C(0xCBF29CE484222325);
    for (uint32_t i = 0; i < sizeof(image); ++i)
        fnv = (fnv ^ image[i]) * UINT64_C(0x100000001B3);
    old_write(sizeof(image), &fnv, sizeof(fnv));
    // All three historical QMK log encodings, including a >64 KiB address.
    uint8_t entries[] = {0x45, 0xAB, 0xA0, 0x08, 0x29, 0x11, 0x70, 1, 2, 3, 4, 5};
    old_write(SVAL_STORE_SIZE + 8, entries, sizeof(entries));
}
static void assert_import(void) {
    assert(read_byte(5) == 0xAB);
    assert(read_byte(16) == 1 && read_byte(17) == 0);
    for (uint32_t i = 0; i < 5; ++i)
        assert(read_byte(70000 + i) == i + 1);
    assert(read_byte(120000) == (uint8_t)((120000u * 7u) ^ (120000u >> 8)));
}
static void import_and_retry(void) {
    old_fixture();
    memcpy(baseline, flash, sizeof(flash));
    corrupt_read = SVAL_STORE_LEGACY_BASE + 120000;
    reboot();
    assert(sval_store_status() == SVAL_STORE_IMPORTED);
    assert_import();
    assert(!memcmp(flash + SVAL_STORE_LEGACY_BASE, baseline + SVAL_STORE_LEGACY_BASE, SVAL_STORE_LEGACY_SIZE));
    reboot();
    assert_import();
    assert(operations == 0);
    puts("PASS: full legacy import, all log encodings, transient read retry, old flash unchanged");
}
static void recovery_and_edits(void) {
    write_byte(100, 17);
    assert(sval_store_flush()); // new bank 0, previous bank 1 also has the edit
    flash[SVAL_STORE_BASE + SVAL_STORE_IMAGE + 90000] ^= 1;
    reboot();
    assert(sval_store_status() == SVAL_STORE_RECOVERED);
    assert(read_byte(100) == 17);
    write_byte(100, 23);
    assert(sval_store_flush()); // repairs bank 0 from verified recovered state
    reboot();
    assert(read_byte(100) == 23);
    write_byte(100, 29);
    reboot();
    assert(read_byte(100) == 29);
    reboot();
    assert(read_byte(100) == 29);
    puts("PASS: permanent corruption fallback, subsequent edits survive repeated reboot");
}
static void snapshot_cuts(void) {
    write_byte(100, 42);
    memcpy(baseline, flash, sizeof(flash));
    operations = 0;
    assert(sval_store_flush());
    int count = operations;
    for (int step = 0; step < count; ++step) {
        memcpy(flash, baseline, sizeof(flash));
        reboot();
        operations = 0;
        cut_at     = step;
        if (!setjmp(power_cut)) {
            sval_store_flush();
            assert(!"power cut was not reached");
        }
        reboot();
        assert(read_byte(100) == 42);
        assert_import();
        assert(sval_store_status() != SVAL_STORE_READ_ONLY);
    }
    printf("PASS: power cut during every snapshot erase/program operation (%d cuts)\n", count);
}
static void append_cut(void) {
    assert(sval_store_flush());
    operations = 0;
    cut_at     = 0;
    if (!setjmp(power_cut)) {
        write_byte(100, 99);
        assert(!"power cut was not reached");
    }
    reboot();
    assert(read_byte(100) == 42);
    assert(sval_store_status() == SVAL_STORE_RECOVERED);
    write_byte(100, 77);
    reboot();
    assert(read_byte(100) == 77);
    puts("PASS: torn final journal page retains prefix; next edit safely retires damaged bank");
}
static void all_bad_read_only(void) {
    flash[SVAL_STORE_BASE + SVAL_STORE_IMAGE + 500] ^= 1;
    flash[SVAL_STORE_BASE + SVAL_STORE_BANK_SIZE + SVAL_STORE_IMAGE + 500] ^= 1;
    memcpy(baseline, flash, sizeof(flash));
    reboot();
    assert(sval_store_status() == SVAL_STORE_READ_ONLY);
    assert(!sval_store_clear());
    uint8_t defaults[3] = {1, 2, 3};
    assert(!sval_store_write(0, defaults, sizeof(defaults)));
    assert(!sval_store_flush());
    assert(operations == 0 && !memcmp(flash, baseline, sizeof(flash)));
    capacity_ok = false;
    reboot();
    assert(sval_store_status() == SVAL_STORE_READ_ONLY && operations == 0);
    capacity_ok = true;
    assert(sval_store_prepare_reset());
    assert(sval_store_clear());
    write_byte(100, 88);
    reboot();
    assert(read_byte(100) == 88);
    puts("PASS: both banks corrupt or undersized die: no erase, no stale import, volatile defaults only");
    puts("PASS: explicit factory reset exits read-only mode and new settings survive reboot");
}
static void import_cuts(void) {
    old_fixture();
    memcpy(baseline, flash, sizeof(flash));
    // Cover the first snapshot, second snapshot, and migration-witness write.
    const int cuts[] = {0, 15, 16, 270, 527, 528, 529, 544, 545, 800, 1056, 1057, 1058, 1059};
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        memcpy(flash, baseline, sizeof(flash));
        operations = 0;
        cut_at     = cuts[i];
        if (!setjmp(power_cut)) {
            sval_store_init();
            assert(!"power cut was not reached");
        }
        reboot();
        assert_import();
        assert(sval_store_status() != SVAL_STORE_READ_ONLY);
        write_byte(100, 61);
        reboot();
        assert(read_byte(100) == 61);
    }
    puts("PASS: interrupted initial migration and witness creation resume without losing configuration");
}
static void malformed_legacy(void) {
    old_fixture();
    flash[SVAL_STORE_LEGACY_BASE + SVAL_STORE_SIZE + 8] = 0xFF ^ 0x3F; // invalid length 7
    memcpy(baseline, flash, sizeof(flash));
    reboot();
    assert(sval_store_status() == SVAL_STORE_READ_ONLY);
    assert(!memcmp(baseline, flash, sizeof(flash)));
    old_fixture();
    flash[SVAL_STORE_LEGACY_BASE + 90000] ^= 1;
    reboot();
    assert(sval_store_status() == SVAL_STORE_READ_ONLY && operations == 0);
    puts("PASS: malformed or checksum-invalid legacy data stays untouched");
}
static void new_store_reads_and_rollover(void) {
    old_fixture();
    reboot();
    write_byte(100, 7);
    const uint32_t faults[] = {SVAL_STORE_BASE + SVAL_STORE_BANK_SIZE, SVAL_STORE_BASE + SVAL_STORE_BANK_SIZE + SVAL_STORE_IMAGE + 80000, SVAL_STORE_BASE + SVAL_STORE_BANK_SIZE + SVAL_STORE_LOG + 20};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        corrupt_read = faults[i];
        reboot();
        assert(read_byte(100) == 7 && operations == 0);
    }
    uint8_t macro[700], readback[700];
    for (unsigned i = 0; i < sizeof(macro); ++i)
        macro[i] = i * 3;
    assert(sval_store_write(100000, macro, sizeof(macro)));
    reboot();
    sval_store_read(100000, readback, sizeof(readback));
    assert(!memcmp(macro, readback, sizeof(macro)));
    const unsigned pages = (SVAL_STORE_BANK_SIZE - SVAL_STORE_LOG) / SVAL_STORE_PAGE;
    for (unsigned i = 0; i < pages + 2; ++i)
        write_byte(100, i % 251);
    reboot();
    assert(read_byte(100) == (pages + 1) % 251);
    sval_store_read(100000, readback, sizeof(readback));
    assert(!memcmp(macro, readback, sizeof(macro)));
    assert(sval_store_flush());
    operations = 0;
    assert(sval_store_flush() && operations == 0);
    assert(!sval_store_write(SVAL_STORE_SIZE, macro, 1));
    assert(!sval_store_write(UINT32_MAX, macro, sizeof(macro)));
    puts("PASS: transient header/image/log reads, full journal rollover, macro persistence, no-op flush, bounds");
}
static void background_snapshot_edit(void) {
    old_fixture();
    reboot();
    write_byte(100, 51);
    // Erase the destination and write some snapshot pages, then edit a byte
    // already copied. The partly-built snapshot must never be committed.
    for (unsigned i = 0; i < SVAL_STORE_BANK_SIZE / SVAL_STORE_SECTOR + 4; ++i)
        assert(sval_store_flush_step());
    assert(sval_store_flush_pending());
    write_byte(100, 52);
    assert(!sval_store_flush_pending());
    assert(sval_store_flush_step());
    while (sval_store_flush_pending())
        assert(sval_store_flush_step());
    reboot();
    assert(read_byte(100) == 52);
    puts("PASS: edits during incremental backup cancel the old snapshot and survive reboot");
}
int main(void) {
    import_and_retry();
    recovery_and_edits();
    snapshot_cuts();
    append_cut();
    all_bad_read_only();
    import_cuts();
    malformed_legacy();
    new_store_reads_and_rollover();
    background_snapshot_edit();
    puts("All durable storage tests passed");
}
