#include "svalboard.h"
#if VIA_ENABLE
#include "via.h"
#endif
#include "version.h"
#include "layout_stamp.h"
#include "split_common/transactions.h"
#include <string.h>
#include QMK_KEYBOARD_H
#include "usb_main.h"
#include "suspend.h"
#include "scanlab.h"

// USB remote wakeup status bit (from USB spec, not exported by QMK headers)
#ifndef USB_GETSTATUS_REMOTE_WAKEUP_ENABLED
#define USB_GETSTATUS_REMOTE_WAKEUP_ENABLED (2U)
#endif

saved_values_t global_saved_values;
const int16_t mh_timer_choices[6] = { 200, 300, 400, 500, 800, -1 }; // -1 is infinite.

uint8_t sval_active_layer = 0;

static int8_t hw_rev_cache = -1;
uint8_t sval_hw_rev(void) {
    if (hw_rev_cache < 0) {
#if defined(SVAL_HW_REV_FORCE)
        hw_rev_cache = SVAL_HW_REV_FORCE;
#else
        setPinInputHigh(SVAL_HW_REV_PIN);
        wait_us(200);
        hw_rev_cache = readPin(SVAL_HW_REV_PIN) ? SVAL_HW_REV_A : SVAL_HW_REV_FLIPFET;
#endif
    }
    return (uint8_t)hw_rev_cache;
}

bool sval_hw_rev_is_flipfet(void) {
    return sval_hw_rev() == SVAL_HW_REV_FLIPFET;
}

// Store svalboard data in VIA custom config at offset 0
#define SVALBOARD_VIA_CONFIG_OFFSET 0
#define SVALBOARD_VIA_CONFIG_SIZE sizeof(saved_values_t)

// Bump only when a saved_values field is reinterpreted without the struct
// changing size (renamed, reordered or rescaled in place).
#ifndef SVALBOARD_SAVED_VALUES_SCHEMA
#    define SVALBOARD_SAVED_VALUES_SCHEMA 0
#endif

// Magic bytes for EEPROM validation: a layout stamp over the saved_values
// geometry, so pointer, layer-colour and scan settings reset only when they
// would otherwise be misread. It used to be the build timestamp to the second,
// which reset them on every firmware update.
//
// The first two bytes are fixed and are not valid BCD, so an old timestamp magic
// can never be mistaken for a stamp.
#define SVALBOARD_MAGIC_SIZE 6
#define SVALBOARD_MAGIC_OFFSET (SVALBOARD_VIA_CONFIG_OFFSET + SVALBOARD_VIA_CONFIG_SIZE)

#if VIA_ENABLE
static void svalboard_get_magic(uint8_t *magic) {
    const uint32_t values[] = {
        SVALBOARD_VIA_CONFIG_OFFSET,
        SVALBOARD_VIA_CONFIG_SIZE,
        DYNAMIC_KEYMAP_LAYER_COUNT,
        SVALBOARD_SAVED_VALUES_SCHEMA,
    };
    uint32_t stamp = layout_stamp(values, sizeof(values) / sizeof(values[0]));
    magic[0]       = 0xA5;
    magic[1]       = 0x5B;
    magic[2]       = (stamp >> 24) & 0xFF;
    magic[3]       = (stamp >> 16) & 0xFF;
    magic[4]       = (stamp >> 8) & 0xFF;
    magic[5]       = stamp & 0xFF;
}

static bool svalboard_eeprom_is_valid(void) {
    uint8_t stored[SVALBOARD_MAGIC_SIZE];
    uint8_t expected[SVALBOARD_MAGIC_SIZE];
    via_read_custom_config(stored, SVALBOARD_MAGIC_OFFSET, SVALBOARD_MAGIC_SIZE);
    svalboard_get_magic(expected);
    return memcmp(stored, expected, SVALBOARD_MAGIC_SIZE) == 0;
}

static void svalboard_eeprom_set_valid(void) {
    uint8_t magic[SVALBOARD_MAGIC_SIZE];
    svalboard_get_magic(magic);
    via_update_custom_config(magic, SVALBOARD_MAGIC_OFFSET, SVALBOARD_MAGIC_SIZE);
}

void write_eeprom_kb(void) {
    via_update_custom_config(&global_saved_values, SVALBOARD_VIA_CONFIG_OFFSET, SVALBOARD_VIA_CONFIG_SIZE);
}
#else
static bool svalboard_eeprom_is_valid(void) { return true; }
static void svalboard_eeprom_set_valid(void) {}
void write_eeprom_kb(void) {}
#endif

#define HSV(c) (struct layer_hsv) { (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF}

void read_eeprom_kb(void) {
    // Check if EEPROM data is valid (matches current firmware version)
    if (!svalboard_eeprom_is_valid()) {
        // Fresh EEPROM - apply defaults
        memset(&global_saved_values, 0, sizeof(global_saved_values));

        global_saved_values.right_dpi_index = 3;
        global_saved_values.left_dpi_index = 3;
        global_saved_values.mh_timer_index = 3;
        global_saved_values.left_scroll = true;
        global_saved_values.auto_mouse = true;
        global_saved_values.axis_scroll_lock = true;
        global_saved_values.turbo_scan = 0;
        global_saved_values.natural_scroll = false;
        global_saved_values.left_automouse = true;
        global_saved_values.right_automouse = true;
        global_saved_values.automouse_threshold = 50;
        global_saved_values.automouse_decay = 7;  // 70ms
        // Revision B settles more slowly; start it on explicit conservative timing.
        global_saved_values.scan_prewait_us = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_PREWAIT_US : 0;
        global_saved_values.scan_postwait_us = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_POSTWAIT_US : 0;
        global_saved_values.scan_period_us      = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US : 0;
        global_saved_values.scan_idle_period_ms = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS : 0;
        global_saved_values.scan_idle_after_ms  = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS : 0;
        global_saved_values.scan_deep_after_s   = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S : 0;
        global_saved_values.scan_deep_period_ms = sval_hw_rev_is_flipfet() ? SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS : 0;
        global_saved_values.idle_flags          = SVAL_IDLE_FLAGS_DEFAULT;
        global_saved_values.scan_deep_clock_idx = 0;

        // Layer colors
        global_saved_values.layer_colors[0] = HSV(0x55FFFF);  // Green
        global_saved_values.layer_colors[1] = HSV(0x15FFFF);  // Orange
        global_saved_values.layer_colors[2] = HSV(0x95FFFF);  // Azure
        global_saved_values.layer_colors[3] = HSV(0x0BB0FF);  // Coral
        global_saved_values.layer_colors[4] = HSV(0x2BFFFF);  // Yellow
        global_saved_values.layer_colors[5] = HSV(0x80FF80);  // Teal
        global_saved_values.layer_colors[6] = HSV(0x00FFFF);  // Red
        global_saved_values.layer_colors[7] = HSV(0x00FFFF);  // Red
        global_saved_values.layer_colors[8] = HSV(0xEAFFFF);  // Pink
        global_saved_values.layer_colors[9] = HSV(0xBFFF80);  // Purple
        global_saved_values.layer_colors[10] = HSV(0x0BB0FF); // Coral
        global_saved_values.layer_colors[11] = HSV(0x6AFFFF); // Spring Green
        global_saved_values.layer_colors[12] = HSV(0x80FF80); // Teal
        global_saved_values.layer_colors[13] = HSV(0x80FFFF); // Turquoise
        global_saved_values.layer_colors[14] = HSV(0x2BFFFF); // Yellow
        global_saved_values.layer_colors[15] = HSV(0xD5FFFF); // Magenta

        write_eeprom_kb();
        svalboard_eeprom_set_valid();
    } else {
#if VIA_ENABLE
        via_read_custom_config(&global_saved_values, SVALBOARD_VIA_CONFIG_OFFSET, SVALBOARD_VIA_CONFIG_SIZE);
#endif
    }
    sval_active_layer = 0;
}

static const char YES[] = "yes";
static const char NO[] = "no";

const char *yes_or_no(int flag) {
    if (flag) {
	return YES;
    } else {
	return NO;
    }
}

const uint16_t dpi_choices[] = { 200, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400, 12000 }; // If we need more, add them.
#define DPI_CHOICES_LENGTH (sizeof(dpi_choices)/sizeof(dpi_choices[0]))
extern bool is_mac;

// Boost state lives in keymaps/keymap_support.c; surfaced here for output_keyboard_info.
#include "axis_scale.h"
extern uint8_t boost_hold_2, boost_hold_3, boost_hold_5;
extern bool boost_toggle_2, boost_toggle_3, boost_toggle_5;
extern axis_scale_t boost_x;

void output_keyboard_info(void) {
    char output_buffer[256];

    sprintf(output_buffer, "%s:%s @ %s\n", QMK_KEYBOARD, QMK_KEYMAP, QMK_VERSION);
    send_string(output_buffer);
    sprintf(output_buffer, "Left Ptr: Scroll %s, cpi: %d, Right Ptr: Scroll %s, cpi: %d\n",
	    yes_or_no(global_saved_values.left_scroll), dpi_choices[global_saved_values.left_dpi_index],
	    yes_or_no(global_saved_values.right_scroll), dpi_choices[global_saved_values.right_dpi_index]);
    send_string(output_buffer);
    sprintf(output_buffer, "Axis Scroll Lock: %s (is Mac: %d), Natural Scroll: %s, Mouse Layer: %s (left: %s, right: %s), Mouse Layer Timeout: %d, Turbo Scan: %d\n",
	    yes_or_no(global_saved_values.axis_scroll_lock),
	    is_mac,
	    yes_or_no(global_saved_values.natural_scroll),
	    yes_or_no(global_saved_values.auto_mouse),
	    yes_or_no(global_saved_values.left_automouse),
	    yes_or_no(global_saved_values.right_automouse),
	    mh_timer_choices[global_saved_values.mh_timer_index],
	    global_saved_values.turbo_scan);
    send_string(output_buffer);
    sprintf(output_buffer, "BOOST: hold[2,3,5]=%d,%d,%d  tg[2,3,5]=%d,%d,%d  mult=%d\n",
            boost_hold_2, boost_hold_3, boost_hold_5,
            boost_toggle_2, boost_toggle_3, boost_toggle_5,
            boost_x.mult);
    send_string(output_buffer);
    uint16_t pre, post;
    sval_scan_timing(&pre, &post);
    sprintf(output_buffer, "HW rev: %s, Scan timing: pre %d us, post %d us (%s)\n",
            sval_hw_rev_is_flipfet() ? "B (flipfet)" : "A",
            pre, post,
            (global_saved_values.scan_prewait_us || global_saved_values.scan_postwait_us) ? "explicit" : "turbo table");
    send_string(output_buffer);
    uint32_t frame_us;
    uint16_t led_us;
    uint8_t  stage;
    sval_scan_stats(&frame_us, &led_us, &stage);
    sprintf(output_buffer, "Scan period: %d us; light idle %d ms after %d ms; deep idle %d ms after %d s; measured frame %lu us, LED on %d us, stage %d\n",
            global_saved_values.scan_period_us, global_saved_values.scan_idle_period_ms, global_saved_values.scan_idle_after_ms,
            global_saved_values.scan_deep_period_ms, global_saved_values.scan_deep_after_s,
            (unsigned long)frame_us, led_us, stage);
    send_string(output_buffer);
    sprintf(output_buffer, "Idle power: pointer rest %s, RGB dim %s, CPU sleep %s, low clock in deep idle %s (%d MHz), long naps %s; clock now %d MHz\n",
            (global_saved_values.idle_flags & SVAL_IDLE_POINTER_REST) ? "on" : "off",
            (global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM) ? "on" : "off",
            (global_saved_values.idle_flags & SVAL_IDLE_CPU_SLEEP) ? "on" : "off",
            (global_saved_values.idle_flags & SVAL_IDLE_LOW_CLOCK) ? "on" : "off", sval_deep_clock_mhz(),
            (global_saved_values.idle_flags & SVAL_IDLE_LONG_NAP) ? "on" : "off",
            sval_clock_mhz());
    send_string(output_buffer);
}

const uint16_t sval_postwait_us[] = {90, 60, 45, 30, 25, 20, 15};
const uint16_t sval_prewait_us[] = {90, 60, 45, 30, 25, 20, 15};
#define TURBO_CHOICES_LENGTH (sizeof(sval_postwait_us)/sizeof(sval_postwait_us[0]))
void change_turbo_scan(void) {
    if (global_saved_values.turbo_scan + 1 < TURBO_CHOICES_LENGTH) {
        global_saved_values.turbo_scan++;
    } else {
        global_saved_values.turbo_scan = 0;
    }
    write_eeprom_kb();
}

void increase_left_dpi(void) {
    if (global_saved_values.left_dpi_index + 1 < DPI_CHOICES_LENGTH) {
        global_saved_values.left_dpi_index++;
        set_left_dpi(global_saved_values.left_dpi_index);
        write_eeprom_kb();
    }
}

void decrease_left_dpi(void) {
    if (global_saved_values.left_dpi_index > 0) {
        global_saved_values.left_dpi_index--;
        set_left_dpi(global_saved_values.left_dpi_index);
        write_eeprom_kb();
    }
}

void increase_right_dpi(void) {
    if (global_saved_values.right_dpi_index + 1 < DPI_CHOICES_LENGTH) {
        global_saved_values.right_dpi_index++;
        set_right_dpi(global_saved_values.right_dpi_index);
        write_eeprom_kb();
    }
}

void decrease_right_dpi(void) {
    if (global_saved_values.right_dpi_index > 0) {
        global_saved_values.right_dpi_index--;
        set_right_dpi(global_saved_values.right_dpi_index);
        write_eeprom_kb();
    }
}

int16_t get_left_dpi() {
    return dpi_choices[global_saved_values.left_dpi_index];
}

int16_t get_right_dpi() {
    return dpi_choices[global_saved_values.right_dpi_index];
}

void set_left_dpi(uint8_t index) {
    uprintf("LDPI: %d %d\n", index, dpi_choices[index]);
    pointing_device_set_cpi_on_side(true, dpi_choices[index]);
}

void set_right_dpi(uint8_t index) {
    uprintf("RDPI: %d %d\n", index, dpi_choices[index]);
    pointing_device_set_cpi_on_side(false, dpi_choices[index]);
}

void set_dpi_from_eeprom(void) {
    set_left_dpi(global_saved_values.left_dpi_index);
    set_right_dpi(global_saved_values.right_dpi_index);
}

// ---- Host activity. Deep idle paces the whole main loop off the nap length, so a config
// app talking over raw HID would get one request/response round trip per nap (20 ms on the
// master). A bootstrap is hundreds of round trips, which is long enough that the host gives
// up before the board has said anything wrong. Every host packet stamps the clock here; while
// the stamp is fresh the pacing gate keeps full speed and 1 ms naps. The scan period itself
// is left alone, so the sensor LEDs stay in deep idle and the power saving is kept.
static uint32_t host_last_ms  = 0;
static bool     host_seen_any = false;

void sval_host_packet_kb(void) {
    host_last_ms  = timer_read32();
    host_seen_any = true;
}

uint32_t sval_host_idle_ms(void) {
    return host_seen_any ? timer_elapsed32(host_last_ms) : UINT32_MAX;
}

bool sval_host_recent(void) {
    return host_seen_any && timer_elapsed32(host_last_ms) < SVAL_HOST_ACTIVE_MS;
}

// ---- RGB idle dimming (master). Light idle divides the brightness, deep idle turns the
// strip off; the first input restores it. Layer colour changes while dimmed keep using the
// awake brightness so the dimmed value never reaches EEPROM.
static uint8_t  rgb_idle_applied = 0;   // 0 awake, 1 dimmed, 2 off
static uint32_t rgb_last_write_ms = 0;

bool sval_rgb_idle_quiesced(void) {
    if (!is_keyboard_master()) return true;
    if (!(global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM)) return true;
    return rgb_idle_applied == 2 && timer_elapsed32(rgb_last_write_ms) > 5;
}
static uint8_t rgb_awake_val    = 0;
static uint8_t rgb_dim_val      = 0;
static bool    rgb_awake_enabled = false;

uint8_t sval_rgb_awake_val(void) {
    return rgb_idle_applied ? rgb_awake_val : rgblight_get_val();
}

static void sval_rgb_idle_restore(void) {
    if (!rgb_idle_applied) return;
    sval_clock_full(); // WS2812 timing needs the clock the driver was set up for
    if (rgb_awake_enabled && !rgblight_is_enabled()) rgblight_enable_noeeprom();
    // Only undo our own change; if someone adjusted the brightness while dimmed, keep theirs.
    if (rgb_idle_applied == 2 || rgblight_get_val() == rgb_dim_val) {
        rgblight_sethsv_noeeprom(rgblight_get_hue(), rgblight_get_sat(), rgb_awake_val);
    }
    rgb_idle_applied = 0;
}

void sval_rgb_idle_task(void) {
    if (!(global_saved_values.idle_flags & SVAL_IDLE_RGB_DIM)) {
        sval_rgb_idle_restore();
        return;
    }
    uint32_t quiet = last_input_activity_elapsed();
    uint8_t  want  = 0;
    if (global_saved_values.scan_deep_after_s && quiet > (uint32_t)global_saved_values.scan_deep_after_s * 1000u) {
        want = 2;
    } else if (global_saved_values.scan_idle_after_ms && quiet > global_saved_values.scan_idle_after_ms) {
        want = 1;
    }
    if (want == rgb_idle_applied) return;
    if (want == 0) {
        sval_rgb_idle_restore();
        return;
    }
    if (rgb_idle_applied == 0) {
        rgb_awake_val     = rgblight_get_val();
        rgb_awake_enabled = rgblight_is_enabled();
    }
    if (want == 1) {
        rgb_dim_val = rgb_awake_val / SVAL_IDLE_RGB_LIGHT_DIV;
        if (rgb_awake_val && rgb_dim_val < 2) rgb_dim_val = 2;
        rgblight_sethsv_noeeprom(rgblight_get_hue(), rgblight_get_sat(), rgb_dim_val);
    } else {
        rgblight_disable_noeeprom();
    }
    rgb_last_write_ms = timer_read32();
    rgb_idle_applied  = want;
}

__attribute__((weak)) void sval_pointer_rest_apply(void) {}
__attribute__((weak)) void sval_pointer_status(sval_idle_status_t *st) { st->sensor_present = 0; }

void sval_idle_status(sval_idle_status_t *st) {
    memset(st, 0, sizeof(*st));
    sval_pointer_status(st);
    st->rgb_val_now   = rgblight_get_val();
    st->rgb_val_awake = sval_rgb_awake_val();
    st->rgb_stage     = rgb_idle_applied;
    st->rgb_enabled   = rgblight_is_enabled();
}

void sval_set_active_layer(uint32_t layer, bool save) {
    if (layer > 15) layer = 15;
    sval_active_layer = layer;
    struct layer_hsv cols  = global_saved_values.layer_colors[layer];
    uint8_t          awake = sval_rgb_awake_val();
    uint8_t          shown = rgb_idle_applied == 1 ? rgb_dim_val : awake;
    if (save) {
        rgblight_sethsv(cols.hue, cols.sat, awake); // store with the awake brightness
        if (shown != awake) rgblight_sethsv_noeeprom(cols.hue, cols.sat, shown);
    } else {
        rgblight_sethsv_noeeprom(cols.hue, cols.sat, shown);
    }
}

// RPC listener for split keyboard sync
void kb_sync_listener(uint8_t in_buflen, const void* in_data, uint8_t out_buflen, void* out_data) {
    const presence_rpc_t *in = (const presence_rpc_t *)in_data;
    global_saved_values.turbo_scan       = in->turbo_scan;
    global_saved_values.scan_prewait_us  = in->scan_prewait_us;
    global_saved_values.scan_postwait_us = in->scan_postwait_us;
    global_saved_values.scan_period_us      = in->scan_period_us;
    global_saved_values.scan_idle_period_ms = in->scan_idle_period_ms;
    global_saved_values.scan_idle_after_ms  = in->scan_idle_after_ms;
    global_saved_values.scan_deep_after_s   = in->scan_deep_after_s;
    global_saved_values.scan_deep_period_ms = in->scan_deep_period_ms;
    if (global_saved_values.idle_flags != in->idle_flags) {
        global_saved_values.idle_flags = in->idle_flags;
        sval_pointer_rest_apply();
    }
    global_saved_values.scan_deep_clock_idx = in->scan_deep_clock_idx;
}

// Scan Lab requests from the master run here on the other half.
static void scanlab_rpc_listener(uint8_t in_buflen, const void* in_data, uint8_t out_buflen, void* out_data) {
    uint8_t req[SCANLAB_REQ_LEN] = {0};
    uint8_t rsp[SCANLAB_RSP_LEN] = {0};
    memcpy(req, in_data, in_buflen < SCANLAB_REQ_LEN ? in_buflen : SCANLAB_REQ_LEN);
    scanlab_handle(req, rsp);
    memcpy(out_data, rsp, out_buflen < SCANLAB_RSP_LEN ? out_buflen : SCANLAB_RSP_LEN);
}

void keyboard_post_init_kb(void) {
    read_eeprom_kb();
    sval_pointer_rest_apply(); // the sensor was initialised before the flags were read
    sval_sleep_gating_init();
    set_dpi_from_eeprom();
    keyboard_post_init_user();
    scanlab_init();
    transaction_register_rpc(KEYBOARD_SYNC_A, kb_sync_listener);
    transaction_register_rpc(KEYBOARD_SYNC_B, scanlab_rpc_listener);
    if (is_keyboard_master()) {
        sval_set_active_layer(sval_active_layer, false);
    }
}

static bool is_connected = false;
bool sval_other_half_connected(void) {
    return is_connected;
}

// Custom USB wake handler for split keyboards with NO_USB_STARTUP_CHECK
// This replaces the wake functionality that NO_USB_STARTUP_CHECK disables
static void sval_usb_wake_handler(void) {
    // Only master half handles USB wake
    if (!is_keyboard_master()) return;

    // Check if USB is in suspended state
    if (USB_DRIVER.state == USB_SUSPENDED) {
        // Check if host has enabled remote wakeup capability
        if (USB_DRIVER.status & USB_GETSTATUS_REMOTE_WAKEUP_ENABLED) {
            // Check for wake condition (key pressed)
            if (suspend_wakeup_condition()) {
                usbWakeupHost(&USB_DRIVER);
#if USB_SUSPEND_WAKEUP_DELAY > 0
                wait_ms(USB_SUSPEND_WAKEUP_DELAY);
#endif
            }
        }
    }
}

void housekeeping_task_kb(void) {
    sval_usb_wake_handler();
    scanlab_housekeeping();

    if (is_keyboard_master()) {
        sval_rgb_idle_task();
        static uint32_t last_ping = 0;
        if (timer_elapsed(last_ping) > 500) {
            presence_rpc_t rpcout = {global_saved_values.turbo_scan, global_saved_values.scan_prewait_us, global_saved_values.scan_postwait_us,
                                     global_saved_values.scan_period_us, global_saved_values.scan_idle_period_ms, global_saved_values.scan_idle_after_ms,
                                     global_saved_values.scan_deep_after_s, global_saved_values.scan_deep_period_ms, global_saved_values.idle_flags,
                                     global_saved_values.scan_deep_clock_idx};
            presence_rpc_t rpcin = {0};
            if (transaction_rpc_exec(KEYBOARD_SYNC_A, sizeof(presence_rpc_t), &rpcout, sizeof(presence_rpc_t), &rpcin)) {
                if (!is_connected) {
                    is_connected = true;
                    sval_on_reconnect();
                }
            } else {
                is_connected = false;
            }
            last_ping = timer_read32();
        }
    }
}

void sval_on_reconnect(void) {
    // Reset colors, or it won't communicate the right color.
    rgblight_sethsv_noeeprom(0, 0, rgblight_get_val()); //reuse existing val, so brightness doesn't reset
    sval_set_active_layer(sval_active_layer, true);
}

#ifndef SVALBOARD_REENABLE_BOOTMAGIC_LITE
// This is to override `bootmagic_lite` feature (see docs/feature_bootmagic.md),
// which can't be turned off in the usual way (via info.json) because setting
// `VIA_ENABLE` forces `BOOTMAGIC_ENABLE` in `builddefs/common_features.mk`.
//
// Obviously if you find this feature useful, you might want to define the
// SVALBOARD_... gating macro, and then possibly also (re-)define the
// `"bootmagic": { "matrix": [X,Y] },` in `info.json` to point the matrix at
// a more useful key than the [0,0] default. Ideally a center key, which is
// normally ~always present. Because the default (thumb knuckle) means that
// if you boot with the key pulled out, the eeprom gets cleared.

void bootmagic_lite(void) {
  // boo!
}
#endif

__attribute__((weak)) void recalibrate_pointer(void) {
}


const char chordal_hold_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM =
    LAYOUT(
            'R', 'R', 'R', 'R', 'R', 'R',
            'R', 'R', 'R', 'R', 'R', 'R',
            'R', 'R', 'R', 'R', 'R', 'R',
            'R', 'R', 'R', 'R', 'R', 'R',
            'L', 'L', 'L', 'L', 'L', 'L',
            'L', 'L', 'L', 'L', 'L', 'L',
            'L', 'L', 'L', 'L', 'L', 'L',
            'L', 'L', 'L', 'L', 'L', 'L',
            '*', '*', '*', '*', '*', '*',
            '*', '*', '*', '*', '*', '*'
          );

#if VIA_ENABLE
// VIA custom keyboard value IDs (channel 1)
enum sval_via_value_id {
    id_left_dpi = 0,
    id_left_scroll = 1,
    id_right_dpi = 2,
    id_right_scroll = 3,
    id_automouse_enable = 4,
    id_automouse_timeout = 5,
    id_automouse_threshold = 6,
    id_natural_scroll = 7,
    id_axis_lock = 8,
    id_turbo_scan = 9,
    id_automouse_decay = 10,  // Accumulator decay time in 10ms units
    id_left_automouse = 11,   // Left pointer movement activates the mouse layer
    id_right_automouse = 12,  // Right pointer movement activates the mouse layer
    id_scan_prewait_us = 13,  // u16: row-on settle time (0 = turbo table)
    id_scan_postwait_us = 14, // u16: row-off recovery time (0 = turbo table)
    id_hw_revision = 15,      // read-only: 0 = revision A, 1 = revision B (flipfet)
    // 16-19: tapping settings
    id_scan_period_us = 20,       // u16: frame period while active (0 = unpaced)
    id_scan_idle_period_ms = 21,  // u16: light idle frame period, ms (<= active = no change)
    id_scan_idle_after_ms = 22,   // u16: light idle timeout since last matrix change, ms (0 = never)
    id_scan_deep_after_s = 23,    // u16: deep idle timeout, seconds (0 = never)
    id_scan_deep_period_ms = 24,  // u16: deep idle frame period, ms
    id_idle_pointer_rest = 25,    // toggle: trackball sensor rest modes
    id_idle_rgb_dim = 26,         // toggle: dim RGB in light idle, off in deep idle
    id_idle_cpu_sleep = 27,       // toggle: sleep the core between paced frames
    id_idle_low_clock = 28,       // toggle: 48 MHz system clock in deep idle
    id_idle_long_nap = 29,        // toggle: long naps in deep idle
    id_scan_deep_clock_idx = 30,  // u8 dropdown: deep-idle clock 0 = 48 MHz, 1 = 24 MHz, 2 = 12 MHz
    id_tapping_term = 16,
    id_permissive_hold = 17,
    id_hold_on_other_key = 18,
    id_retro_tapping = 19,
    // 25-31 reserved
    id_layer0_color = 32,
    // 32-47 are layer colors (id_layer0_color + layer)
};

void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    // New VIA API: data[0]=command, data[1]=channel, data[2]=value_id, data[3+]=value_data
    if (data[1] == SCANLAB_CHANNEL) {
        scanlab_via_command(data, length);
        return;
    }
    uint8_t command = data[0];
    uint8_t *value_id = &data[2];
    uint8_t *value_data = &data[3];

    switch (command) {
        case id_custom_set_value:
            switch (*value_id) {
                case id_left_dpi:
                    if (value_data[0] < DPI_CHOICES_LENGTH) {
                        global_saved_values.left_dpi_index = value_data[0];
                        set_left_dpi(value_data[0]);
                    }
                    break;
                case id_left_scroll:
                    global_saved_values.left_scroll = value_data[0];
                    break;
                case id_right_dpi:
                    if (value_data[0] < DPI_CHOICES_LENGTH) {
                        global_saved_values.right_dpi_index = value_data[0];
                        set_right_dpi(value_data[0]);
                    }
                    break;
                case id_right_scroll:
                    global_saved_values.right_scroll = value_data[0];
                    break;
                case id_automouse_enable:
                    global_saved_values.auto_mouse = value_data[0];
                    break;
                case id_automouse_timeout:
                    if (value_data[0] < 6) {
                        global_saved_values.mh_timer_index = value_data[0];
                    }
                    break;
                case id_natural_scroll:
                    global_saved_values.natural_scroll = value_data[0];
                    break;
                case id_axis_lock:
                    global_saved_values.axis_scroll_lock = value_data[0];
                    break;
                case id_turbo_scan:
                    if (value_data[0] < TURBO_CHOICES_LENGTH) {
                        global_saved_values.turbo_scan = value_data[0];
                    }
                    break;
                case id_automouse_threshold:
                    global_saved_values.automouse_threshold = value_data[0] | (value_data[1] << 8);
                    break;
                case id_automouse_decay:
                    global_saved_values.automouse_decay = value_data[0];
                    break;
                case id_left_automouse:
                    global_saved_values.left_automouse = value_data[0];
                    break;
                case id_right_automouse:
                    global_saved_values.right_automouse = value_data[0];
                    break;
                case id_scan_prewait_us:
                    global_saved_values.scan_prewait_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_postwait_us:
                    global_saved_values.scan_postwait_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_period_us:
                    global_saved_values.scan_period_us = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_idle_period_ms:
                    global_saved_values.scan_idle_period_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_idle_after_ms:
                    global_saved_values.scan_idle_after_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_after_s:
                    global_saved_values.scan_deep_after_s = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_period_ms:
                    global_saved_values.scan_deep_period_ms = value_data[0] | (value_data[1] << 8);
                    break;
                case id_scan_deep_clock_idx:
                    global_saved_values.scan_deep_clock_idx = value_data[0] > 2 ? 2 : value_data[0];
                    break;
                case id_idle_pointer_rest:
                case id_idle_rgb_dim:
                case id_idle_cpu_sleep:
                case id_idle_low_clock:
                case id_idle_long_nap: {
                    uint8_t bit = 1u << (*value_id - id_idle_pointer_rest);
                    if (value_data[0]) global_saved_values.idle_flags |= bit; else global_saved_values.idle_flags &= ~bit;
                    if (bit == SVAL_IDLE_POINTER_REST) sval_pointer_rest_apply();
                    break;
                }
                default:
                    // Layer colors: id 32-47
                    if (*value_id >= id_layer0_color && *value_id < id_layer0_color + 16) {
                        uint8_t layer = *value_id - id_layer0_color;
                        // VIA color is H, S (2 bytes)
                        global_saved_values.layer_colors[layer].hue = value_data[0];
                        global_saved_values.layer_colors[layer].sat = value_data[1];
                        // Update display if this is the active layer
                        if (layer == sval_active_layer) {
                            sval_set_active_layer(layer, false);
                        }
                    }
                    break;
            }
            break;

        case id_custom_get_value:
            switch (*value_id) {
                case id_left_dpi:
                    value_data[0] = global_saved_values.left_dpi_index;
                    break;
                case id_left_scroll:
                    value_data[0] = global_saved_values.left_scroll;
                    break;
                case id_right_dpi:
                    value_data[0] = global_saved_values.right_dpi_index;
                    break;
                case id_right_scroll:
                    value_data[0] = global_saved_values.right_scroll;
                    break;
                case id_automouse_enable:
                    value_data[0] = global_saved_values.auto_mouse;
                    break;
                case id_automouse_timeout:
                    value_data[0] = global_saved_values.mh_timer_index;
                    break;
                case id_natural_scroll:
                    value_data[0] = global_saved_values.natural_scroll;
                    break;
                case id_axis_lock:
                    value_data[0] = global_saved_values.axis_scroll_lock;
                    break;
                case id_turbo_scan:
                    value_data[0] = global_saved_values.turbo_scan;
                    break;
                case id_automouse_threshold:
                    value_data[0] = global_saved_values.automouse_threshold & 0xFF;
                    value_data[1] = (global_saved_values.automouse_threshold >> 8) & 0xFF;
                    break;
                case id_automouse_decay:
                    value_data[0] = global_saved_values.automouse_decay;
                    break;
                case id_left_automouse:
                    value_data[0] = global_saved_values.left_automouse;
                    break;
                case id_right_automouse:
                    value_data[0] = global_saved_values.right_automouse;
                    break;
                case id_scan_prewait_us:
                    value_data[0] = global_saved_values.scan_prewait_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_prewait_us >> 8) & 0xFF;
                    break;
                case id_scan_postwait_us:
                    value_data[0] = global_saved_values.scan_postwait_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_postwait_us >> 8) & 0xFF;
                    break;
                case id_hw_revision:
                    value_data[0] = sval_hw_rev();
                    break;
                case id_scan_period_us:
                    value_data[0] = global_saved_values.scan_period_us & 0xFF;
                    value_data[1] = (global_saved_values.scan_period_us >> 8) & 0xFF;
                    break;
                case id_scan_idle_period_ms:
                    value_data[0] = global_saved_values.scan_idle_period_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_idle_period_ms >> 8) & 0xFF;
                    break;
                case id_scan_idle_after_ms:
                    value_data[0] = global_saved_values.scan_idle_after_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_idle_after_ms >> 8) & 0xFF;
                    break;
                case id_scan_deep_after_s:
                    value_data[0] = global_saved_values.scan_deep_after_s & 0xFF;
                    value_data[1] = (global_saved_values.scan_deep_after_s >> 8) & 0xFF;
                    break;
                case id_scan_deep_period_ms:
                    value_data[0] = global_saved_values.scan_deep_period_ms & 0xFF;
                    value_data[1] = (global_saved_values.scan_deep_period_ms >> 8) & 0xFF;
                    break;
                case id_scan_deep_clock_idx:
                    value_data[0] = global_saved_values.scan_deep_clock_idx;
                    break;
                case id_idle_pointer_rest:
                case id_idle_rgb_dim:
                case id_idle_cpu_sleep:
                case id_idle_low_clock:
                case id_idle_long_nap:
                    value_data[0] = (global_saved_values.idle_flags >> (*value_id - id_idle_pointer_rest)) & 1;
                    break;
                default:
                    // Layer colors: id 32-47
                    if (*value_id >= id_layer0_color && *value_id < id_layer0_color + 16) {
                        uint8_t layer = *value_id - id_layer0_color;
                        value_data[0] = global_saved_values.layer_colors[layer].hue;
                        value_data[1] = global_saved_values.layer_colors[layer].sat;
                    }
                    break;
            }
            break;

        case id_custom_save:
            write_eeprom_kb();
            break;
    }
}
#endif // VIA_ENABLE
