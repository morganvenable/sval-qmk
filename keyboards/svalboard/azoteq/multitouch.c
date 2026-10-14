// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in, local USB-half prototype. See tools/multitouch/README.md.
#ifdef SVAL_MT_HOST_TEST
#    include "mt_test_platform.h"
#    include "touch_gestures.h"
#else
#include "quantum.h"
#include "multitouch.h"
#include "touch_gestures.h"
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

#define FRAME_SIZE 56
#define QUEUE_SIZE 8
#define LEASE_MS 500
// Leaving unified mode keeps the chip quiet until both pads are all-up; this
// caps it if a pad never reports a trustworthy all-up (a stuck fault flag).
#ifndef SVAL_MT_RECOVER_MAX_MS
#    define SVAL_MT_RECOVER_MAX_MS 5000
#endif

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
static uint16_t configured_period = 10;
static bool restore_pending;
#ifdef SVAL_MT_SPLIT
static uint8_t remote_snapshot[FRAME_SIZE];
static bool snapshot_valid, remote_seen, remote_stale;
static uint32_t snapshot_at, remote_received, remote_poll;
static uint16_t remote_sequence;
#endif
// Pair capture ended (STOP or lease expiry): chip output stays off until both
// pads report all fingers up, so a handoff mid-touch can't jump or click.
static bool pair_recovering, local_all_up = true, far_up = true;
static uint32_t recover_since, far_at;
// Live contact counts per hand, for STATUS; and the last positions for auto mouse.
static uint8_t pad_count[2];
static uint32_t pad_count_at[2];
static bool pad_count_seen[2];
static uint8_t act_id[2][5];
static uint16_t act_x[2][5], act_y[2][5];
// Chip wheel held while a second finger may still turn into a firmware pinch.
static int16_t held_h[2], held_v[2];
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

// The auto-mouse layer in unified mode (keymaps/keymap_support.c). Movement is
// in 800-DPI counts. Weak so builds without it still link; host tests define it.
#ifndef SVAL_MT_HOST_TEST
__attribute__((weak)) void sval_mt_contact_activity(bool left, uint16_t movement) { (void)left; (void)movement; }
#else
void sval_mt_contact_activity(bool left, uint16_t movement);
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
    restore_pending |= configured_max != 5 || configured_period != 10;
    recovering |= owner;
    if (pair_capture) { pair_recovering = true; recover_since = timer_read32(); }
    owner = want_owner = pair_capture = false;
    client = 0;
    reset_stream();
}

static bool lease_live(void) { return client && timer_elapsed32(renewed) < LEASE_MS; }

// Unified touch mode: the explicit pair-capture lease is live.
bool sval_mt_suppress_pair_mouse(void) {
    return is_keyboard_master() && pair_capture && lease_live();
}

// The old single-pad injector's ownership (mode 1). Split builds refuse it.
bool sval_mt_suppress_mouse(void) {
    return is_keyboard_master() && (owner || recovering);
}

static bool far_is_up(void) {
#ifdef SVAL_MT_SPLIT
    return !is_transport_connected() || timer_elapsed32(far_at) > 100 || far_up;
#else
    return true;
#endif
}

// What of the chip's own mouse report survives: 0 everything, 1 the wheel only, 2 nothing.
static uint8_t chip_mode(void) {
    if (!is_keyboard_master()) return 0;
    if (tg_claimed()) return 2;
    if (sval_mt_suppress_pair_mouse()) return (tg_config.owners & TG_OWN_SCROLL_CHIP) ? 1 : 2;
    // A lease that just ran out but that sval_mt_task hasn't stopped yet is
    // already the handoff: no chip report may slip through in between.
    if (pair_recovering || pair_capture) return 2;
    return 0;
}

static int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : (int16_t)v; }

// One pad's chip report (pad 0 left hand). Returns chip_mode(); h and v are
// rewritten for the pinch hold. Kept separate from report_mouse_t for host tests.
uint8_t sval_mt_chip_filter(uint8_t pad, int16_t *h, int16_t *v) {
    uint8_t mode = chip_mode();
    if (pad > 1) return mode;
    if (mode == 2) { held_h[pad] = held_v[pad] = 0; return 2; } // a claimed pinch drops the held wheel
    if (tg_pinch_hold(pad, timer_read32())) {
        held_h[pad] = clamp16((int32_t)held_h[pad] + *h);
        held_v[pad] = clamp16((int32_t)held_v[pad] + *v);
        *h = *v = 0;
        return mode;
    }
    *h = clamp16((int32_t)*h + held_h[pad]);
    *v = clamp16((int32_t)*v + held_v[pad]);
    held_h[pad] = held_v[pad] = 0;
    return mode;
}

#ifndef SVAL_MT_HOST_TEST
void sval_mt_filter_reports(report_mouse_t *left, report_mouse_t *right) {
    report_mouse_t *reports[2] = {left, right};
    for (uint8_t pad = 0; pad < 2; pad++) {
        report_mouse_t *r = reports[pad];
        int16_t h = r->h, v = r->v;
        uint8_t mode = sval_mt_chip_filter(pad, &h, &v);
        if (mode == 2) *r = (report_mouse_t){0};
        else if (mode == 1) { *r = (report_mouse_t){0}; r->h = h; r->v = v; }
        else { r->h = h; r->v = v; }
    }
}
#endif

uint8_t sval_mt_lease_mode(void) {
    uint8_t mode = sval_mt_suppress_pair_mouse() ? 2 : lease_live() ? 1 : 0;
    return mode | (pair_recovering ? 0x80 : 0);
}

uint8_t sval_mt_pad_contacts(uint8_t pad) {
    if (pad > 1 || !pad_count_seen[pad] || timer_elapsed32(pad_count_at[pad]) > 100) return 0xFF;
    return pad_count[pad];
}

// In unified mode the pointing reports are zeroed before the layer logic, so
// one-finger contact movement drives the auto-mouse layer instead.
static void contact_activity(uint8_t pad, const uint8_t *frame) {
    uint8_t mask = (frame[2] & 0x2F) ? 0 : frame[3], n = 0;
    for (uint8_t i = 0; i < 5; i++) n += (mask >> i) & 1;
    uint32_t best = 0;
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t *p = frame + 16 + 8 * i;
        if (!(mask & (1 << i))) { act_id[pad][i] = 0; continue; }
        uint16_t x = p[1] | (uint16_t)p[2] << 8, y = p[3] | (uint16_t)p[4] << 8;
        if (n == 1 && act_id[pad][i] == p[0]) {
            uint32_t moved = (uint32_t)(x > act_x[pad][i] ? x - act_x[pad][i] : act_x[pad][i] - x) +
                             (uint32_t)(y > act_y[pad][i] ? y - act_y[pad][i] : act_y[pad][i] - y);
            if (moved > best) best = moved;
        }
        act_id[pad][i] = p[0]; act_x[pad][i] = x; act_y[pad][i] = y;
    }
    // TPS43: 2048 counts over 43 mm is about 1210 counts per inch.
    if (best && (sval_mt_suppress_pair_mouse() || pair_recovering) && !tg_claimed())
        sval_mt_contact_activity(pad == 0, (uint16_t)(best * 800 / 1210 > 65535 ? 65535 : best * 800 / 1210));
}

// Every frame the USB half sees, from either pad, passes here before it is
// queued: contact counts, auto mouse, the recognizer, and the claim flag.
static void observe(uint8_t *frame) {
    uint8_t pad = frame[1];
    if (pad > 1) return;
    uint32_t now = timer_read32();
    if (!(frame[2] & 0x2F)) {
        uint8_t n = 0;
        for (uint8_t i = 0; i < 5; i++) n += (frame[3] >> i) & 1;
        pad_count[pad] = n; pad_count_at[pad] = now; pad_count_seen[pad] = true;
    }
    contact_activity(pad, frame);
    tg_feed(pad, frame, now);
    if (tg_claimed()) frame[2] |= 0x40; // firmware claimed: the host withdraws these contacts
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
    if (client && timer_elapsed32(renewed) >= LEASE_MS) stop_stream();
    if (op == 1) {
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
    } else if (op == 2) {
        // Renew. The client wrapper's IDs expire after two minutes, so a renew
        // may also move the lease to a fresh wrapper ID: same token, and the
        // old ID in bytes 10-13. The lease and the touch in progress carry on.
        if (!client || request_token != token || (client != requester && get32(data + 10) != client)) status = 3;
        else { client = requester; renewed = timer_read32(); }
    } else if (op == 3) {
        if (!client || client != requester || request_token != token) status = 3;
        else stop_stream();
    } else if (op != 0) status = 2;
    memset(data + 6, 0, 26);
    data[6] = 0x80 | op;
    data[7] = status;
    data[8] = 1; // protocol version
    data[9] = (owner ? 1 : 0) | (want_owner ? 2 : 0) | (recovering ? 4 : 0) | (pair_capture ? 8 : 0) |
              ((tg_config.owners & TG_OWN_SCROLL_CHIP) ? 0x10 : 0) |
              ((tg_config.owners & (TG_OWN_SWIPE_FW | TG_OWN_PINCH_FW)) ? 0x20 : 0);
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
    if (valid && !flags) local_all_up = raw[5] == 0;
    // The USB half builds frames while streaming, and for the recognizer and the
    // handoff after unified mode even with no host attached.
    bool streaming = client != 0;
    if (is_keyboard_master() && !streaming && !tg_running() && !pair_recovering) return;
    if (flags || (streaming && count == QUEUE_SIZE)) {
        if (streaming && count == QUEUE_SIZE) { dropped++; flags |= 0x10; }
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
    observe(frame);
    if (!streaming) return;
    memcpy(queue[(head + count) % QUEUE_SIZE], frame, FRAME_SIZE);
    count++;
}

#ifdef SVAL_MT_SPLIT
// Wire v1: magic, valid, sample age:u16, coherent 56-byte contact frame.
// The two MCU clocks remain separate; age is measured on the source MCU.
static void mt_rpc(uint8_t in_len, const void *in, uint8_t out_len, void *out) {
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

// Polled every 10 ms while a host lease is held, while leaving unified mode,
// and whenever a gesture owner is the firmware (even with no host attached).
static void poll_remote(void) {
    if (timer_elapsed32(remote_poll) < 10 || updater_active()) return;
    remote_poll = timer_read32();
    uint8_t request = 1, reply[60] = {0};
    bool ok = is_transport_connected() && transaction_rpc_exec(KEYBOARD_MT, 1, &request, sizeof(reply), reply);
    if (ok && reply[0] == 0xA7 && reply[1] && reply[4] == 1 &&
        reply[5] == (is_keyboard_left() ? 1 : 0)) {
        uint8_t *frame = reply + 4;
        uint16_t seq = frame[4] | (uint16_t)frame[5] << 8;
        uint16_t age = reply[2] | (uint16_t)reply[3] << 8;
        if (age <= 100) {
            remote_received = far_at = timer_read32();
            if (!(frame[2] & 0x0F)) far_up = !frame[3]; // the far pad's own all-up flag
            if (!remote_seen || remote_stale || seq != remote_sequence) {
                if (remote_stale) frame[2] |= 0x10;
                observe(frame);
                if (client) queue_remote(frame);
            }
            remote_sequence = seq; remote_seen = true; remote_stale = false;
            return;
        }
    }
    if (remote_seen && !remote_stale && timer_elapsed32(remote_received) > 100) {
        uint8_t frame[FRAME_SIZE] = {0};
        frame[0] = 1; frame[1] = is_keyboard_left() ? 1 : 0;
        frame[2] = 0x20; put16(frame + 4, ++remote_sequence);
        put16(frame + 6, FRAME_SIZE);
        observe(frame);
        if (client) queue_remote(frame);
        remote_stale = true;
    }
}
#endif

void sval_mt_init(void) {
#ifdef SVAL_MT_SPLIT
    transaction_register_rpc(KEYBOARD_MT, mt_rpc);
#endif
}

void sval_mt_task(void) {
    if (!is_keyboard_master()) return;
    if (restore_pending && azoteq_iqs5xx_mt_config(5, 10)) {
        configured_max = 5; configured_period = 10; restore_pending = false; reset_stream();
    }
    if (client && timer_elapsed32(renewed) >= LEASE_MS) stop_stream();
#ifdef SVAL_MT_SPLIT
    if (client || pair_recovering || tg_running()) poll_remote();
#endif
    tg_tick(timer_read32());
    if (pair_recovering && !pair_capture &&
        ((local_all_up && far_is_up()) || timer_elapsed32(recover_since) > SVAL_MT_RECOVER_MAX_MS)) {
        pair_recovering = false;
    }
    tg_task();
    if (!client) return;
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
