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

// Reads are timed by pad_schedule.h: from the end of the last read, so they
// land just before the chip's window opens instead of waiting in it.
#include "pad_schedule.h"
_Static_assert(CH_CFG_ST_FREQUENCY == 1000000, "the pad schedule assumes a 1 MHz system timer");
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    define AZO_READ_BYTES 53 // address + register, address + 45 bytes, end session
#else
#    define AZO_READ_BYTES 18 // address + register, address + 10 bytes, end session
#endif
#ifndef I2C1_CLOCK_SPEED
#    define I2C1_CLOCK_SPEED 100000 // the ChibiOS I2C driver's default
#endif
// 90% of the bare bus time (9 clocks a byte): never above the real transfer.
#define AZO_XFER_GUESS_US ((uint32_t)((uint64_t)AZO_READ_BYTES * 9 * 1000000 * 9 / 10 / I2C1_CLOCK_SPEED))
static pad_sched_t azo_sched;
static bool        azo_sched_ready = false;
i2c_status_t       azoteq_iqs5xx_last_base_status(void);

static inline uint32_t azo_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

#ifdef SVAL_PERF_STATS
void sval_pad_sched_stats(uint32_t *lead_us, uint32_t *xfer_us, uint32_t *wait_us) {
    *lead_us = azo_sched_ready ? (uint32_t)azo_sched.lead : 0;
    *xfer_us = azo_sched_ready ? azo_sched.xfer : 0;
    *wait_us = azo_sched_ready ? azo_sched.wait : 0;
}
#endif

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

report_mouse_t pointing_device_driver_get_report(report_mouse_t mouse_report) {
#ifdef SVAL_AZOTEQ_MULTITOUCH
    const uint16_t interval = sval_mt_poll_ms();
#else
    const uint16_t interval = AZO_MS;
#endif
    if (!azo_sched_ready) {
        pad_sched_init(&azo_sched, AZO_XFER_GUESS_US);
        azo_sched_ready = true;
    }
    uint32_t started = azo_now_us();
    if (pad_sched_due(&azo_sched, started)) {
        mouse_report = azoteq_iqs5xx_get_report(mouse_report);
        azo_held_buttons = mouse_report.buttons;
        pad_sched_done(&azo_sched, started, azo_now_us(), azoteq_iqs5xx_last_base_status() == I2C_STATUS_SUCCESS, interval * 1000u);
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
#if defined(SVAL_NATIVE_PTP) && defined(SVAL_MT_SPLIT) && defined(MOUSE_EXTENDED_REPORT)
    // The half without USB marks its split pointing report with a counter
    // that moves only when its pad has news (multitouch.c), so the USB half
    // fetches the pad snapshot only then. boot_x is otherwise always zero
    // here (host_mouse_send fills it), and the USB half clears it on arrival.
    if (!is_keyboard_master()) mouse_report.boot_x = (int8_t)sval_mt_split_hint();
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
