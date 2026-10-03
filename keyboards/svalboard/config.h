/*
Copyright 2023 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
// This is used by our user's keymaps for conditionals.
#define SVALBOARD

// Keyboard UID for profile matching (same as original Vial UID for .vil compatibility)
#define SVAL_KEYBOARD_UID {0x1B, 0x18, 0x7D, 0xF2, 0x21, 0xF6, 0x29, 0x48}

#define POINTING_DEVICE_HIRES_SCROLL_ENABLE 1
#define POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER 120
#define WHEEL_EXTENDED_REPORT 1

#define AXIS_TYPE int16_t

//#define FORTY_FOUR_MM_TB
/* key matrix size */
// Rows are doubled-up
#define MATRIX_ROWS  10
#define MATRIX_COLS  6
#define PFET_ROWS
#define EE_HANDS
//#define DEBUG_MATRIX_SCAN_RATE
// Svalboard stores its config in VIA custom config area
// saved_values_t (~54 bytes) + 3 bytes magic = ~60 bytes, round to 100
#define VIA_EEPROM_CUSTOM_CONFIG_SIZE 100


#define FLASH_LEN (16 * 1024 * 1024)
#define WEAR_LEVELING_BACKING_SIZE (128 * 1024)

// Identity (identity.c): the USB serial is "sval:" + the board's stored 8-byte
// serial as 16 hex digits, and the product string is the user's name when set.
// Every keymap uses it, so drop the module's fixed SERIAL_NUMBER literal.
#undef SERIAL_NUMBER
#define SERIAL_NUMBER_LENGTH (5 + 16) // "sval:" + 16 hex digits
#define USB_PRODUCT_STRING_RUNTIME
// wiring of each half
//Layout for svalboard v0 (different from lalboard_v2)
//1 2 3 4 5 6
//S E D N W None
//Both Thumbs (these are same as lalboard_v2)
//OL OU D IL MODE DOUBLE
//Knuckle Nail Down Pad Up Double
//#define THUMB_DOWN_ACTIVE_DARK
#define MATRIX_COL_PUSHED_STATES { 0, 0, 1, 0, 0, 0 }
#ifdef THUMB_DOWN_ACTIVE_DARK
    #define MATRIX_COL_PUSHED_STATES_THUMBS { 0, 0, 1, 0, 0, 0 }
#else
    #define MATRIX_COL_PUSHED_STATES_THUMBS { 0, 0, 0, 0, 0, 0 }
#endif
#define DOUBLEDOWN_COL 5 // need a pullup on COL6

// Hardware revision strap. The pin is read with the internal pull-up enabled:
// open = revision A, bridged to ground = revision B ("flipfet"). Override the
// pin per build if the MCU board routes a different spare GPIO, or force a
// revision with -DSVAL_HW_REV_FORCE=1 on prototypes without the strap.
#ifndef SVAL_HW_REV_PIN
    #define SVAL_HW_REV_PIN GP22
#endif
// Revision B boots on explicit scan timing. Measured with the Scan Lab on a
// revision B board (see docs/scan-lab.md): centre keys settle in 21-27 us,
// side keys in 11-15 us, every line recovers within 1 us. 45 / 5 us leaves
// 1.7x margin on settle and 5x on recovery.
#ifndef SVAL_FLIPFET_DEFAULT_PREWAIT_US
    #define SVAL_FLIPFET_DEFAULT_PREWAIT_US 45
#endif
#ifndef SVAL_FLIPFET_DEFAULT_POSTWAIT_US
    #define SVAL_FLIPFET_DEFAULT_POSTWAIT_US 5
#endif
// Frame pacing: sensor LED duty = rows x (pre-wait + read) / period. 1 ms
// matches the USB poll interval; the idle period defaults to the same value
// so there is no latency trade-off unless a user chooses one.
#ifndef SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US
    #define SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US 1000
#endif
#ifndef SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS
    #define SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS 1     // light idle: same as active by default
#endif
#ifndef SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS
    #define SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS 10000   // light idle also dims the RGB, so not during typing pauses
#endif
#ifndef SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S
    #define SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S 0       // deep idle off by default
#endif
#ifndef SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS
    #define SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS 0
#endif

// Idle power features (saved_values.idle_flags). Each is a runtime toggle (VIA ids 25-27) so
// their effect can be measured one at a time from the Scan Lab panel.
#define SVAL_IDLE_POINTER_REST 0x01   // let the trackball sensor use its own rest modes
#define SVAL_IDLE_RGB_DIM      0x02   // dim the RGB in light idle, off in deep idle
#define SVAL_IDLE_CPU_SLEEP    0x04   // sleep the core between paced frames instead of spinning
#define SVAL_IDLE_LOW_CLOCK    0x08   // deep idle: clk_sys from the USB PLL at 48 MHz, system PLL off
#define SVAL_IDLE_LONG_NAP     0x10   // deep idle: naps of SVAL_DEEP_NAP_*_US instead of 1 ms
#ifndef SVAL_IDLE_FLAGS_DEFAULT
    #define SVAL_IDLE_FLAGS_DEFAULT (SVAL_IDLE_POINTER_REST | SVAL_IDLE_RGB_DIM | SVAL_IDLE_CPU_SLEEP | SVAL_IDLE_LOW_CLOCK | SVAL_IDLE_LONG_NAP)
#endif
#ifndef SVAL_DEEP_NAP_MASTER_US
    #define SVAL_DEEP_NAP_MASTER_US 20000 // the sensor only reports every 100-500 ms in rest; USB replies wait at most this
#endif
#ifndef SVAL_DEEP_NAP_SLAVE_US
    #define SVAL_DEEP_NAP_SLAVE_US 4000   // must stay well inside the master's 20 ms split-transaction timeout
#endif
#ifndef SVAL_HOST_ACTIVE_MS
    #define SVAL_HOST_ACTIVE_MS 1500  // a host app is mid-conversation: full clock and 1 ms naps, so a
                                      // request/response round trip is not paced by the deep-idle nap
#endif
#ifndef SVAL_IDLE_RGB_LIGHT_DIV
    #define SVAL_IDLE_RGB_LIGHT_DIV 4   // light idle brightness = awake brightness / this
#endif
#ifndef SVAL_SLEEP_MAX_US
    #define SVAL_SLEEP_MAX_US 1000      // longest single nap between frames; keeps USB and pointer polling responsive
#endif
#ifndef SVAL_SLEEP_MIN_US
    #define SVAL_SLEEP_MIN_US 60        // below this the timer round trip costs more than it saves
#endif
// Host-initiated reboot into the bootloader (Scan Lab REBOOT_ARM / REBOOT_GO).
// Off unless a keymap or build enables it; the scanlab keymap does.
#ifndef SVAL_HOST_BOOTLOADER
    #define SVAL_HOST_BOOTLOADER 0
#endif
// Thumb-row pushed states on revision B. Identical to revision A until the
// settle probe says otherwise; the probe reports each key's polarity.
#ifndef MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET
    #define MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET MATRIX_COL_PUSHED_STATES_THUMBS
#endif

#define SERIAL_DEBUG
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET_TIMEOUT 500 // Timeout window in ms in which the double tap can occur.

// Macro count (Sval entry counts are now in each keymap's sval.json)
// 256 macros: QMK's macro keycodes (0x7700-0x777F) reach macro 127; the Sval
// module maps 0x7680-0x76FF to macros 128-255 (SVAL_MACRO_HIGH_BASE).
#define DYNAMIC_KEYMAP_MACRO_COUNT 256

// Sval defaults for Svalboard - sane mod-tap experience
#define SVAL_DEFAULT_NKRO 1
#define SVAL_DEFAULT_PERMISSIVE_HOLD 1
#define SVAL_DEFAULT_CHORDAL_HOLD 1

#define USB_MAX_POWER_CONSUMPTION 500
#define USB_SUSPEND_WAKEUP_DELAY 500
#define SERIAL_USART_SPEED 1000000

#define MOUSE_EXTENDED_REPORT
#define SPLIT_POINTING_ENABLE
#define POINTING_DEVICE_COMBINED
#define POINTING_DEVICE_AUTO_MOUSE_MH_ENABLE
#define POINTING_DEVICE_TASK_THROTTLE_MS 1


// Avoid slave-slave deadlock due to missing USB_VBUS_PIN.
//
// End result of enabling this: when you plug the keyboard to a finnicky USB
// hub, KVM, or a machine that boots slowly (ECC RAM), the keyboard no longer
// needs to be reset to come to life.
#define SPLIT_WATCHDOG_ENABLE

// WS2812-fu here:
//  pretty lights
//  https://docs.qmk.fm/#/feature_rgblight?id=configuration
#define WS2812_DI_PIN GP19
#define RGBLED_SPLIT { 1, 1 }
#define RGBLIGHT_LAYERS_RETAIN_VAL
#define RGBLIGHT_LAYERS DYNAMIC_KEYMAP_LAYER_COUNT
#define RGBLIGHT_DEFAULT_SAT 0 // white?
#define RGBLIGHT_LIMIT_VAL 255
#define RGBLIGHT_DEFAULT_VAL 128
#define RGBLIGHT_SLEEP // don't annoy when host asleep
#define RGBLIGHT_MAX_LAYERS 16 //DYNAMIC_KEYMAP_LAYER_COUNT
#define RGBLIGHT_VAL_STEP 10
#define RGBLIGHT_LED_COUNT 2

#define SPLIT_TRANSACTION_IDS_KB KEYBOARD_SYNC_A, KEYBOARD_SYNC_B

#define PERMISSIVE_HOLD

#define MOUSEKEY_WHEEL_DELTA 120

#define OS_DETECTION_KEYBOARD_RESET

#define PMW33XX_LIFTOFF_DISTANCE 0x00
