// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Updater LED states (see update_led.h).
//
// D20, holding rgblight off: SVAL_UPDATER builds set RGBLIGHT_DRIVER = custom,
// and this file is rgblight's driver. It passes everything through to the
// WS2812 driver, except while the updater owns the LEDs: then rgblight's writes
// and flushes are dropped (its animations, layer colours, idle dimming and
// split sync carry on in RAM) and the updater writes the WS2812 driver
// directly. Nothing is persisted, and handing back is a repaint from
// rgblight's current state. The fallback the plan names,
// rgblight_disable_noeeprom(), was not needed: this gate builds and links.

#include "quantum.h"
#include "rgblight.h"
#include "rgblight_drivers.h"
#include "ws2812.h"
#include "color.h"
#include "update_led.h"

#ifndef RGBLIGHT_CUSTOM
#    error "updater: the LED gate needs RGBLIGHT_DRIVER = custom (keyboards/svalboard/rules.mk)"
#endif

#define CONFIRM_PERIOD_MS 500 // blue blink, 2 Hz, so it differs from the 1 Hz error blink
#define ERROR_PERIOD_MS 1000  // red, 1 Hz, 50/50 (D24)
#define FLASH_STEP_MS 75      // white double flash: on, off, on, off (~300 ms, D4)
#define RAINBOW_MS_PER_HUE 8  // a full hue turn in about 2 s
#define MIN_WRITE_MS 20       // at most 50 WS2812 writes a second

static bool owned;

static void gate_set_color(int index, uint8_t red, uint8_t green, uint8_t blue) {
    if (!owned) ws2812_set_color(index, red, green, blue);
}

static void gate_set_color_all(uint8_t red, uint8_t green, uint8_t blue) {
    if (!owned) ws2812_set_color_all(red, green, blue);
}

static void gate_flush(void) {
    if (!owned) ws2812_flush();
}

const rgblight_driver_t rgblight_driver = {
    .init          = ws2812_init,
    .set_color     = gate_set_color,
    .set_color_all = gate_set_color_all,
    .flush         = gate_flush,
};

static update_led_mode_t shown = UPDATE_LED_NONE;
static uint32_t          since, last_write;
static rgb_t             last;

static rgb_t colour(update_led_mode_t mode, uint32_t t) {
    const uint8_t v   = SVAL_UPDATE_LED_VAL;
    const rgb_t   off = {0, 0, 0};
    switch (mode) {
        case UPDATE_LED_CONFIRM:
            return t % CONFIRM_PERIOD_MS < CONFIRM_PERIOD_MS / 2 ? (rgb_t){0, 0, v} : off;
        case UPDATE_LED_CHORD:
            return (t / FLASH_STEP_MS) % 2 == 0 && t < 4 * FLASH_STEP_MS ? (rgb_t){v, v, v} : off;
        case UPDATE_LED_APPROVED:
            return (rgb_t){0, 0, v};
        case UPDATE_LED_PROGRESS:
            return hsv_to_rgb((hsv_t){(uint8_t)(t / RAINBOW_MS_PER_HUE), 255, v});
        case UPDATE_LED_WRITING:
            return (rgb_t){v, 0, v};
        case UPDATE_LED_ERROR:
            return t % ERROR_PERIOD_MS < ERROR_PERIOD_MS / 2 ? (rgb_t){v, 0, 0} : off;
        default:
            return off;
    }
}

// Back to rgblight: repaint from its current state (a static colour is pushed
// to the driver again; layers and the off state are written by rgblight_set).
static void release(void) {
    owned = false;
    if (rgblight_is_enabled()) rgblight_sethsv_noeeprom(rgblight_get_hue(), rgblight_get_sat(), rgblight_get_val());
    rgblight_set();
}

void update_led_show(update_led_mode_t mode) {
    uint32_t now   = timer_read32();
    bool     force = false;
    if (mode != shown) {
        shown = mode;
        since = now;
        force = true;
        if (mode == UPDATE_LED_NONE) {
            release();
            return;
        }
        owned = true;
    }
    if (mode == UPDATE_LED_NONE) return;
    if (!force && timer_elapsed32(last_write) < MIN_WRITE_MS) return;
    rgb_t c = colour(mode, now - since);
    if (force || c.r != last.r || c.g != last.g || c.b != last.b) {
        ws2812_set_color_all(c.r, c.g, c.b);
        ws2812_flush();
        last       = c;
        last_write = now;
    }
}
