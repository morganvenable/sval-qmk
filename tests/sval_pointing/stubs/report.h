#pragma once
#include <stdint.h>
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_NKRO 6
#define REPORT_ID_CONSUMER 4
#define REPORT_ID_SYSTEM 3
typedef struct {
    uint8_t mods, reserved, keys[6];
} report_keyboard_t;
typedef struct {
    uint8_t report_id, mods, bits[30];
} report_nkro_t;
typedef struct {
    uint8_t  report_id;
    uint16_t usage;
} report_extra_t;
typedef struct {
    uint8_t buttons;
    int16_t x, y, h, v;
} report_mouse_t;

#define MOUSE_REPORT_HV_MAX 32767
#define MOUSE_REPORT_HV_MIN (-32767)
