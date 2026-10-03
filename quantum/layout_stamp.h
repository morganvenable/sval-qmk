// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stddef.h>

// Layout stamps: a hash of the compile-time values that decide how a stored
// region is read (start address, entry counts, entry sizes, keycode numbering).
// A region is trusted only while its stored stamp matches, so the stamp changes
// exactly when the meaning of the stored bytes changes and never on a plain
// rebuild. Always derive the inputs from the same constants the reader uses;
// a hand-maintained number alone is easy to forget to bump, and forgetting turns
// a reset into a keyboard that misreads its own settings.

#define LAYOUT_STAMP_FNV_OFFSET 0x811C9DC5u
#define LAYOUT_STAMP_FNV_PRIME 0x01000193u

static inline uint32_t layout_stamp(const uint32_t *values, size_t count) {
    uint32_t hash = LAYOUT_STAMP_FNV_OFFSET;
    for (size_t i = 0; i < count; i++) {
        uint32_t v = values[i];
        for (uint8_t b = 0; b < 4; b++) {
            hash ^= (uint8_t)(v >> (b * 8));
            hash *= LAYOUT_STAMP_FNV_PRIME;
        }
    }
    return hash;
}
