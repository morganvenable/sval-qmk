// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// In-firmware updater, M2: presence, the other half's session behind the
// KEYBOARD_UPDATE split RPC, and the relay from the half with USB (see
// update_split.h and update_split_wire.h).

#include <string.h>
#include "update_split.h"
#include "update_flash.h"
#include "update_image.h"
#include "update_keys.h"
#include "update_commit.h"
#include "updater_port.h"
#include "optional/monocypher-ed25519.h"

static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = (v >> 16) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get24(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static bool due(uint32_t now, uint32_t at) {
    return (int32_t)(now - at) >= 0;
}

// Uncached slot bytes into RAM, a word at a time (off and n multiples of 4).
static bool slot_copy(uint32_t off, uint8_t *dst, uint32_t n) {
    if (off % 4 || n % 4) return false;
    const volatile uint32_t *p = (const volatile uint32_t *)update_slot_read(off, n);
    if (!p) return false;
    for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)dst)[i] = p[i];
    return true;
}

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

uint8_t update_split_other(update_presence_t *p) {
    *p = presence_last;
    return presence;
}

// ---- KEYBOARD_UPDATE, the half without USB --------------------------------------------
//
// sl is shared by the RPC callback (SlaveThread, which holds the split
// shared-memory lock for the whole transaction) and housekeeping (which takes
// the lock to read or change it). hk is housekeeping's own.

#define SLAVE_COMMIT_DELAY_MS 100 // so the COMMIT answer reaches the master first (as updater.c)
#define SLAVE_VERIFY_SLICE 2048   // image bytes hashed per housekeeping pass (as updater.c)

static struct {
    // The last answer, for a retry of the same request (same seq and CRC).
    bool     cached;
    uint8_t  seq;
    uint16_t crc;
    uint8_t  rsp[USPLIT_MSG_MAX];
    uint8_t  rsp_len;
    // The session.
    uint8_t        state;      // update_state_t
    uint8_t        last_error; // update_status_t
    uint32_t       mseq;       // mailbox sequence number: moves with every new session and every cancel
    uint32_t       rx_ms;      // the last well-formed KEYBOARD_UPDATE request
    uint8_t        filled;     // signed manifest bytes received
    uint32_t       image_len;  // from the checked manifest (ERASING on)
    uint32_t       erase_off, erase_end;
    uint32_t       crc_body; // VERIFIED: CRC-32/MPEG-2 over [0x100, image_len)
    uint32_t       commit_due_ms;
    uint8_t        manifest[UPDATE_SIGNED_MANIFEST_BYTES] __attribute__((aligned(4)));
    usplit_pages_t pages; // RECEIVING: the page being assembled
} sl;

static struct {
    bool              have; // manifest is the copy for mailbox sequence mseq
    uint32_t          mseq;
    bool              verifying; // VERIFYING_IMAGE has begun: sha, verify_off and crc are live
    uint32_t          verify_off;
    uint32_t          crc;
    crypto_sha512_ctx sha;
    uint8_t           manifest[UPDATE_SIGNED_MANIFEST_BYTES] __attribute__((aligned(4)));
} hk;

#define SL_MANIFEST ((const sval_update_manifest_t *)hk.manifest)

uint8_t update_split_slave_state(void) {
    return sl.state;
}

bool update_split_slave_active(void) {
    return sl.state != UPDATE_STATE_IDLE;
}

// To IDLE, with last_error kept as the reason. Never erases (as the USB
// ABORT): BEGIN erases anyway. Callback, or housekeeping holding the lock.
static void slave_cancel(uint8_t error) {
    sl.state      = UPDATE_STATE_IDLE;
    sl.last_error = error;
    sl.mseq++;
    sl.filled    = 0;
    sl.image_len = sl.erase_off = sl.erase_end = sl.crc_body = 0;
    memset(sl.manifest, 0, sizeof(sl.manifest));
    memset(&sl.pages, 0, sizeof(sl.pages));
}

static update_status_t slave_fail(update_status_t st) {
    sl.state      = UPDATE_STATE_ERROR;
    sl.last_error = st;
    return st;
}

static update_status_t slave_begin(const usplit_req_t *r, uint8_t *payload) {
    if (r->off == 0) {
        // A new session, from IDLE or over a half-sent manifest only.
        if (sl.state != UPDATE_STATE_IDLE && sl.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
        if (!update_flash_available()) return UPDATE_UNAVAILABLE; // probed at init, on the main thread
        slave_cancel(UPDATE_OK);
        sl.state = UPDATE_STATE_MANIFEST_LOADING;
    } else if (sl.state != UPDATE_STATE_MANIFEST_LOADING) {
        // A repeat of a fragment already taken is acknowledged; nothing else.
        bool repeat = sl.state != UPDATE_STATE_IDLE && sl.state != UPDATE_STATE_ERROR && r->off + r->n <= sl.filled;
        payload[0]  = sl.filled;
        return repeat ? UPDATE_OK : UPDATE_INVALID;
    } else if (r->off + r->n <= sl.filled) {
        payload[0] = sl.filled; // a repeat: acknowledged, not written again
        return UPDATE_OK;
    } else if (r->off != sl.filled) {
        payload[0] = sl.filled;
        return UPDATE_OUT_OF_ORDER;
    }
    memcpy(sl.manifest + r->off, r->data, r->n);
    sl.filled  = (uint8_t)(r->off + r->n);
    payload[0] = sl.filled;
    // The last fragment posts the first job: housekeeping copies the manifest
    // out, checks it and its signature (D7), then erases.
    if (sl.filled == UPDATE_SIGNED_MANIFEST_BYTES) sl.state = UPDATE_STATE_VERIFYING_MANIFEST;
    return UPDATE_OK;
}

static update_status_t slave_page(const usplit_req_t *r, uint8_t *payload) {
    if (sl.state == UPDATE_STATE_VERIFYING_MANIFEST || sl.state == UPDATE_STATE_ERASING) {
        put24(payload, 0);
        return UPDATE_BUSY;
    }
    if (sl.state != UPDATE_STATE_RECEIVING) {
        put24(payload, sl.state == UPDATE_STATE_IDLE ? 0 : sl.pages.next);
        return UPDATE_INVALID;
    }
    uint32_t        done;
    update_status_t st = usplit_pages_accept(&sl.pages, r->off, r->data, r->n, &done);
    put24(payload, sl.pages.next);
    if (st == UPDATE_OVERRUN) return slave_fail(UPDATE_OVERRUN);
    if (st != UPDATE_OK) return st; // OUT_OF_ORDER or INVALID: refused, nothing changes
    if (done != UINT32_MAX) {
        // One page, here in the SlaveThread while the master waits for the
        // answer (R16): no interrupts-off window races the link.
        update_status_t f = update_flash_program_page(done, sl.pages.page);
        if (f != UPDATE_OK) return slave_fail(f == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
    }
    return UPDATE_OK;
}

static void slave_status(uint8_t *payload) {
    bool rx = sl.state >= UPDATE_STATE_RECEIVING && sl.state <= UPDATE_STATE_COMMITTING;
    payload[0] = sl.last_error;
    put24(&payload[1], rx ? sl.pages.next : 0);
    put16(&payload[4], sl.erase_off / UPDATE_FLASH_SECTOR);
    put16(&payload[6], sl.erase_end / UPDATE_FLASH_SECTOR);
    put16(&payload[8], sl.state == UPDATE_STATE_VERIFIED || sl.state == UPDATE_STATE_COMMITTING ? sl.crc_body & 0xFFFF : 0);
    payload[10] = sl.state != UPDATE_STATE_IDLE ? 0x01 : 0;
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
        case USPLIT_OP_BEGIN:
            return slave_begin(r, payload);
        case USPLIT_OP_PAGE:
            return slave_page(r, payload);
        case USPLIT_OP_STATUS:
            slave_status(payload);
            return UPDATE_OK;
        case USPLIT_OP_END:
            // A repeat (its answer lost, and the retry cache since overwritten) is acknowledged again.
            if (sl.state == UPDATE_STATE_VERIFYING_IMAGE || sl.state == UPDATE_STATE_VERIFIED) return UPDATE_ACCEPTED;
            if (sl.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
            if (sl.pages.next != sl.image_len) return UPDATE_OUT_OF_ORDER; // END before the last byte
            sl.state = UPDATE_STATE_VERIFYING_IMAGE;                     // housekeeping: SHA-512, structure, CRC
            return UPDATE_ACCEPTED;
        case USPLIT_OP_COMMIT:
            if (sl.state == UPDATE_STATE_COMMITTING && r->crc_lo16 == (sl.crc_body & 0xFFFF)) return UPDATE_ACCEPTED; // a repeat: one commit
            if (sl.state != UPDATE_STATE_VERIFIED) return UPDATE_INVALID;
            if (r->crc_lo16 != (sl.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
            if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;     // D12
            if (!update_commit_available()) return UPDATE_UNSUPPORTED;
            // Housekeeping runs the commit (update_commit.c, unchanged) behind
            // the master's pause, which the master took before sending this.
            sl.state         = UPDATE_STATE_COMMITTING;
            sl.commit_due_ms = update_split_port_now_ms() + SLAVE_COMMIT_DELAY_MS;
            return UPDATE_ACCEPTED;
        default: // USPLIT_OP_ABORT
            if (sl.state == UPDATE_STATE_COMMITTING) return UPDATE_BUSY; // the copy may have started (D23)
            if (sl.state != UPDATE_STATE_IDLE) slave_cancel(UPDATE_OK);
            return UPDATE_OK;
    }
}

void update_split_slave_rpc(const uint8_t *in, uint8_t in_len, uint8_t *out, uint8_t out_len) {
    memset(out, 0, out_len);
    if (in_len < USPLIT_REQ_HDR) return; // no op and seq to answer to
    usplit_req_t    r;
    update_status_t st = usplit_req_parse(in, in_len, &r);
    if (st == UPDATE_OK) sl.rx_ms = update_split_port_now_ms(); // traffic: the session's timeout restarts
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

// ---- the slave's housekeeping ----------------------------------------------------------

// Posts a job's result if the session is still where the job found it (same
// mailbox sequence number and state): else an ABORT, the timeout or a new
// session won the race, and the result is dropped.
static bool post_begin(uint32_t mseq, uint8_t state) {
    update_split_port_lock();
    if (sl.mseq == mseq && sl.state == state) return true; // the caller posts, then unlocks
    update_split_port_unlock();
    return false;
}

static void slave_check_manifest(uint32_t mseq) {
    update_device_t dev;
    update_device_self(&dev);
    update_status_t st = updater_port_settings_failing() ? UPDATE_UNAVAILABLE : update_manifest_check(SL_MANIFEST, &dev);
    if (st == UPDATE_OK) st = update_manifest_signature(hk.manifest); // this half checks it itself (D7)
    if (!post_begin(mseq, UPDATE_STATE_VERIFYING_MANIFEST)) return;
    if (st != UPDATE_OK) {
        slave_fail(st);
    } else {
        sl.image_len = SL_MANIFEST->image_len;
        sl.erase_off = 0;
        sl.erase_end = (sl.image_len + UPDATE_FLASH_SECTOR - 1) / UPDATE_FLASH_SECTOR * UPDATE_FLASH_SECTOR;
        sl.state     = UPDATE_STATE_ERASING;
    }
    update_split_port_unlock();
}

// One sector per pass, as the M1 path; sectors already erased are skipped.
static void slave_erase(uint32_t mseq, uint32_t off) {
    update_status_t st = update_flash_erase_sector(off);
    if (!post_begin(mseq, UPDATE_STATE_ERASING)) return;
    if (sl.erase_off == off) {
        if (st != UPDATE_OK) {
            slave_fail(st == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
        } else {
            sl.erase_off += UPDATE_FLASH_SECTOR;
            if (sl.erase_off >= sl.erase_end) {
                usplit_pages_init(&sl.pages, sl.image_len);
                sl.state = UPDATE_STATE_RECEIVING;
            }
        }
    }
    update_split_port_unlock();
}

static void slave_verify(uint32_t mseq) {
    uint32_t len = SL_MANIFEST->image_len;
    if (!hk.verifying) {
        crypto_sha512_init(&hk.sha);
        hk.verify_off = 0;
        hk.crc        = UPDATE_CRC32_INIT;
        hk.verifying  = true;
    }
    uint8_t         buf[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
    update_status_t st  = UPDATE_OK;
    uint32_t        end = len - hk.verify_off > SLAVE_VERIFY_SLICE ? hk.verify_off + SLAVE_VERIFY_SLICE : len;
    while (hk.verify_off < end) {
        uint32_t n = end - hk.verify_off < sizeof(buf) ? end - hk.verify_off : sizeof(buf);
        if (!slot_copy(hk.verify_off, buf, n)) {
            st = UPDATE_FLASH_ERR;
            break;
        }
        crypto_sha512_update(&hk.sha, buf, n);
        uint32_t skip = hk.verify_off < UPDATE_VECTORS_OFFSET ? UPDATE_VECTORS_OFFSET - hk.verify_off : 0;
        if (skip < n) hk.crc = update_crc32_mpeg2_update(hk.crc, buf + skip, n - skip);
        hk.verify_off += n;
    }
    if (st == UPDATE_OK && hk.verify_off < len) return; // more next pass
    if (st == UPDATE_OK) {
        uint8_t hash[UPDATE_SHA512_BYTES];
        crypto_sha512_final(&hk.sha, hash);
        st = memcmp(hash, SL_MANIFEST->sha512, UPDATE_SHA512_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_HASH;
    }
    if (st == UPDATE_OK) {
        uint8_t head[UPDATE_HEAD_BYTES] __attribute__((aligned(4)));
        st = slot_copy(0, head, sizeof(head)) ? update_image_head_check(head, len) : UPDATE_FLASH_ERR;
    }
    hk.verifying = false;
    if (!post_begin(mseq, UPDATE_STATE_VERIFYING_IMAGE)) return;
    if (st != UPDATE_OK) {
        slave_fail(st);
    } else {
        sl.crc_body = hk.crc;
        sl.state    = UPDATE_STATE_VERIFIED;
    }
    update_split_port_unlock();
}

static void slave_commit(uint32_t mseq) {
    if (!post_begin(mseq, UPDATE_STATE_COMMITTING)) return;
    // Image bytes in RAM: the page buffer goes now (the commit sweeps the
    // double-tap magic from the rest of RAM itself, R23).
    memset(&sl.pages, 0, sizeof(sl.pages));
    update_split_port_unlock();
    memset(&hk.sha, 0, sizeof(hk.sha));
    // The M1 commit, unchanged: step 0 checks the manifest against this build
    // and the slot's SHA-512 and CRC again. It returns only when it refused
    // before touching the firmware.
    update_status_t st = update_commit_run(SL_MANIFEST, hk.crc);
    if (!post_begin(mseq, UPDATE_STATE_COMMITTING)) return;
    slave_fail(st != UPDATE_OK ? st : UPDATE_FLASH_ERR);
    update_split_port_unlock();
}

void update_split_slave_task(void) {
    if (sl.state == UPDATE_STATE_IDLE) return; // a byte read: the common case takes no lock
    uint32_t now = update_split_port_now_ms();
    update_split_port_lock();
    uint8_t  state = sl.state;
    uint32_t mseq  = sl.mseq;
    if (state == UPDATE_STATE_IDLE) {
        update_split_port_unlock();
        return;
    }
    // No KEYBOARD_UPDATE traffic for 5 s: the master is gone (link pulled, or
    // it failed and its ABORT was lost). Drop the session, keeping any error
    // as the reason; the LEDs go back to normal. Not once COMMIT was accepted.
    if (state != UPDATE_STATE_COMMITTING && now - sl.rx_ms >= SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS) {
        slave_cancel(state == UPDATE_STATE_ERROR ? sl.last_error : UPDATE_TIMEOUT);
        update_split_port_unlock();
        return;
    }
    // The mailbox: the manifest is copied out once per session, here, under
    // the lock; everything after works on the copy.
    if (state == UPDATE_STATE_VERIFYING_MANIFEST && !(hk.have && hk.mseq == mseq)) {
        memcpy(hk.manifest, sl.manifest, sizeof(hk.manifest));
        hk.have      = true;
        hk.mseq      = mseq;
        hk.verifying = false;
    }
    bool     mine       = hk.have && hk.mseq == mseq;
    uint32_t erase_off  = sl.erase_off;
    bool     commit_now = state == UPDATE_STATE_COMMITTING && due(now, sl.commit_due_ms);
    bool     lost       = !mine && (state == UPDATE_STATE_ERASING || state == UPDATE_STATE_RECEIVING || state == UPDATE_STATE_VERIFYING_IMAGE || state == UPDATE_STATE_VERIFIED || state == UPDATE_STATE_COMMITTING);
    if (lost) slave_fail(UPDATE_INVALID); // cannot happen: the copy is taken before ERASING
    update_split_port_unlock();
    if (lost) return;

    switch (state) {
        case UPDATE_STATE_VERIFYING_MANIFEST:
            slave_check_manifest(mseq);
            break;
        case UPDATE_STATE_ERASING:
            slave_erase(mseq, erase_off);
            break;
        case UPDATE_STATE_VERIFYING_IMAGE:
            slave_verify(mseq);
            break;
        case UPDATE_STATE_COMMITTING:
            if (commit_now) slave_commit(mseq);
            break;
        default:
            break;
    }
}

// ---- the relay, the half with USB -----------------------------------------------------

#define RELAY_RESYNC_MAX 8
// A request that arrives damaged is answered INVALID with its op and seq (when
// those survived) and is not cached, so the same frame sent again runs; a
// well-formed request the other half refuses as INVALID is cached and gets
// INVALID again. So INVALID is taken as a refusal only on the fourth answer
// in a row to the same frame.
#define RELAY_INVALID_TRIES 4

static uint8_t master_seq; // every new request gets the next one (retries keep theirs)

static struct {
    uint8_t         phase; // relay_phase_t
    update_status_t error;
    uint8_t         manifest[UPDATE_SIGNED_MANIFEST_BYTES];
    uint32_t        image_len, crc_body;
    uint32_t        off;   // RELAY_MANIFEST: manifest bytes taken; RELAY_PAGES: next image byte to send
    uint8_t         req[USPLIT_MSG_MAX];
    uint8_t         req_len, req_n;
    bool            pending; // req is out (or to go out again) without a valid answer
    bool            paused;
    uint32_t        start_ms, end_ms, last_ok_ms, next_ms, hold_until_ms;
    uint8_t         slave_state, slave_error;
    uint16_t        slave_erased, slave_to_erase;
    uint32_t        acked;
    uint16_t        retries, resyncs;
    uint8_t         invalids; // INVALID answers in a row to the pending frame
    uint32_t        page_off; // the slot page in page[], or UINT32_MAX
    uint8_t         page[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
} rl;

bool update_relay_busy(void) {
    return rl.phase != RELAY_IDLE && rl.phase != RELAY_DONE && rl.phase != RELAY_FAILED;
}

static bool relay_pause(void) {
    if (rl.paused) return true;
    if (!update_split_port_pause(true)) return false;
    rl.paused = true;
    return true;
}

static void relay_resume(void) {
    if (!rl.paused) return;
    update_split_port_pause(false);
    rl.paused = false;
}

static bool relay_request(uint8_t len, uint8_t n) {
    rl.req_len  = len;
    rl.req_n    = n;
    rl.pending  = len != 0;
    rl.invalids = 0;
    return len != 0;
}

static bool relay_new(uint8_t op) {
    return relay_request(usplit_req_build(rl.req, op, ++master_seq, NULL, 0), 0);
}

static void relay_send_abort(void) {
    uint8_t req[USPLIT_MSG_MAX], rsp[USPLIT_MSG_MAX];
    uint8_t len = usplit_req_build(req, USPLIT_OP_ABORT, ++master_seq, NULL, 0);
    update_split_port_rpc(req, len, rsp, usplit_rsp_len(USPLIT_OP_ABORT)); // one try: the other half times out anyway
}

static void relay_fail(update_status_t st) {
    if (!update_relay_busy()) return;
    rl.phase   = RELAY_FAILED;
    rl.error   = st;
    rl.pending = false;
    rl.end_ms  = update_split_port_now_ms();
    // The other half drops its session (it refuses while committing), then
    // the link runs again.
    relay_send_abort();
    relay_resume();
}

// The other half answered in a state the relay did not expect: its error if
// it latched one, else it lost the session (it rebooted, or timed out).
static void relay_lost(void) {
    relay_fail(rl.slave_error != UPDATE_OK ? (update_status_t)rl.slave_error : UPDATE_UNAVAILABLE);
}

// Sends the pending request once. True with *r filled on a valid answer; on
// none, the request stays pending for a retry, and the relay fails once there
// has been no valid answer for SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS.
static bool relay_exchange(usplit_rsp_t *r, uint8_t *buf) {
    uint8_t op   = rl.req[0];
    uint8_t want = usplit_rsp_len(op);
    memset(buf, 0, USPLIT_MSG_MAX);
    bool     ok  = update_split_port_rpc(rl.req, rl.req_len, buf, want) && usplit_rsp_parse(buf, want, op, rl.req[1], r) == UPDATE_OK;
    uint32_t now = update_split_port_now_ms();
    if (ok && r->status == UPDATE_INVALID && ++rl.invalids < RELAY_INVALID_TRIES) ok = false; // maybe damaged on the way: again
    if (ok) {
        rl.pending     = false;
        rl.last_ok_ms  = now;
        rl.slave_state = r->state;
        return true;
    }
    rl.retries++;
    if (now - rl.last_ok_ms >= SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS) relay_fail(UPDATE_UNAVAILABLE);
    return false;
}

static void relay_status_payload(const usplit_rsp_t *r) {
    rl.slave_error    = r->payload[0];
    rl.slave_erased   = get16(&r->payload[4]);
    rl.slave_to_erase = get16(&r->payload[6]);
}

// One request and its answer at most. True when the relay may go straight on
// in this pass.
static bool relay_step(void) {
    uint8_t      buf[USPLIT_MSG_MAX];
    usplit_rsp_t r;
    uint32_t     now = update_split_port_now_ms();
    memset(&r, 0, sizeof(r)); // a request that could not be built fails as INVALID below
    switch (rl.phase) {
        case RELAY_START:
            // An old session on the other half (a failed relay whose ABORT was
            // lost) would refuse BEGIN: clear it first. ABORT never erases.
            if (!rl.pending && !relay_new(USPLIT_OP_ABORT)) break;
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            rl.phase = RELAY_MANIFEST;
            rl.off   = 0;
            return true;

        case RELAY_MANIFEST: {
            if (!rl.pending) {
                uint8_t n = UPDATE_SIGNED_MANIFEST_BYTES - rl.off < USPLIT_BEGIN_DATA_MAX ? (uint8_t)(UPDATE_SIGNED_MANIFEST_BYTES - rl.off) : USPLIT_BEGIN_DATA_MAX;
                if (!relay_request(usplit_req_begin(rl.req, ++master_seq, (uint8_t)rl.off, rl.manifest + rl.off, n), n)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            if (r.payload[0] != rl.off + rl.req_n) {
                relay_fail(UPDATE_INVALID);
                return false;
            }
            rl.off = r.payload[0];
            if (rl.off < UPDATE_SIGNED_MANIFEST_BYTES) return true;
            // The other half now checks the signature and erases, behind the pause.
            rl.phase   = RELAY_ERASE_WAIT;
            rl.next_ms = now + SVAL_UPDATE_RELAY_POLL_MS;
            return false;
        }

        case RELAY_ERASE_WAIT:
        case RELAY_VERIFY_WAIT:
        case RELAY_VERIFIED:
            if (!rl.pending) {
                if (!due(now, rl.next_ms)) return false;
                if (!relay_new(USPLIT_OP_STATUS)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_OK) break;
            relay_status_payload(&r);
            rl.next_ms = now + (rl.phase == RELAY_VERIFIED ? SVAL_UPDATE_RELAY_KEEPALIVE_MS : SVAL_UPDATE_RELAY_POLL_MS);
            if (r.state == UPDATE_STATE_ERROR) {
                relay_lost();
                return false;
            }
            if (rl.phase == RELAY_ERASE_WAIT) {
                if (r.state == UPDATE_STATE_VERIFYING_MANIFEST || r.state == UPDATE_STATE_ERASING) return false;
                if (r.state != UPDATE_STATE_RECEIVING) {
                    relay_lost();
                    return false;
                }
                // Erased: the image goes over with the link running, so both
                // halves keep typing between the RPCs.
                relay_resume();
                rl.phase = RELAY_PAGES;
                rl.off   = 0;
                return true;
            }
            if (rl.phase == RELAY_VERIFY_WAIT) {
                if (r.state == UPDATE_STATE_VERIFYING_IMAGE) return false;
                if (r.state != UPDATE_STATE_VERIFIED) {
                    relay_lost();
                    return false;
                }
                if (get16(&r.payload[8]) != (rl.crc_body & 0xFFFF)) {
                    relay_fail(UPDATE_BAD_HASH); // it verified some other image
                    return false;
                }
                relay_resume();
                rl.phase   = RELAY_VERIFIED;
                rl.next_ms = now + SVAL_UPDATE_RELAY_KEEPALIVE_MS;
                return false;
            }
            if (r.state != UPDATE_STATE_VERIFIED) relay_lost(); // RELAY_VERIFIED: still holding it?
            return false;

        case RELAY_PAGES: {
            if (!rl.pending) {
                if (rl.off >= rl.image_len) {
                    // All acknowledged: the other half verifies behind the pause.
                    if (!relay_pause()) {
                        relay_fail(UPDATE_UNAVAILABLE);
                        return false;
                    }
                    if (!relay_new(USPLIT_OP_END)) break;
                    rl.phase = RELAY_END;
                    return true;
                }
                uint32_t base = rl.off - rl.off % UPDATE_FLASH_PAGE;
                if (rl.page_off != base) {
                    if (!update_split_port_slot_read(base, rl.page, UPDATE_FLASH_PAGE)) {
                        relay_fail(UPDATE_FLASH_ERR);
                        return false;
                    }
                    rl.page_off = base;
                }
                uint32_t at = rl.off % UPDATE_FLASH_PAGE;
                uint32_t n  = UPDATE_FLASH_PAGE - at < USPLIT_PAGE_DATA_MAX ? UPDATE_FLASH_PAGE - at : USPLIT_PAGE_DATA_MAX;
                if (n > rl.image_len - rl.off) n = rl.image_len - rl.off;
                if (!relay_request(usplit_req_page(rl.req, ++master_seq, rl.off, rl.page + at, (uint8_t)n), (uint8_t)n)) break;
            }
            if (!relay_exchange(&r, buf)) return false;
            uint32_t next = get24(r.payload);
            if (r.status == UPDATE_OK && next >= rl.off + rl.req_n && next <= rl.image_len) {
                rl.off   = next;
                rl.acked = next;
                return true;
            }
            if (r.status == UPDATE_OUT_OF_ORDER && next < rl.image_len && rl.resyncs < RELAY_RESYNC_MAX) {
                rl.resyncs++; // the other half holds [0, next): carry on from there
                rl.off = next;
                return true;
            }
            if (r.status == UPDATE_BUSY) return false; // a new request next pass
            if (r.state == UPDATE_STATE_IDLE) {
                relay_fail(UPDATE_UNAVAILABLE); // it lost the session: rebooted, or timed out
                return false;
            }
            relay_fail(r.status == UPDATE_OK ? UPDATE_INVALID : (update_status_t)r.status);
            return false;
        }

        case RELAY_END:
            if (!rl.pending && !relay_new(USPLIT_OP_END)) break;
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_ACCEPTED) break;
            rl.phase   = RELAY_VERIFY_WAIT;
            rl.next_ms = now + SVAL_UPDATE_RELAY_POLL_MS;
            return false;

        case RELAY_COMMIT:
            // The request was built by update_relay_commit(); a retry keeps it.
            if (!relay_exchange(&r, buf)) return false;
            if (r.status != UPDATE_ACCEPTED) break;
            // The other half commits and resets. The link stays paused so this
            // half does not stall on a silent link (R18).
            rl.phase         = RELAY_HOLD;
            rl.hold_until_ms = now + SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS;
            return false;

        case RELAY_HOLD: {
            if (!due(now, rl.hold_until_ms)) return false;
            // One probe. A half that reset into its new image answers IDLE
            // with no error, or (with a new split table) not at all; one that
            // refused the commit is still in ERROR, or dropped it by timeout.
            rl.phase = RELAY_PROBE;
            if (!relay_new(USPLIT_OP_STATUS)) break;
            uint8_t want = usplit_rsp_len(USPLIT_OP_STATUS);
            memset(buf, 0, sizeof(buf));
            bool answered = update_split_port_rpc(rl.req, rl.req_len, buf, want) && usplit_rsp_parse(buf, want, USPLIT_OP_STATUS, rl.req[1], &r) == UPDATE_OK;
            rl.pending = false;
            if (answered) {
                rl.slave_state = r.state;
                relay_status_payload(&r);
                bool failed = r.state == UPDATE_STATE_ERROR || (r.state == UPDATE_STATE_IDLE && r.payload[0] != UPDATE_OK) || (r.state != UPDATE_STATE_IDLE && r.state != UPDATE_STATE_COMMITTING);
                if (failed) {
                    relay_lost();
                    return false;
                }
            }
            rl.phase  = RELAY_DONE;
            rl.end_ms = update_split_port_now_ms();
            relay_resume();
            return false;
        }

        default:
            return false;
    }
    // A refusal: the other half's status, as the relay's error.
    relay_fail(r.status != UPDATE_OK && r.status != UPDATE_ACCEPTED ? (update_status_t)r.status : UPDATE_INVALID);
    return false;
}

bool update_relay_start(const uint8_t *signed_manifest, uint32_t image_len, uint32_t crc_body) {
    if (update_relay_busy()) return false;
    relay_resume();
    memset(&rl, 0, sizeof(rl));
    memcpy(rl.manifest, signed_manifest, sizeof(rl.manifest));
    rl.image_len = image_len;
    rl.crc_body  = crc_body;
    rl.page_off  = UINT32_MAX;
    rl.start_ms = rl.last_ok_ms = update_split_port_now_ms();
    // Paused from the start: the other half's signature check and erase come
    // straight after the manifest.
    if (!relay_pause()) return false;
    rl.phase = RELAY_START;
    return true;
}

void update_relay_task(void) {
    if (!update_relay_busy()) return;
    uint32_t t0 = update_split_port_now_ms();
    while (relay_step() && update_relay_busy() && update_split_port_now_ms() - t0 < SVAL_UPDATE_RELAY_PASS_MS) {
    }
}

bool update_relay_commit(void) {
    if (rl.phase != RELAY_VERIFIED) return false;
    if (!relay_pause()) return false;
    // A keepalive still waiting for its answer is dropped: COMMIT replaces it.
    if (!relay_request(usplit_req_commit(rl.req, ++master_seq, (uint16_t)(rl.crc_body & 0xFFFF)), 0)) {
        relay_resume();
        return false;
    }
    rl.phase = RELAY_COMMIT;
    return true;
}

void update_relay_abort(void) {
    if (rl.phase == RELAY_COMMIT || rl.phase == RELAY_HOLD || rl.phase == RELAY_PROBE) return; // COMMIT is out
    if (update_relay_busy()) {
        relay_send_abort();
        rl.phase   = RELAY_IDLE;
        rl.pending = false;
    }
    relay_resume(); // a finished relay (DONE, FAILED) keeps its record for the RELAY op
}

void update_relay_info(update_relay_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->phase          = rl.phase;
    info->error          = rl.error;
    info->slave_state    = rl.slave_state;
    info->slave_error    = rl.slave_error;
    info->acked          = rl.acked;
    info->image_len      = rl.image_len;
    info->slave_erased   = rl.slave_erased;
    info->slave_to_erase = rl.slave_to_erase;
    info->retries        = rl.retries;
    info->elapsed_ms     = rl.phase == RELAY_IDLE ? 0 : (update_relay_busy() ? update_split_port_now_ms() : rl.end_ms) - rl.start_ms;
    info->paused         = rl.paused;
}

update_status_t update_split_other_info(uint8_t payload[USPLIT_INFO_PAYLOAD], uint8_t *state) {
    if (update_relay_busy()) return UPDATE_BUSY; // its requests and the other half's retry cache stay its own
    uint8_t      req[USPLIT_MSG_MAX], buf[USPLIT_MSG_MAX];
    usplit_rsp_t r;
    uint8_t      len  = usplit_req_build(req, USPLIT_OP_INFO, ++master_seq, NULL, 0);
    uint8_t      want = usplit_rsp_len(USPLIT_OP_INFO);
    for (int i = 0; i < 2; i++) {
        memset(buf, 0, sizeof(buf));
        if (update_split_port_rpc(req, len, buf, want) && usplit_rsp_parse(buf, want, USPLIT_OP_INFO, req[1], &r) == UPDATE_OK && r.status == UPDATE_OK) {
            memcpy(payload, r.payload, USPLIT_INFO_PAYLOAD);
            *state = r.state;
            return UPDATE_OK;
        }
    }
    return UPDATE_UNAVAILABLE;
}

#ifdef SVAL_UPDATER_HOST_TEST
void update_split_host_reset(void) {
    memset(&sl, 0, sizeof(sl));
    memset(&hk, 0, sizeof(hk));
    memset(&rl, 0, sizeof(rl));
    master_seq = 0;
    presence   = UPDATE_PRESENCE_NONE;
    memset(&presence_last, 0, sizeof(presence_last));
}

// The other half's RAM after a reset: its session and its retry cache gone.
void update_split_host_slave_reset(void) {
    memset(&sl, 0, sizeof(sl));
    memset(&hk, 0, sizeof(hk));
}
#else

// ---- firmware ------------------------------------------------------------------------

#    include "quantum.h"
#    include "timer.h"
#    include "split_common/transactions.h"
#    include "synchronization_util.h"
#    include "version.h" // QMK_GIT_HASH
#    include "split_pause.h"
#    include "updater.h"

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

uint32_t update_split_port_now_ms(void) {
    return timer_read32();
}

void update_split_port_lock(void) {
    split_shared_memory_lock();
}

void update_split_port_unlock(void) {
    split_shared_memory_unlock();
}

bool update_split_port_rpc(const uint8_t *req, uint8_t req_len, uint8_t *rsp, uint8_t rsp_len) {
    return transaction_rpc_exec(KEYBOARD_UPDATE, req_len, req, rsp_len, rsp);
}

bool update_split_port_pause(bool on) {
    return sval_split_pause(on);
}

bool update_split_port_slot_read(uint32_t off, uint8_t *dst, uint32_t n) {
    return slot_copy(off, dst, n);
}
#endif
