// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Host stand-ins for the Pico SDK and boot ROM pieces update_flash.c uses
// (built with -DSVAL_UPDATER_HOST_TEST). The flash die is a 16 MiB array; the
// ROM functions are mocks in test_updater.c that check their calling sequence
// and can tear an operation.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_HOST_FLASH_BYTES (16u * 1024 * 1024)
extern uint8_t update_host_flash[UPDATE_HOST_FLASH_BYTES];
#define UPDATE_NOCACHE(off) ((const volatile uint8_t *)(update_host_flash + (off)))

#define __no_inline_not_in_flash_func(name) name
#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")

typedef void (*rom_connect_internal_flash_fn)(void);
typedef void (*rom_flash_exit_xip_fn)(void);
typedef void (*rom_flash_range_erase_fn)(uint32_t, size_t, uint32_t, uint8_t);
typedef void (*rom_flash_range_program_fn)(uint32_t, const uint8_t *, size_t);
typedef void (*rom_flash_flush_cache_fn)(void);
enum {
    ROM_FUNC_CONNECT_INTERNAL_FLASH = 1,
    ROM_FUNC_FLASH_EXIT_XIP,
    ROM_FUNC_FLASH_RANGE_ERASE,
    ROM_FUNC_FLASH_RANGE_PROGRAM,
    ROM_FUNC_FLASH_FLUSH_CACHE,
};
void *rom_func_lookup_inline(uint32_t code);

uint32_t save_and_disable_interrupts(void);
void     restore_interrupts(uint32_t status);
void     flash_do_cmd(const uint8_t *tx, uint8_t *rx, size_t count);
void     update_host_enter_xip(const uint32_t *boot2_copy);

extern const uint8_t BOOT2_ROM[256];
