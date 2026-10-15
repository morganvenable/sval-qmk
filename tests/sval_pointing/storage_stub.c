#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
uint8_t  test_storage[100];
bool     test_write_failed;
uint32_t via_read_custom_config(void *p, uint32_t offset, uint32_t size) {
    assert(offset + size <= 100);
    memcpy(p, test_storage + offset, size);
    return size;
}
uint32_t via_update_custom_config(const void *p, uint32_t offset, uint32_t size) {
    assert(offset + size <= 100);
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
