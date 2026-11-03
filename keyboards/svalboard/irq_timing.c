// SPDX-License-Identifier: GPL-2.0-or-later
// Dev builds only (SVAL_IRQ_TIMING): the longest run of every interrupt
// handler, from ChibiOS's IRQ prologue and epilogue hooks
// (platforms/chibios/boards/common/configs/chconf.h), in microseconds on the
// 1 MHz timer. Nested handlers count inside the one they interrupt. Read by
// the touchpad dev command (0xFC op 0x42 action 8, azoteq/multitouch.c).
#include <stdint.h>
#include "hardware/structs/timer.h"

#define SVAL_IRQ_SLOTS 48 // exception numbers 0..47 (IRQ n is exception 16 + n)
volatile uint16_t sval_irq_max_us[SVAL_IRQ_SLOTS];
volatile uint32_t sval_irq_count[SVAL_IRQ_SLOTS];
static uint32_t   enter_t[8];
static uint8_t    enter_v[8];
static volatile uint8_t depth;

static inline uint32_t ipsr(void) {
    uint32_t r;
    __asm__ volatile("mrs %0, ipsr" : "=r"(r));
    return r & 0x3Fu;
}

void sval_irq_enter(void) {
    uint8_t d = depth;
    if (d < 8) {
        enter_t[d] = timer_hw->timerawl;
        enter_v[d] = (uint8_t)ipsr();
    }
    depth = d + 1;
}

void sval_irq_leave(void) {
    uint8_t d = depth;
    if (!d) return;
    d--;
    depth = d;
    if (d >= 8) return;
    uint32_t us = timer_hw->timerawl - enter_t[d];
    uint8_t  v  = enter_v[d] % SVAL_IRQ_SLOTS;
    sval_irq_count[v]++;
    if (us > sval_irq_max_us[v]) sval_irq_max_us[v] = us > 0xFFFF ? 0xFFFF : (uint16_t)us;
}
