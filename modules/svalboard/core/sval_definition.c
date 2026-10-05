// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "sval_definition_data.h"
#include <string.h>

// Get the size of the compressed keyboard definition
uint32_t sval_get_definition_size(void) {
    return SVAL_DEFINITION_SIZE;
}

// Get a chunk of the compressed keyboard definition
// Returns the number of bytes copied (up to max_size)
uint8_t sval_get_definition_chunk(uint16_t offset, uint8_t *buffer, uint8_t max_size) {
    if (offset >= SVAL_DEFINITION_SIZE) {
        return 0;
    }

    uint16_t remaining  = SVAL_DEFINITION_SIZE - offset;
    uint8_t  chunk_size = remaining < max_size ? remaining : max_size;

    // Copy from PROGMEM
    memcpy_P(buffer, &sval_definition_data[offset], chunk_size);

    return chunk_size;
}
