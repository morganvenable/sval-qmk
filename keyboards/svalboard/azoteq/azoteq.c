// Azoteq IQS5xx custom driver wrapper
// Calls upstream driver functions directly - no pointing_device.c patch needed

#include "quantum.h"
#include "pointing_device.h"
#include "drivers/sensors/azoteq_iqs5xx.h"
#include "svalboard.h"
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    include "multitouch.h"
#endif
#include "pointing_detect.h"

// Azoteq can only report at 100Hz. Faster polling causes issues.
#define AZO_MS 10

static uint16_t azo_timer = 0;
static uint8_t azo_held_buttons = 0;
static uint16_t azo_cached_cpi = 0;

// Public in the driver but missing from its header.
i2c_status_t azoteq_iqs5xx_end_session(void);

static uint16_t azo_product = AZOTEQ_IQS5XX_UNKNOWN;

// An IQS5xx in a low-power mode NACKs the first addressing, then holds SCL after the next until its
// communication window opens (datasheet 8.8.2), which can take longer than the driver's 10 ms
// timeout. Opening a window first lets what follows run inside it: an I2C STOP doesn't close it.
#define AZO_I2C_ADDRESS (0x74 << 1)
#define AZO_WAKE_TIMEOUT_MS 300
static void azo_wake(void) {
    i2c_init();
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (i2c_ping_address(AZO_I2C_ADDRESS, AZO_WAKE_TIMEOUT_MS) == I2C_STATUS_SUCCESS) return;
        wait_us(200); // the datasheet asks for at least 150 us before addressing again
    }
}

bool pointing_device_driver_init(void) {
    azo_wake();
    azoteq_iqs5xx_init();
    // The product number, read again here because init's result doesn't say whether one answered.
    azo_wake();
    azo_product = azoteq_iqs5xx_get_product();
    if (azo_product != AZOTEQ_IQS5XX_UNKNOWN) azoteq_iqs5xx_end_session();
    return true;
}

void sval_pointer_identity(sval_pointer_identity_t *id) {
    id->built_for = SVAL_SENSOR_AZOTEQ;
    if (azo_product != AZOTEQ_IQS5XX_UNKNOWN) {
        id->detected           = SVAL_SENSOR_AZOTEQ;
        id->product_id         = azo_product >> 8;
        id->inverse_product_id = azo_product & 0xFF;
        id->flags |= SVAL_POINTER_INIT_OK;
    } else {
        id->detected = SVAL_SENSOR_NO_RESPONSE;
    }
}

uint8_t sval_pointer_own_bus(void) {
    return SVAL_BUS_I2C;
}

#ifdef SVAL_AZOTEQ_MULTITOUCH
// The contact stream keeps the chip at its full 2048 x 1792 resolution, so the
// driver ignores the CPI setting (azoteq_iqs5xx_set_cpi). Apply it here
// instead, as the resolution register would have: min(native, CPI x pad size),
// so the cursor and the chip's scroll move as in a normal build.
#    define AZO_NATIVE_X 2048
#    define AZO_NATIVE_Y 1792
static int32_t azo_remainder[4];
static int32_t azo_scale(int32_t value, uint8_t axis, uint32_t scaled, uint32_t native) {
    int32_t total = value * (int32_t)scaled + azo_remainder[axis];
    int32_t out   = total / (int32_t)native;
    azo_remainder[axis] = total - out * (int32_t)native;
    return out;
}
static void azo_apply_cpi(report_mouse_t *r) {
    if (!azo_cached_cpi) return;
    uint32_t sx = ((uint32_t)azo_cached_cpi * 430 + 127) / 254, sy = ((uint32_t)azo_cached_cpi * 400 + 127) / 254;
    if (sx > AZO_NATIVE_X) sx = AZO_NATIVE_X;
    if (sy > AZO_NATIVE_Y) sy = AZO_NATIVE_Y;
    r->x = azo_scale(r->x, 0, sx, AZO_NATIVE_X);
    r->y = azo_scale(r->y, 1, sy, AZO_NATIVE_Y);
    r->h = azo_scale(r->h, 2, sx, AZO_NATIVE_X);
    r->v = azo_scale(r->v, 3, sy, AZO_NATIVE_Y);
}
#endif

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
#ifdef SVAL_AZOTEQ_MULTITOUCH
    const uint16_t interval = sval_mt_poll_ms();
#else
    const uint16_t interval = AZO_MS;
#endif
    if (timer_elapsed(azo_timer) >= interval) {
        azo_timer = timer_read();
        mouse_report = azoteq_iqs5xx_get_report(mouse_report);
        azo_held_buttons = mouse_report.buttons;
#ifdef SVAL_AZOTEQ_MULTITOUCH
        azo_apply_cpi(&mouse_report);
#endif
    }

    // Invert V-axis
    mouse_report.v = -mouse_report.v;
    mouse_report.buttons = azo_held_buttons;

#ifdef SVAL_AZOTEQ_MULTITOUCH
    if (sval_mt_suppress_mouse()) {
        mouse_report = (report_mouse_t){0};
        azo_held_buttons = 0;
    }
#endif
    return mouse_report;
}

uint16_t pointing_device_driver_get_cpi(void) {
    return azo_cached_cpi;
}

void pointing_device_driver_set_cpi(uint16_t cpi) {
    if (cpi != azo_cached_cpi) {
        azoteq_iqs5xx_set_cpi(cpi);
        azo_cached_cpi = cpi;
    }
}
