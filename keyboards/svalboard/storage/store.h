// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keep logical geometry identical to the previous Sval EEPROM driver.
#define SVAL_STORE_SIZE (128u * 1024u)
#define SVAL_STORE_BASE 0x200000u
#define SVAL_STORE_BANK_SIZE (1024u * 1024u)
#define SVAL_STORE_PAGE 256u
#define SVAL_STORE_SECTOR 4096u
#define SVAL_STORE_ERASE (64u * 1024u)
#define SVAL_STORE_WITNESS (SVAL_STORE_BASE + 2 * SVAL_STORE_BANK_SIZE)
#define SVAL_STORE_IMAGE SVAL_STORE_SECTOR
#define SVAL_STORE_LOG (SVAL_STORE_IMAGE + SVAL_STORE_SIZE)
#define SVAL_STORE_DATA 236u
#define SVAL_STORE_LEGACY_BASE 0x160000u
#define SVAL_STORE_LEGACY_SIZE (512u * 1024u)

typedef enum {
    SVAL_STORE_OK,
    SVAL_STORE_IMPORTED,
    SVAL_STORE_RECOVERED,
    SVAL_STORE_READ_ONLY,
} sval_store_status_t;

void sval_store_init(void);
void sval_store_read(uint32_t address, void *data, size_t length);
bool sval_store_write(uint32_t address, const void *data, size_t length);
bool sval_store_clear(void);
bool sval_store_flush(void);
bool sval_store_flush_step(void);
bool sval_store_flush_pending(void);
// Only explicit user reset gestures may lift the read-only recovery latch.
bool                sval_store_prepare_reset(void);
void                sval_storage_task(void);
sval_store_status_t sval_store_status(void);

// Hardware boundary. All offsets are absolute flash offsets, never XIP pointers.
bool sval_store_flash_init(void);
bool sval_store_flash_read(uint32_t offset, void *data, size_t length);
bool sval_store_flash_erase(uint32_t offset, uint32_t length); // 4 KiB sector or 64 KiB block
bool sval_store_flash_program(uint32_t offset, const void *page);
// Read-only import of the immediately preceding svalboard/qmk format, not Vial.
bool sval_store_import(uint8_t *image);
