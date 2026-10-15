// SPDX-License-Identifier: GPL-2.0-or-later
// The pointing tools' USB seam, kept out of QMK core.
//
// The output broker (point_output.c) has to see every keyboard, NKRO and extra
// report QMK sends, compose its own contribution on top, and learn whether the
// endpoint accepted each report, so that taps are paced by the host and never
// queued for catch-up. Instead of patching tmk_core/protocol/chibios/usb_main.c,
// this wraps the host driver the way keytest.c does: the wrapped driver hands
// those three reports to the broker, and the broker sends on the same endpoints
// with the same 100 ms timeout as usb_main.c's send_report(), which reports
// acceptance. Mouse, LEDs and raw HID keep going straight to the original driver.
#include <ch.h>
#include <hal.h>
#include "point_output.h"
#include "action_util.h"
#include "host.h"
#include "host_driver.h"
#include "usb_device_state.h"
#include "usb_driver.h"
#include "usb_endpoints.h"

extern usb_endpoint_in_t usb_endpoints_in[USB_ENDPOINT_IN_COUNT];

static bool send(usb_endpoint_in_lut_t endpoint, void *report, size_t size) {
    return usb_endpoint_in_send(&usb_endpoints_in[endpoint], (uint8_t *)report, size, TIME_MS2I(100), false);
}

bool sval_point_transport_keyboard(report_keyboard_t *report) {
    // Boot protocol: no report ID or other fields, as usb_main.c's send_keyboard().
    if (usb_device_state_get_protocol() == USB_PROTOCOL_BOOT) {
        return send(USB_ENDPOINT_IN_KEYBOARD, &report->mods, 8);
    }
    return send(USB_ENDPOINT_IN_KEYBOARD, report, KEYBOARD_REPORT_SIZE);
}

bool sval_point_transport_nkro(report_nkro_t *report) {
    return send(USB_ENDPOINT_IN_SHARED, report, sizeof(*report));
}

bool sval_point_transport_extra(report_extra_t *report) {
    return send(USB_ENDPOINT_IN_SHARED, report, sizeof(*report));
}

static host_driver_t wrapped;
static bool          installed;

static void observe_keyboard(report_keyboard_t *report) {
    sval_point_output_observe_keyboard(report);
}
static void observe_nkro(report_nkro_t *report) {
    sval_point_output_observe_nkro(report);
}
static void observe_extra(report_extra_t *report) {
    sval_point_output_observe_extra(report);
}

void sval_point_transport_install(void) {
    if (installed || !host_get_driver()) return;
    wrapped               = *host_get_driver();
    wrapped.send_keyboard = observe_keyboard;
    wrapped.send_nkro     = observe_nkro;
    wrapped.send_extra    = observe_extra;
    host_set_driver(&wrapped);
    installed = true;
    // Give the broker the keys already held, so its first contribution keeps them.
    send_keyboard_report();
}
