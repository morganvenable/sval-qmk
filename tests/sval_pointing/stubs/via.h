#pragma once
#include <stdint.h>
#define VIA_EEPROM_CUSTOM_CONFIG_SIZE 100
uint32_t via_read_custom_config(void *data, uint32_t offset, uint32_t size);
uint32_t via_update_custom_config(const void *data, uint32_t offset, uint32_t size);
