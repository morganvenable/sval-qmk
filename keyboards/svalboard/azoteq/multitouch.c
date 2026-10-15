// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in, local USB-half prototype. See tools/multitouch/README.md.
#ifdef SVAL_MT_HOST_TEST
#    include "mt_test_platform.h"
#else
#include "quantum.h"
#include "multitouch.h"
#include "drivers/sensors/azoteq_iqs5xx.h"
#include "usb_main.h"
#include <string.h>
#ifdef SVAL_MT_SPLIT
#    include "transactions.h"
#    include "transport.h"
#    include "split_util.h"
#    include "updater/updater.h"
#endif

extern usb_endpoint_in_t usb_endpoints_in[USB_ENDPOINT_IN_COUNT];

#endif
#ifdef SVAL_NATIVE_PTP
#    include "ptp.h"
#endif
#ifdef SVAL_PERF_STATS
#    include "perf.h"
#endif

#ifndef SVAL_MT_DEFAULT_PERIOD_MS
#    define SVAL_MT_DEFAULT_PERIOD_MS 10
#endif

#define FRAME_SIZE 56
#define QUEUE_SIZE 8
#define LEASE_MS 500

static uint8_t queue[QUEUE_SIZE][FRAME_SIZE];
static uint8_t head, count, fragment, last_mask, ids[5], next_id;
static uint16_t sequence, token;
static uint32_t client, renewed, epoch, dropped, errors, samples;
static bool want_owner, owner, recovering, pair_capture;
static uint32_t read_timeouts, read_errors, end_timeouts, end_errors, max_read_ms;
static uint32_t acquisition_faults, nack_faults, retry_attempts, retry_recovered, nonack_faults, last_i2c_flags;
void sval_mt_acquisition_fault(uint32_t flags, bool retried, bool recovered) {
    acquisition_faults++; last_i2c_flags = flags;
    if (flags == 0x04) nack_faults++; else nonack_faults++;
    if (retried) retry_attempts++;
    if (recovered) retry_recovered++;
}
static uint32_t measured_samples, cycle_sum, cycle_max, rate_missed, too_many, acquisition_sum;
static uint8_t configured_max = 5;
static uint16_t configured_period = SVAL_MT_DEFAULT_PERIOD_MS;
static bool restore_pending;
#ifdef SVAL_NATIVE_PTP
// Who owns the pads (ptp.h). RAM only: the build default returns at power-up.
static uint8_t touch_owner = SVAL_TOUCH_OWNER_DEFAULT;
#endif
#ifdef SVAL_MT_SPLIT
static uint8_t remote_snapshot[FRAME_SIZE];
static bool snapshot_valid, remote_seen, remote_stale;
static uint32_t snapshot_at, remote_received, remote_poll;
static uint16_t remote_sequence;
#endif
extern bool azoteq_iqs5xx_mt_config(uint8_t maximum, uint16_t period);

uint16_t sval_mt_poll_ms(void) { return configured_period; }

void sval_mt_read_status(i2c_status_t read_status, i2c_status_t end_status, uint32_t elapsed_ms) {
    if (read_status == I2C_STATUS_TIMEOUT) read_timeouts++;
    else if (read_status != I2C_STATUS_SUCCESS) read_errors++;
    if (end_status == I2C_STATUS_TIMEOUT) end_timeouts++;
    else if (end_status != I2C_STATUS_SUCCESS) end_errors++;
    if (elapsed_ms > max_read_ms) max_read_ms = elapsed_ms;
    acquisition_sum += elapsed_ms;
}

// QMK usb_endpoint_in_send resets/retries the queue even for TIME_IMMEDIATE.
// Post one whole packet atomically instead, preserving queued VIA replies.
#ifndef SVAL_MT_HOST_TEST
static bool try_send(const uint8_t report[RAW_EPSIZE]) {
    usb_endpoint_in_t *endpoint = &usb_endpoints_in[USB_ENDPOINT_IN_RAW];
    output_buffers_queue_t *q = &endpoint->obqueue;
    bool sent = false;
    osalSysLock();
    if (usbGetDriverStateI(endpoint->config.usbp) == USB_ACTIVE && !q->suspended &&
        q->ptr == NULL && !obqIsFullI(q) && obqGetEmptyBufferTimeoutS(q, TIME_IMMEDIATE) == MSG_OK) {
        memcpy(q->ptr, report, RAW_EPSIZE);
        obqPostFullBufferS(q, RAW_EPSIZE);
        sent = true;
    }
    osalSysUnlock();
    return sent;
}

#else
static bool try_send(const uint8_t report[RAW_EPSIZE]) { (void)report; return true; }
#endif

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void reset_stream(void) {
    count = fragment = head = last_mask = next_id = 0;
    memset(ids, 0, sizeof(ids));
    epoch++;
#ifdef SVAL_MT_SPLIT
    remote_seen = remote_stale = false;
#endif
}
static void stop_stream(void) {
    restore_pending |= configured_max != 5 || configured_period != SVAL_MT_DEFAULT_PERIOD_MS;
    recovering |= owner;
    owner = want_owner = pair_capture = false;
    client = 0;
    reset_stream();
}

bool sval_mt_suppress_pair_mouse(void) {
#ifdef SVAL_NATIVE_PTP
    // Input Mode 3 (or the hold after it): the touchpad interface owns both pads.
    if (is_keyboard_master() && sval_ptp_suppress_mouse()) return true;
#endif
    return is_keyboard_master() && pair_capture && client && timer_elapsed32(renewed) < LEASE_MS;
}

#ifdef SVAL_NATIVE_PTP
// 0xFC op 0x42: get or set the pads' owner. data[7] 0 get, 1 set; data[8] owner.
// Refusals keep the old owner; nothing falls back to anything.
static void owner_command(uint8_t *data, uint8_t length) {
    uint8_t action = data[7], wanted = data[8], status = 0;
    if (action == 1 && wanted != touch_owner) {
        if (wanted == SVAL_TOUCH_OWNER_DETECTOR || wanted > SVAL_TOUCH_OWNER_NATIVE) {
            status = 2; // the detector is not built; unknown owners are refused
        } else {
            if (touch_owner == SVAL_TOUCH_OWNER_HOST) stop_stream(); // ends any lease
            sval_ptp_set_native(wanted == SVAL_TOUCH_OWNER_NATIVE);
            touch_owner = wanted;
        }
    } else if (action > 1) {
        status = 2;
    }
    memset(data + 6, 0, length - 6);
    data[6] = 0xC2;
    data[7] = status;
    data[8] = touch_owner;
    data[9] = 1; // bit 0: native PTP built
    sval_ptp_status(data + 10);
}
#endif

bool sval_mt_suppress_mouse(void) {
    return is_keyboard_master() && (owner || recovering || sval_mt_suppress_pair_mouse());
}

void sval_mt_command(uint8_t *data, uint8_t length) {
    if (length != RAW_EPSIZE) return;
    uint32_t requester = get32(data + 1);
    uint8_t op = data[6], mode = data[7], status = 0;
    uint16_t request_token = data[8] | (uint16_t)data[9] << 8;
    if (op >= 5 && op <= 7) {
        if (!client || client != requester || request_token != token || timer_elapsed32(renewed) >= LEASE_MS || want_owner) status = 3;
        else if (op == 5) {
            uint8_t maximum = data[10];
            uint16_t period = data[11] | (uint16_t)data[12] << 8;
            if (maximum < 1 || maximum > 5 || period < 5 || period > 50) status = 2;
            else if (!azoteq_iqs5xx_mt_config(maximum, period)) { status = 4; restore_pending = true; }
            else { configured_max = maximum; configured_period = period; restore_pending = false; reset_stream(); }
        } else if (op == 7) measured_samples = cycle_sum = cycle_max = rate_missed = too_many = acquisition_sum = 0;
        memset(data + 6, 0, length - 6);
        data[6] = 0x80 | op; data[7] = status;
        if (op == 6) {
            put32(data + 8, measured_samples); put32(data + 12, cycle_sum);
            put32(data + 16, cycle_max); put32(data + 20, rate_missed);
            put32(data + 24, too_many); put32(data + 28, acquisition_sum);
        } else { data[8] = configured_max; put16(data + 9, configured_period); data[11] = restore_pending; }
        return;
    }
    if (op == 9) {
        extern uint32_t sval_mt_rp_abort_source;
        memset(data + 6, 0, length - 6); data[6] = 0x89;
        put32(data + 8, sval_mt_rp_abort_source);
        return;
    }
    if (op == 8) {
        memset(data + 6, 0, length - 6); data[6] = 0x88;
        put32(data + 8, acquisition_faults); put32(data + 12, nack_faults);
        put32(data + 16, retry_attempts); put32(data + 20, retry_recovered);
        put32(data + 24, nonack_faults); put32(data + 28, last_i2c_flags);
        return;
    }
    if (op == 4) {
        memset(data + 6, 0, length - 6);
        data[6] = 0x84;
        put32(data + 8, read_timeouts); put32(data + 12, read_errors);
        put32(data + 16, end_timeouts); put32(data + 20, end_errors);
        put32(data + 24, max_read_ms);
        return;
    }
#ifdef SVAL_PERF_STATS
    // 0xFC op 0x43: timing counters. data[7] 0 this half, 1 the other half,
    // 2 reset both; data[8] page. Reply: 0xC3, status, page, target, native
    // built, 0xA5, then 13 little-endian u32 from data[12].
    if (op == 0x43) {
        uint8_t  target = data[7], page = data[8];
        uint32_t words[SVAL_PERF_PAGE_WORDS] = {0};
        memset(data + 6, 0, length - 6);
        data[6] = 0xC3; data[8] = page; data[9] = target;
        data[11] = 0xA5; // marks a counters reply (older builds answer 0xC3 too)
#    ifdef SVAL_NATIVE_PTP
        data[10] = 1;
#    endif
        if (target == 0) {
            data[7] = sval_perf_page(page, words) ? 0 : 2;
        } else if (target == 1 || target == 2) {
#    ifdef SVAL_MT_SPLIT
            uint8_t req = target == 2 ? 0x20 : 0x10 | (page & 0x0F), reply[60] = {0};
            bool    ok  = !updater_active() && is_transport_connected() && transaction_rpc_exec(KEYBOARD_MT, 1, &req, sizeof(reply), reply) && reply[0] == 0xA8 && reply[1];
            data[7] = ok ? 0 : 1;
            if (ok && target == 1) memcpy(words, reply + 4, sizeof(words));
#    else
            data[7] = 1;
#    endif
            if (target == 2) sval_perf_reset();
        } else {
            data[7] = 2;
        }
        for (uint8_t i = 0; i < SVAL_PERF_PAGE_WORDS; i++) put32(data + 12 + 4 * i, words[i]);
        return;
    }
#endif
#ifdef SVAL_NATIVE_PTP
    if (op == 0x42) {
        owner_command(data, length);
        return;
    }
#endif
    if (client && timer_elapsed32(renewed) >= LEASE_MS) stop_stream();
    if (op == 1) {
#ifdef SVAL_NATIVE_PTP
        if (touch_owner != SVAL_TOUCH_OWNER_HOST) status = 5; // another owner has the pads
        else
#endif
        if (client && client != requester) status = 1; // busy
        else if (mode > 2) status = 2;
#ifdef SVAL_MT_SPLIT
        // Only the explicit unified host may acquire pair capture (mode 2).
        // Old single-pad clients requesting mode 1 must still be refused.
        else if (mode == 1) status = 2;
#else
        else if (mode == 2) status = 2;
#endif
        else {
            stop_stream();
            client = requester;
            renewed = timer_read32();
            want_owner = mode == 1;
            pair_capture = mode == 2;
            if (++token == 0) ++token;
        }
    } else if (op == 2 || op == 3) {
        if (!client || client != requester || request_token != token) status = 3;
        else if (op == 2) renewed = timer_read32();
        else stop_stream();
    } else if (op != 0) status = 2;
    memset(data + 6, 0, 26);
    data[6] = 0x80 | op;
    data[7] = status;
    data[8] = 1; // protocol version
    data[9] = (owner ? 1 : 0) | (want_owner ? 2 : 0) | (recovering ? 4 : 0) | (pair_capture ? 8 : 0);
    put16(data + 10, token);
    put16(data + 12, 2048);
    put16(data + 14, 1792);
    data[16] = 43; data[17] = 40; data[18] = 5;
    put32(data + 19, samples);
    put32(data + 23, errors);
    put32(data + 27, dropped);
}

void sval_mt_sample(const uint8_t *raw, bool valid) {
#ifndef SVAL_MT_SPLIT
    if (!is_keyboard_master()) return;
#endif
    samples++;
    uint8_t flags = 0, mask = 0;
    if (!valid) { flags |= 4; errors++; }
    else {
        measured_samples++; cycle_sum += raw[0];
        if (raw[0] > cycle_max) cycle_max = raw[0];
        if (raw[4] & 8) rate_missed++;
        if (raw[4] & 4) too_many++;
        if (raw[4] & 2) flags |= 1; // palm
        if (raw[4] & 4) flags |= 2; // too many fingers
        if (raw[3] & 0x98) flags |= 8; // reset, re-ATI, ATI error
        if (raw[5] > 5) flags |= 4;
        if (!flags && raw[5]) {
            for (uint8_t i = 0; i < 5; i++) {
                const uint8_t *p = raw + 10 + 7 * i;
                if (p[4] || p[5]) mask |= 1 << i;
            }
        }
    }
    // Only a trustworthy physical all-up sample permits a change of owner.
    if (valid && !flags && raw[5] == 0) {
        recovering = false;
        if (client && want_owner) owner = true;
    }
#ifdef SVAL_NATIVE_PTP
    // The native touchpad needs this half's frames without any host client.
    bool to_ptp = is_keyboard_master() && sval_ptp_wants_frames();
    if (!client && is_keyboard_master() && !to_ptp) return;
#else
    if (!client && is_keyboard_master()) return;
#endif
    if (flags || count == QUEUE_SIZE) {
        if (count == QUEUE_SIZE) { dropped++; flags |= 0x10; }
        reset_stream();
    }
    // Restart before an 8-bit lifetime ID can wrap ambiguously.
    if (next_id > 249 && (mask & ~last_mask)) { reset_stream(); flags |= 0x10; }
    uint8_t frame[FRAME_SIZE];
    memset(frame, 0, FRAME_SIZE);
    frame[0] = 1; frame[1] = is_keyboard_left() ? 0 : 1;
    frame[2] = flags | (owner ? 0x80 : 0);
    frame[3] = mask;
    put16(frame + 4, sequence++);
    put16(frame + 6, FRAME_SIZE);
    put32(frame + 8, timer_read32() * 1000u);
    put32(frame + 12, epoch);
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t *p = raw + 10 + 7 * i;
        uint8_t *out = frame + 16 + 8 * i;
        if ((mask & (1 << i)) && !(last_mask & (1 << i))) ids[i] = ++next_id;
        out[0] = ids[i];
        out[1] = p[1]; out[2] = p[0];
        out[3] = p[3]; out[4] = p[2];
        out[5] = p[5]; out[6] = p[4]; out[7] = p[6];
    }
    last_mask = mask;
#ifdef SVAL_MT_SPLIT
    if (!is_keyboard_master()) {
        // The split callback only copies a complete cached frame; no I2C in
        // the slave transport thread. This short critical section is bounded.
        osalSysLock();
        memcpy(remote_snapshot, frame, FRAME_SIZE);
        snapshot_at = timer_read32(); snapshot_valid = true;
        osalSysUnlock();
        return;
    }
#endif
#ifdef SVAL_NATIVE_PTP
    if (to_ptp) sval_ptp_feed(frame);
    if (!client) return;
#endif
    memcpy(queue[(head + count) % QUEUE_SIZE], frame, FRAME_SIZE);
    count++;
}

#ifdef SVAL_MT_SPLIT
// Wire v1: magic, valid, sample age:u16, coherent 56-byte contact frame.
// The two MCU clocks remain separate; age is measured on the source MCU.
static void mt_rpc(uint8_t in_len, const void *in, uint8_t out_len, void *out) {
#ifdef SVAL_PERF_STATS
    // 0x10 | page: this half's counters; 0x20: reset them. Same 60-byte reply.
    if (in_len == 1 && out_len == 60 && (*(const uint8_t *)in & 0xF0)) {
        uint8_t  req = *(const uint8_t *)in, *reply = out;
        uint32_t words[SVAL_PERF_PAGE_WORDS];
        memset(reply, 0, out_len);
        reply[0] = 0xA8;
        if (req == 0x20) {
            sval_perf_reset();
            reply[1] = 1;
        } else if ((req & 0xF0) == 0x10 && sval_perf_page(req & 0x0F, words)) {
            reply[1] = 1; reply[2] = req & 0x0F;
            memcpy(reply + 4, words, sizeof(words));
        }
        return;
    }
#endif
    if (in_len != 1 || *(const uint8_t *)in != 1 || out_len != 60) return;
    uint8_t *reply = out;
    memset(reply, 0, out_len);
    osalSysLock();
    reply[0] = 0xA7; reply[1] = snapshot_valid;
    uint32_t age = timer_elapsed32(snapshot_at);
    put16(reply + 2, age > 65535 ? 65535 : age);
    memcpy(reply + 4, remote_snapshot, FRAME_SIZE);
    osalSysUnlock();
}

static void queue_remote(uint8_t *frame) {
    if (count == QUEUE_SIZE) {
        // Drop oldest, flag the new frame so host per-pad guards cancel.
        head = (head + 1) % QUEUE_SIZE; count--; dropped++;
        frame[2] |= 0x10;
    }
    memcpy(queue[(head + count) % QUEUE_SIZE], frame, FRAME_SIZE);
    count++;
}

static void poll_remote(void) {
    if (timer_elapsed32(remote_poll) < 10 || updater_active()) return;
    remote_poll = timer_read32();
    uint8_t request = 1, reply[60] = {0};
#ifdef SVAL_PERF_STATS
    uint32_t rpc_started = sval_perf_now_us();
    bool     connected   = is_transport_connected();
#else
    const bool connected = is_transport_connected();
#endif
    bool ok = connected && transaction_rpc_exec(KEYBOARD_MT, 1, &request, sizeof(reply), reply);
#ifdef SVAL_PERF_STATS
    bool rpc_fresh = false;
#endif
    if (ok && reply[0] == 0xA7 && reply[1] && reply[4] == 1 &&
        reply[5] == (is_keyboard_left() ? 1 : 0)) {
        uint8_t *frame = reply + 4;
        uint16_t seq = frame[4] | (uint16_t)frame[5] << 8;
        uint16_t age = reply[2] | (uint16_t)reply[3] << 8;
        if (age <= 100) {
            remote_received = timer_read32();
            if (!remote_seen || remote_stale || seq != remote_sequence) {
                if (remote_stale) frame[2] |= 0x10;
#ifdef SVAL_NATIVE_PTP
                if (sval_ptp_wants_frames()) sval_ptp_feed(frame); // only the mouse hold
#endif
                queue_remote(frame);
#ifdef SVAL_PERF_STATS
                rpc_fresh = true;
#endif
            }
            remote_sequence = seq; remote_seen = true; remote_stale = false;
#ifdef SVAL_PERF_STATS
            sval_perf_rpc(sval_perf_now_us() - rpc_started, true, rpc_fresh);
#endif
            return;
        }
    }
#ifdef SVAL_PERF_STATS
    if (connected) sval_perf_rpc(sval_perf_now_us() - rpc_started, ok, false);
#endif
    if (remote_seen && !remote_stale && timer_elapsed32(remote_received) > 100) {
        uint8_t frame[FRAME_SIZE] = {0};
        frame[0] = 1; frame[1] = is_keyboard_left() ? 1 : 0;
        frame[2] = 0x20; put16(frame + 4, ++remote_sequence);
        put16(frame + 6, FRAME_SIZE);
        queue_remote(frame); remote_stale = true;
    }
}

#ifdef SVAL_NATIVE_PTP
// The native touchpad's own poll of the other half: faster, and no queue or
// host client. Duplicate snapshots are skipped; a silent half is lifted.
static uint32_t ptp_poll_at, ptp_received, ptp_remote_epoch;
static uint16_t ptp_remote_sequence;
static bool     ptp_remote_seen, ptp_remote_lost;

static void poll_remote_ptp(void) {
    if (timer_elapsed32(ptp_poll_at) < SVAL_PTP_REMOTE_POLL_MS || updater_active()) return;
    ptp_poll_at = timer_read32();
    uint8_t request = 1, reply[60] = {0};
    uint8_t remote_pad = is_keyboard_left() ? 1 : 0;
#ifdef SVAL_PERF_STATS
    uint32_t rpc_started = sval_perf_now_us();
    bool     connected   = is_transport_connected();
#else
    const bool connected = is_transport_connected();
#endif
    bool ok = connected && transaction_rpc_exec(KEYBOARD_MT, 1, &request, sizeof(reply), reply);
#ifdef SVAL_PERF_STATS
    bool rpc_fresh = false;
#endif
    if (ok && reply[0] == 0xA7 && reply[1] && reply[4] == 1 && reply[5] == remote_pad) {
        uint8_t *frame = reply + 4;
        uint16_t seq   = frame[4] | (uint16_t)frame[5] << 8;
        uint16_t age   = reply[2] | (uint16_t)reply[3] << 8;
        uint32_t ep    = get32(frame + 12);
        if (age <= SVAL_PTP_REMOTE_STALE_MS) {
            ptp_received = timer_read32();
            if (!ptp_remote_seen || ptp_remote_lost || seq != ptp_remote_sequence || ep != ptp_remote_epoch) {
                sval_ptp_feed(frame);
#    ifdef SVAL_PERF_STATS
                rpc_fresh = true;
#    endif
            }
            ptp_remote_sequence = seq;
            ptp_remote_epoch    = ep;
            ptp_remote_seen     = true;
            ptp_remote_lost     = false;
#    ifdef SVAL_PERF_STATS
            sval_perf_rpc(sval_perf_now_us() - rpc_started, true, rpc_fresh);
#    endif
            return;
        }
    }
#    ifdef SVAL_PERF_STATS
    if (connected) sval_perf_rpc(sval_perf_now_us() - rpc_started, ok, false);
#    endif
    if (ptp_remote_seen && !ptp_remote_lost && timer_elapsed32(ptp_received) > SVAL_PTP_REMOTE_STALE_MS) {
        sval_ptp_pad_lost(remote_pad);
        ptp_remote_lost = true;
    }
}
#endif
#endif

void sval_mt_init(void) {
#ifdef SVAL_MT_SPLIT
    transaction_register_rpc(KEYBOARD_MT, mt_rpc);
#endif
#ifdef SVAL_NATIVE_PTP
    sval_ptp_set_native(touch_owner == SVAL_TOUCH_OWNER_NATIVE);
#endif
}

void sval_mt_task(void) {
#ifdef SVAL_PERF_STATS
    sval_perf_loop();
#endif
    if (!is_keyboard_master()) return;
    if (restore_pending && azoteq_iqs5xx_mt_config(5, SVAL_MT_DEFAULT_PERIOD_MS)) {
        configured_max = 5; configured_period = SVAL_MT_DEFAULT_PERIOD_MS; restore_pending = false; reset_stream();
    }
#ifdef SVAL_NATIVE_PTP
    // The native touchpad runs without any host client.
    if (!client) {
#    ifdef SVAL_MT_SPLIT
        if (sval_ptp_wants_frames()) poll_remote_ptp();
#    endif
        sval_ptp_task();
        return;
    }
    sval_ptp_task();
#else
    if (!client) return;
#endif
    if (timer_elapsed32(renewed) >= LEASE_MS) { stop_stream(); return; }
#ifdef SVAL_MT_SPLIT
    poll_remote();
#endif
    // A bounded drain catches up after a slow scan without waiting for USB.
    for (uint8_t budget = 0; budget < 3 && count; budget++) {
    uint8_t report[RAW_EPSIZE] = {0};
    uint8_t *frame = queue[head];
    report[0] = 0xDD;
    put32(report + 1, client);
    report[5] = 0xFC; report[6] = 0x40;
    put16(report + 7, token);
#if RAW_EPSIZE == 64
    // Compact v1: the opcode implies version 1, so the remaining 55 frame
    // bytes fit with the nine-byte routing/session header in one USB report.
    report[6] = 0x41;
    memcpy(report + 9, frame + 1, FRAME_SIZE - 1);
    if (!try_send(report)) break;
    head = (head + 1) % QUEUE_SIZE;
    count--;
#else
    report[9] = frame[4]; report[10] = frame[5];
    report[11] = fragment;
    report[12] = fragment == 2 ? 18 : 19;
    memcpy(report + 13, frame + 19 * fragment, report[12]);
    if (try_send(report)) {
        if (++fragment == 3) { fragment = 0; head = (head + 1) % QUEUE_SIZE; count--; }
    } else break;
#endif
    }
}
