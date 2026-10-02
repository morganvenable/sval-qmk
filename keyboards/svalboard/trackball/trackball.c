// Trackball (PMW3360/PMW3389) custom driver wrapper
// Calls upstream driver functions directly - no pointing_device.c patch needed

#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/pmw33xx_common.h"
#include "svalboard.h"

static uint16_t trackball_cached_cpi = 0;

// The sensor's own rest modes: Run -> Rest1 (3 mA, 1 ms frames) after ~0.5 s without motion,
// Rest2 (100 ms frames) after ~10 s, Rest3 (500 ms frames) after ~10 min; motion wakes it on its
// own. QMK's init disables them (Config2 = 0), so we re-enable after init when the flag is set.
#define PMW33XX_CONFIG2_REST_EN 0x20
void sval_pointer_rest_apply(void) {
    bool on = (global_saved_values.idle_flags & SVAL_IDLE_POINTER_REST) != 0;
    pmw33xx_write(0, REG_Config2, on ? PMW33XX_CONFIG2_REST_EN : 0x00);
}

bool pointing_device_driver_init(void) {
    bool ok = pmw33xx_init(0);
    if (ok) sval_pointer_rest_apply();
    return ok;
}

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
    mouse_report = pmw33xx_get_report(mouse_report);

    // Axis swap for svalboard orientation
    int16_t swap = mouse_report.x;
    mouse_report.x = -mouse_report.y;
    mouse_report.y = -swap;

    return mouse_report;
}

uint16_t pointing_device_driver_get_cpi(void) {
    return trackball_cached_cpi;
}

void pointing_device_driver_set_cpi(uint16_t cpi) {
    if (cpi != trackball_cached_cpi) {
        pmw33xx_set_cpi_all_sensors(cpi);
        trackball_cached_cpi = cpi;
    }
}
