#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "via.h"
uint8_t  test_storage[VIA_EEPROM_CUSTOM_CONFIG_SIZE];
bool     test_write_failed;
uint32_t test_write_first = UINT32_MAX, test_write_end; // span of every write so far
uint32_t via_read_custom_config(void *p, uint32_t offset, uint32_t size) {
    assert(offset + size <= VIA_EEPROM_CUSTOM_CONFIG_SIZE);
    memcpy(p, test_storage + offset, size);
    return size;
}
uint32_t via_update_custom_config(const void *p, uint32_t offset, uint32_t size) {
    assert(offset + size <= VIA_EEPROM_CUSTOM_CONFIG_SIZE);
    if (offset < test_write_first) test_write_first = offset;
    if (offset + size > test_write_end) test_write_end = offset + size;
    if (!test_write_failed) memcpy(test_storage + offset, p, size);
    return size;
}
bool sval_storage_write_failed(void) {
    return test_write_failed;
}

uint32_t test_user_word;
uint32_t eeconfig_read_user(void) {
    return test_user_word;
}
void eeconfig_update_user(uint32_t value) {
    if (!test_write_failed) test_user_word = value;
}
