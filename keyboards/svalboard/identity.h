// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Board identity: persistent USB serial and user-chosen name (identity.c).

#define IDENTITY_SERIAL_BYTES 8
#define IDENTITY_NAME_MAX_BYTES 64 // UTF-8; the app limits names to 32 characters

typedef enum {
    IDENTITY_SERIAL_NONE     = 0,
    IDENTITY_SERIAL_FLASH_ID = 1, // the flash die's unique ID
    IDENTITY_SERIAL_RANDOM   = 2, // the die has no ID; 64 random bits, stored
} identity_serial_source_t;

typedef enum {
    IDENTITY_OK           = 0,
    IDENTITY_INVALID      = 1, // too long, or not valid UTF-8 text
    IDENTITY_UNAVAILABLE  = 2, // the flash is not 16 MB, so there is nowhere to keep it
    IDENTITY_WRITE_FAILED = 3,
} identity_status_t;

// The settings store used by earlier firmware has been looked at once already
// (and migrated if it held a setup). Kept here because it must survive a full
// settings wipe, or a wiped board would migrate the old setup back in.
#define IDENTITY_FLAG_LEGACY_STORE_CHECKED 0x01
// An attempt is pending until its settings and validity stamps are committed.
#define IDENTITY_FLAG_LEGACY_STORE_PENDING 0x02
bool identity_legacy_store_pending(void);
bool identity_mark_legacy_store_pending(void);
bool identity_legacy_store_checked(void);
void identity_mark_legacy_store_checked(void);

void                identity_init(void); // safe to call repeatedly; loads once
bool                identity_available(void);
const uint8_t      *identity_serial(identity_serial_source_t *source);
const char         *identity_name(void); // "" when unset
identity_status_t   identity_set_name(const char *name, uint8_t len);

// Host access over VIA custom values (protocol in identity.c).
#define IDENTITY_CHANNEL 0x49 // 'I'
enum { IDENTITY_OP_INFO = 0, IDENTITY_OP_NAME = 1, IDENTITY_OP_COMMIT = 2, IDENTITY_OP_REBOOT = 3 };
void identity_via_command(uint8_t *data, uint8_t length);
void identity_task(void); // from housekeeping: carries out a requested reboot
