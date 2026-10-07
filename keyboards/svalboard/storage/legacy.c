// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "store.h"
#include <string.h>

// The former QMK RP2040 wear-leveling driver stores inverted bytes. Decode it
// read-only: the upstream loader may erase/consolidate while replaying a bad log.
static bool read_old(uint32_t at, void *data, size_t n) {
    if (at > SVAL_STORE_LEGACY_SIZE || n > SVAL_STORE_LEGACY_SIZE - at || !sval_store_flash_read(SVAL_STORE_LEGACY_BASE + at, data, n)) return false;
    uint8_t *p = data;
    for (size_t i = 0; i < n; ++i)
        p[i] = ~p[i];
    return true;
}
static bool import_once(uint8_t *image) {
    if (!read_old(0, image, SVAL_STORE_SIZE)) return false;
    uint64_t checksum, expected = UINT64_C(0xCBF29CE484222325);
    bool     zero = true;
    for (uint32_t i = 0; i < SVAL_STORE_SIZE; ++i) {
        zero &= image[i] == 0;
        expected = (expected ^ image[i]) * UINT64_C(0x100000001B3);
    }
    if (!read_old(SVAL_STORE_SIZE, &checksum, sizeof(checksum)) || (checksum != expected && !(zero && checksum == 0))) return false;
    uint32_t at = SVAL_STORE_SIZE + 8;
    while (at < SVAL_STORE_LEGACY_SIZE) {
        uint8_t entry[8] = {0};
        if (!read_old(at, entry, 2)) return false;
        if (!(entry[0] | entry[1])) {
            // An erased hole in a populated log is not an empty/fresh store.
            uint8_t tail[SVAL_STORE_PAGE];
            while (at < SVAL_STORE_LEGACY_SIZE) {
                size_t n = SVAL_STORE_LEGACY_SIZE - at;
                if (n > sizeof(tail)) n = sizeof(tail);
                if (!read_old(at, tail, n)) return false;
                for (size_t i = 0; i < n; ++i)
                    if (tail[i]) return false;
                at += n;
            }
            return true;
        }
        at += 2;
        switch (entry[0] >> 6) {
            case 0: {
                uint32_t length = (entry[0] >> 3) & 7;
                if (!length || length > 5) return false;
                uint32_t extra = length > 3 ? 6 : length > 1 ? 4 : 2;
                if (!read_old(at, entry + 2, extra)) return false;
                at += extra;
                uint32_t address = ((uint32_t)(entry[0] & 7) << 16) | ((uint32_t)entry[1] << 8) | entry[2];
                if (address > SVAL_STORE_SIZE - length) return false;
                memcpy(image + address, entry + 3, length);
                break;
            }
            case 1:
                image[entry[0] & 63] = entry[1];
                break;
            case 2: {
                uint32_t address   = ((uint32_t)(entry[0] & 31) << 9) | ((uint32_t)entry[1] << 1);
                image[address]     = (entry[0] >> 5) & 1;
                image[address + 1] = 0;
                break;
            }
            default:
                return false;
        }
    }
    return true;
}
bool sval_store_import(uint8_t *image) {
    for (unsigned i = 0; i < 3; ++i)
        if (import_once(image)) return true;
    return false;
}
