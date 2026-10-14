// SPDX-License-Identifier: GPL-2.0-or-later
// Svalboard experiment: a native Precision Touchpad interface (SVAL_NATIVE_PTP).
// tmk_core only routes USB traffic for PTP_INTERFACE to these keyboard hooks
// (keyboards/svalboard/azoteq/ptp.c). Nothing here exists without the flag.
#pragma once

#ifdef SVAL_NATIVE_PTP
#    include <stdbool.h>
#    include <stdint.h>

// report_type 1 input, 3 feature. Returns the reply length, or 0 to stall.
uint16_t sval_ptp_get_report(uint8_t report_type, uint8_t report_id, const uint8_t **data);
bool     sval_ptp_set_report_allowed(uint8_t report_type, uint8_t report_id, uint16_t length);
// Runs in the USB ISR after the SET_REPORT data stage.
bool sval_ptp_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data, uint16_t length);
// Runs in the USB ISR on bus reset and on SET_CONFIGURATION.
void sval_ptp_usb_reset(void);
#endif
