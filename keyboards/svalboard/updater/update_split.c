// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// In-firmware updater, M2: presence and the KEYBOARD_UPDATE split RPC (see
// update_split.h and update_split_wire.h).

#include <string.h>
#include "update_split.h"

// ---- presence (D17) ------------------------------------------------------------------

static uint8_t           presence; // update_presence_status_t, master only
static update_presence_t presence_last;

void update_split_presence_fill(void *out, uint8_t out_len) {
    if (out_len < UPDATE_PRESENCE_BYTES) return;
    update_build_id_t id;
    update_presence_t p;
    update_split_port_build_id(&id);
    usplit_presence_encode(&p, &id, update_split_port_updating());
    memcpy(out, &p, sizeof(p));
}

void update_split_presence_result(bool answered, const void *rsp, uint8_t len) {
    if (!answered) {
        presence = UPDATE_PRESENCE_NONE;
        return;
    }
    update_build_id_t id;
    update_split_port_build_id(&id);
    presence = usplit_presence_compare(rsp, len, &id, &presence_last);
}

uint8_t update_split_presence(void) {
    return presence;
}

bool update_split_mismatch(void) {
    return presence == UPDATE_PRESENCE_VERSION || presence == UPDATE_PRESENCE_SAME_HAND || presence == UPDATE_PRESENCE_INVALID;
}

// ---- KEYBOARD_UPDATE, slave side -----------------------------------------------------

static struct {
    uint8_t  state;      // update_state_t
    uint8_t  last_error; // update_status_t
    bool     cached;     // rsp answers the request (seq, crc)
    uint8_t  seq;
    uint16_t crc;
    uint8_t  rsp[USPLIT_MSG_MAX];
    uint8_t  rsp_len;
} sl;

uint8_t update_split_slave_state(void) {
    return sl.state;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// A request that passed usplit_req_parse. Fills the op's response payload.
static update_status_t run(const usplit_req_t *r, uint8_t *payload) {
    switch (r->op) {
        case USPLIT_OP_INFO: {
            update_build_id_t id;
            update_split_port_build_id(&id);
            payload[0] = id.updater_proto;
            payload[1] = id.hand;
            payload[2] = id.pointing_id;
            put32(&payload[3], id.fw_version);
            put32(&payload[7], id.git_hash);
            payload[11] = SVAL_UPDATE_STORAGE_FORMAT;
            put16(&payload[12], SVAL_UPDATE_SECURITY_EPOCH);
            payload[14] = update_split_port_info_flags();
            return UPDATE_OK;
        }
        case USPLIT_OP_STATUS:
            // Stage 1 has no session: nothing staged, nothing to erase.
            payload[0] = sl.last_error;
            return UPDATE_OK;
        case USPLIT_OP_ABORT:
            // Never erases (as the USB ABORT): BEGIN erases anyway.
            sl.state      = UPDATE_STATE_IDLE;
            sl.last_error = UPDATE_OK;
            return UPDATE_OK;
        default:
            // BEGIN, PAGE, END, COMMIT: well formed, but the mailbox,
            // programming and commit arrive in M2a stage 2. Nothing changes.
            return UPDATE_UNSUPPORTED;
    }
}

void update_split_slave_rpc(const uint8_t *in, uint8_t in_len, uint8_t *out, uint8_t out_len) {
    memset(out, 0, out_len);
    if (in_len < USPLIT_REQ_HDR) return; // no op and seq to answer to
    usplit_req_t    r;
    update_status_t st = usplit_req_parse(in, in_len, &r);
    if (st == UPDATE_OK && sl.cached && r.seq == sl.seq && r.crc == sl.crc) {
        // A retry of the last request (its answer was lost): the same answer,
        // and the request is not run again.
        if (out_len >= sl.rsp_len) memcpy(out, sl.rsp, sl.rsp_len);
        return;
    }
    uint8_t payload[USPLIT_MSG_MAX];
    memset(payload, 0, sizeof(payload));
    bool valid = st == UPDATE_OK;
    if (valid) st = run(&r, payload);
    uint8_t rsp[USPLIT_MSG_MAX];
    // An unknown op (or a frame too short to name one) builds nothing: the
    // master's check of the reply fails and it retries.
    uint8_t len = usplit_rsp_build(rsp, sizeof(rsp), (uint8_t)st, r.op, r.seq, sl.state, valid ? payload : NULL);
    if (!len) return;
    if (valid) {
        sl.cached  = true;
        sl.seq     = r.seq;
        sl.crc     = r.crc;
        sl.rsp_len = len;
        memcpy(sl.rsp, rsp, len);
    }
    if (out_len >= len) memcpy(out, rsp, len);
}

#ifdef SVAL_UPDATER_HOST_TEST
void update_split_host_reset(void) {
    memset(&sl, 0, sizeof(sl));
    presence = UPDATE_PRESENCE_NONE;
    memset(&presence_last, 0, sizeof(presence_last));
}
#else

// ---- firmware ------------------------------------------------------------------------

#    include "quantum.h"
#    include "split_common/transactions.h"
#    include "version.h" // QMK_GIT_HASH
#    include "updater.h"
#    include "updater_port.h"
#    include "update_flash.h"
#    include "update_image.h"
#    include "update_keys.h"

static update_build_id_t self_id;
static uint8_t           info_flags; // the constant bits

static void keyboard_update_rpc(uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    update_split_slave_rpc((const uint8_t *)in, in_len, (uint8_t *)out, out_len);
}

void update_split_init(void) {
    update_device_t dev;
    update_device_self(&dev);
    self_id.fw_version    = SVAL_FW_VERSION;
    self_id.git_hash      = usplit_git_hash32(QMK_GIT_HASH, &self_id.git_dirty);
    self_id.updater_proto = UPDATE_PROTOCOL_VERSION;
    self_id.hand          = dev.hand;
    self_id.pointing_id   = dev.pointing_id;
    // update_flash_available() probes the die the first time: here, on the
    // main thread, never in the SlaveThread.
    info_flags = (update_flash_available() ? 0x01 : 0) | (dev.release_build ? 0x04 : 0) | (update_key(UPDATE_KEY_TEST) ? 0x10 : 0) | (self_id.git_dirty ? 0x20 : 0);
#    ifdef SVAL_UPDATE_TEST_HOOKS
    info_flags |= 0x08;
#    endif
    transaction_register_rpc(KEYBOARD_UPDATE, keyboard_update_rpc);
}

void update_split_port_build_id(update_build_id_t *id) {
    *id = self_id;
}

uint8_t update_split_port_info_flags(void) {
    return info_flags | (updater_port_settings_failing() ? 0x02 : 0);
}

bool update_split_port_updating(void) {
    return updater_active();
}
#endif
