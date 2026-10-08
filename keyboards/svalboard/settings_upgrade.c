// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// Settings live at 0x160000 in QMK's wear-leveling format. A board that ran the
// previous firmware also holds its dual-bank store (0x200000..0x40FFFF), and at
// 0x160000 settings from before that, in today's format. On the first boot
// after that upgrade, erase all of it before settings load, so the stale copy
// is never read as current; users reload their layout file.
//
// The previous store's bank B header (0x300000) and migration marker (0x400000)
// identify such a board. Today's store never writes there, and they are erased
// last: if power is lost part way, the next boot finishes the job.
#define OLD_START 0x160000u
#define OLD_END 0x410000u
#define OLD_BANK_B 0x300000u
#define OLD_MARKER 0x400000u
#define OLD_MAGIC 0x31565353u // "SSV1"
#define BLOCK (64u * 1024u)

extern const uint8_t BOOT2_ROM[256];
static uint32_t      boot2[64];

static uint32_t read32(uint32_t offset) {
    return *(const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + offset);
}

static void __no_inline_not_in_flash_func(erase_block)(uint32_t offset) {
    rom_connect_internal_flash_fn connect  = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn         exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn      erase    = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_flush_cache_fn      flush    = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    __compiler_memory_barrier();
    connect();
    exit_xip();
    erase(offset, BLOCK, BLOCK, 0xD8);
    flush();
    ((void (*)(void))((intptr_t)boot2 + 1))();
}

static void erase(uint32_t offset) {
    for (uint32_t i = 0; i < BLOCK; i += sizeof(uint32_t)) {
        if (read32(offset + i) != 0xFFFFFFFFu) {
            uint32_t irq = save_and_disable_interrupts();
            erase_block(offset);
            restore_interrupts(irq);
            return;
        }
    }
}

// Runs before the EEPROM driver reads settings, and before USB starts.
void keyboard_pre_eeprom_init_kb(void) {
    if (read32(OLD_BANK_B) != OLD_MAGIC && read32(OLD_MARKER) != OLD_MAGIC) return;
    memcpy(boot2, BOOT2_ROM, sizeof(boot2));
    for (uint32_t offset = OLD_START; offset < OLD_END; offset += BLOCK) {
        if (offset != OLD_BANK_B && offset != OLD_MARKER) erase(offset);
    }
    erase(OLD_BANK_B);
    erase(OLD_MARKER);
}
