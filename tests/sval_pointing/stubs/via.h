#pragma once
#include <stdint.h>
// The board's real map, so the tests see the addresses the firmware uses.
#include "via_custom_map.h"
#define VIA_EEPROM_CUSTOM_CONFIG_SIZE SVAL_VIA_CUSTOM_SIZE
uint32_t via_read_custom_config(void *data, uint32_t offset, uint32_t size);
uint32_t via_update_custom_config(const void *data, uint32_t offset, uint32_t size);
