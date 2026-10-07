// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "store.h"
#include <string.h>
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// ROM routines permit the real 16 MiB range; the SDK wrappers are limited to
// PICO_FLASH_SIZE_BYTES (2 MiB on this board definition). Check JEDEC first.
extern const uint8_t BOOT2_ROM[256];
static uint32_t      boot2[64];
static uint8_t       page_buffer[SVAL_STORE_PAGE] __attribute__((aligned(4)));
static bool          available;

static void __no_inline_not_in_flash_func(storage_flash_op)(uint32_t at, const uint8_t *page, uint32_t erase_length) {
    rom_connect_internal_flash_fn connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn    program  = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn      flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    __compiler_memory_barrier();
    connect();
    exit_xip();
    if (page)
        program(at, page, SVAL_STORE_PAGE);
    else
        erase(at, erase_length, SVAL_STORE_ERASE, 0xD8);
    flush();
    ((void (*)(void))((intptr_t)boot2 + 1))();
}
bool sval_store_flash_init(void) {
    memcpy(boot2, BOOT2_ROM, sizeof(boot2));
    uint8_t  tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, sizeof(tx));
    restore_interrupts(irq);
    available = rx[3] == 0x18;
    return available;
}
bool sval_store_flash_read(uint32_t at, void *data, size_t length) {
    if (!available || at > 0x1000000u || length > 0x1000000u - at) return false;
    // Volatile, uncached access makes retries perform real flash reads.
    const volatile uint8_t *p   = (const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + at);
    uint8_t                *out = data;
    for (size_t i = 0; i < length; ++i)
        out[i] = p[i];
    return true;
}
static bool write_range(uint32_t at, uint32_t length) {
    return available && at >= SVAL_STORE_BASE && at <= SVAL_STORE_WITNESS + SVAL_STORE_ERASE - length;
}
bool sval_store_flash_erase(uint32_t at, uint32_t length) {
    if ((length != SVAL_STORE_SECTOR && length != SVAL_STORE_ERASE) || !write_range(at, length) || at % length) return false;
    uint32_t irq = save_and_disable_interrupts();
    storage_flash_op(at, NULL, length);
    restore_interrupts(irq);
    return true; // engine verifies the resulting erased bytes
}
bool sval_store_flash_program(uint32_t at, const void *page) {
    if (!write_range(at, SVAL_STORE_PAGE) || at % SVAL_STORE_PAGE) return false;
    memcpy(page_buffer, page, sizeof(page_buffer));
    uint32_t irq = save_and_disable_interrupts();
    storage_flash_op(at, page_buffer, 0);
    restore_interrupts(irq);
    return true; // engine verifies every programmed page
}
