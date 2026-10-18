// Host stand-ins for ptp.c (SVAL_PTP_HOST_TEST): a settable microsecond clock
// and an endpoint that records every report it accepts.
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t    ptp_test_us;
static bool        ptp_test_busy;
static uint8_t     ptp_sent[4096][35];
static unsigned    ptp_sent_count;
static FILE       *ptp_trace;
static const char *ptp_scenario = "start";

static uint32_t ptp_now_us(void) {
    return ptp_test_us;
}

static bool ptp_try_send(const uint8_t *report) {
    if (ptp_test_busy) return false;
    assert(ptp_sent_count < 4096);
    memcpy(ptp_sent[ptp_sent_count++], report, 35);
    if (ptp_trace) {
        fprintf(ptp_trace, "%s ", ptp_scenario);
        for (int i = 0; i < 35; i++)
            fprintf(ptp_trace, "%02x", report[i]);
        fprintf(ptp_trace, "\n");
    }
    return true;
}

#define PTP_LOCK()
#define PTP_UNLOCK()
