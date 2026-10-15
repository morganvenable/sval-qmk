#pragma once
#include <stdint.h>
#define EECONFIG_USER_DATA_SIZE 0
uint32_t eeconfig_read_user(void);
void     eeconfig_update_user(uint32_t value);
