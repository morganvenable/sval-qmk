#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "report.h"
#include "timer.h"
#include "keycodes.h"
#define QK_KB_0 0x7E00
#define MATRIX_ROWS 10
#define MATRIX_COLS 6
typedef struct {
    struct {
        struct {
            uint8_t row, col;
        } key;
        bool pressed;
    } event;
} keyrecord_t;
bool is_keyboard_master(void);

#define POINTING_DEVICE_HIRES_SCROLL_ENABLE
#define POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER 120
