// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include "store.h"
#include "eeprom_driver.h"
#include "timer.h"
#include "keyboard.h"
#include "eeconfig.h"

_Static_assert(EEPROM_SIZE == SVAL_STORE_SIZE, "storage geometry must preserve EEPROM addresses");

static uint32_t changed_at, checked_at;

void eeprom_driver_init(void) {
    sval_store_init();
}
void eeprom_driver_erase(void) {
    sval_store_clear();
}
void eeprom_driver_format(bool erase) {
    (void)erase;
    sval_store_clear();
}
void eeprom_read_block(void *data, const void *address, size_t length) {
    sval_store_read((uintptr_t)address, data, length);
}
void eeprom_write_block(const void *data, void *address, size_t length) {
    sval_store_write((uintptr_t)address, data, length);
    changed_at = timer_read32();
}

void sval_storage_task(void) {
    // Journal writes are immediately durable. A quiet-time snapshot also limits
    // rollback if an entire bank later suffers permanent corruption.
    if (timer_elapsed32(changed_at) < 5000 || last_input_activity_elapsed() < 5000) return;
    if (!sval_store_flush_pending()) {
        if (timer_elapsed32(checked_at) < 15000) return;
        checked_at = timer_read32();
    }
    sval_store_flush_step();
}

#ifdef BOOTMAGIC_ENABLE
void bootmagic_reset_eeprom(void) {
    if (sval_store_prepare_reset()) eeconfig_disable();
}
#endif
