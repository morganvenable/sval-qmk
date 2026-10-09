// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// In-firmware updater: the session state machine (see updater.h).
//
// ---- host protocol: VIA custom values on UPDATE_CHANNEL (0x55, 'U') ----------
//
// Packet: [0xDD][client_id:4][0xFE] + VIA [command][0x55][op][value_data:23],
// command = id_custom_set_value or id_custom_get_value. The reply is the same
// packet with value_data rewritten; value_data[0] is always an update_status_t.
// Every op but INFO needs the client wrapper (UPDATE_INVALID otherwise, with no
// state change), and every request starts with [hand]: this half's
// (UPDATE_HAND_*), or the other half's (M2: see "The other half" below), else
// UPDATE_UNSUPPORTED. INFO also takes hand 0xFF for "this half". Multi-byte
// fields are little-endian.
//
//  op           request after [hand]             reply after [status]
//  0x00 INFO     -                               state, protocol, slot base/4K u16, slot size/4K u16,
//                                                max image/4K u16, JEDEC[3], pointing_id, hand,
//                                                fw_version u32, storage format, security epoch u16,
//                                                flags (bit0 die is 16 MiB, bit1 settings writes failing,
//                                                bit2 release build, bit3 test hooks, bit4 the TEST-ONLY
//                                                key is accepted, bit5 the other half's version
//                                                differs: see STATUS), last error.
//                                                Status UNAVAILABLE when bit0 is clear or bit1 set.
//  0x01 MANIFEST off u8, n 1..20, bytes          filled u8 (of 172: manifest, then signature)
//  0x02 ARM      -                               nonce u32, first 4 bytes of sha512(108 B manifest)
//  0x03 BEGIN    nonce u32                       state
//  0x04 CHUNK    off u24, n 1..18, bytes         next offset u24
//  0x05 END      nonce u32                       state
//  0x06 STATUS   -                               state, staged u24, image_len u24, sectors erased u16,
//                                                sectors to erase u16, last error, signature ms u16,
//                                                image verify ms u16, longest sector erase ms u16,
//                                                image CRC low 16 bits (once VERIFIED), flags (bit0
//                                                chord made, bit1 session bound, bit2 settings writes
//                                                failing), the other half by split presence
//                                                (update_presence_status_t; M2, V)
//  0x07 COMMIT   nonce u32, CRC low 16 bits      state
//  0x08 ABORT    nonce u32                       state
//  0x09 REBIND   nonce u32                       state
//  0x0A TEST_HALT nonce u32, point u8, fed u8    state (SVAL_UPDATE_TEST_HOOKS builds only; INVALID
//                                                elsewhere): the halt point for this session's commit
//                                                (update_halt_t, update_commit.h), in VERIFIED only.
//  0x0B DIAG     clear u8                        main's stack: free bytes u16 (never used since boot),
//                                                size u16; longest updater_task() pass ms u16 and the
//                                                state it began in u8. clear 1 restarts the pass
//                                                maximum after replying. Any wrapped client (M1 #13).
//  0x0C RELAY    -                               M2: the relay to the other half (update_split.h): phase
//                                                (relay_phase_t), the other half's state and last error,
//                                                bytes it acknowledged u24, image_len u24, its sectors
//                                                erased u16 and to erase u16, retries u16, relay ms u32,
//                                                flags (bit0 the link is paused), the relay's error, this
//                                                half's session state. Any hand.
//
// The image CRC is CRC-32/MPEG-2 over image bytes [0x100, image_len): the
// range the commit checks after copying (page 0 is written last).
//
// The other half (M2, store and forward, D14). With hand = the other side,
// MANIFEST starts a session for the other half, on this half (the one with
// USB, where the chord is made, D4). ARM checks the manifest against the
// other half's side and pointing device by split presence (which must answer
// MATCH or VERSION, else UNAVAILABLE) and this build's floors; BEGIN, CHUNK and
// END stage and verify the image in this half's slot exactly as for this half.
// Then:
//   COMMIT in VERIFIED  -> RELAYING: the image goes to the other half from this
//                          slot, which checks the signature and hash itself (D7)
//   (relay done)        -> RELAYED: the other half holds the image, verified
//   COMMIT in RELAYED   -> SUBSIDE_COMMITTING: the other half commits; the link
//                          stays paused through its reset (R18); then IDLE
// Both COMMITs carry the nonce and the image CRC. ABORT stops a relay up to the
// second COMMIT. A relay failure latches ERROR with its status (UNAVAILABLE:
// the other half stopped answering). While a session for one hand runs, the
// session ops for the other hand get BUSY. INFO with the other hand asks the
// other half (KEYBOARD_UPDATE INFO): the reply has INFO's layout with that
// half's state, protocol, pointing ID, hand, fw version, storage format, epoch
// and flags bits 0-4, no JEDEC ID (zeros) and no last error; with no answer it
// is UNAVAILABLE with state 0xFF (as Scan Lab's relay). This half never
// commits an image for the other hand: the session never reaches COMMITTING,
// and the commit's step 0 would refuse its hand anyway.
//
// Sessions. MANIFEST from any wrapped client starts loading and ties the load
// to that client. ARM, from the same client with all 172 bytes, checks the
// manifest's fields, binds the session to that client ID and a fresh nonce
// (D3) and waits for the chord. From then on BEGIN, END, COMMIT and ABORT need
// the client ID and the nonce, CHUNK the client ID; another client gets
// OTHER_CLIENT and changes nothing. REBIND moves the session to the sender's
// client ID when the nonce matches. While ERROR is latched anyone wrapped may
// ABORT; ABORT never erases.
//
// The binding keeps well-behaved clients apart; it is not a defence against a
// hostile one (R10). The client ID travels in every wrapped reply and the nonce
// in ARM's, and raw HID replies reach every open handle, so another process
// with the device open can REBIND the session to itself and then stall, ABORT
// or COMMIT it. It still cannot change what is installed: the chord confirms
// the manifest hash, and the signature and SHA-512 bind the image.
//
// Refusals of a request (INVALID, OTHER_CLIENT, BUSY, OUT_OF_ORDER, a chord not
// made yet, a wrong COMMIT CRC, UNAVAILABLE at ARM/BEGIN/COMMIT, ARM during a
// Scan Lab sweep) change no state. Failures of the update itself latch ERROR with last_error set: a
// manifest or signature check, the image hash or structure, a flash error,
// OVERRUN, the chord window passing (NOT_CONFIRMED), the session timeout
// (TIMEOUT), or a refused commit.

#include <string.h>
#include "updater.h"
#include "updater_port.h"
#include "update_flash.h"
#include "update_image.h"
#include "update_keys.h"
#include "update_gesture.h"
#include "update_led.h"
#include "update_commit.h"
#include "update_split.h"
#include "optional/monocypher-ed25519.h"

#ifdef SVAL_UPDATER_HOST_TEST
enum { id_custom_set_value = 0x07, id_custom_get_value = 0x08 };
#else
#    include "via.h"
#endif

#define VALUE_BYTES 23 // value_data that survives the client wrapper: 32 - 6 - 3
#define HAND_SELF 0xFF // INFO only
#define MANIFEST_DATA_MAX 20
#define CHUNK_DATA_MAX 18
#define VERIFY_SLICE 2048   // image bytes hashed per updater_task() pass
#define COMMIT_DELAY_MS 100 // so the COMMIT reply reaches the host first (as scanlab.c's reboot)
#define CHORD_FLASH_MS 300  // white double flash after the chord (D4)

typedef struct {
    update_state_t  state;
    update_status_t last_error;
    bool            bound;  // ARM issued a nonce
    uint32_t        client; // the session's client ID; before ARM, the manifest loader's
    uint32_t        nonce;
    uint32_t        op_ms;    // last op from the session (SVAL_UPDATE_SESSION_TIMEOUT_MS)
    uint32_t        armed_ms; // ARM (SVAL_UPDATE_CONFIRM_WINDOW_MS)
    uint8_t         filled;   // bytes of signed_manifest received
    uint32_t        image_len;
    uint32_t        erase_off, erase_end;
    uint32_t        staged; // image bytes received; whole pages of them are in the slot
    uint32_t        verify_off;
    uint32_t        crc_body;
    uint32_t        work_ms; // start of image verify
    uint16_t        sig_ms, verify_ms, erase_ms_max;
    uint32_t        commit_due_ms;
    bool            relay; // a session for the other half (M2)
    update_device_t dev;   // relay: the other half, as ARM checked the manifest against it
} session_t;

static session_t         s;
static uint32_t          now_ms;
static uint8_t           signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES] __attribute__((aligned(4)));
static uint8_t           page[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
static crypto_sha512_ctx sha;
static uint16_t          pass_max_ms; // DIAG: the longest updater_task() pass since boot or the last clear
static uint8_t           pass_max_state;

#define MANIFEST ((const sval_update_manifest_t *)signed_manifest)

// ---- helpers ------------------------------------------------------------------------

static uint16_t get16(const uint8_t *p) {
    return p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t get24(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t get32(const uint8_t *p) {
    return get24(p) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static void put24(uint8_t *p, uint32_t v) {
    put16(p, v);
    p[2] = (v >> 16) & 0xFF;
}
static void put32(uint8_t *p, uint32_t v) {
    put24(p, v);
    p[3] = (v >> 24) & 0xFF;
}
static uint16_t ms16(uint32_t ms) {
    return ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
}

// The other half answers presence and is not the same release (V).
static bool split_mismatch(void) {
    uint8_t p = updater_port_split_presence();
    return p == UPDATE_PRESENCE_VERSION || p == UPDATE_PRESENCE_SAME_HAND || p == UPDATE_PRESENCE_INVALID;
}

static uint8_t self_hand(void) {
    update_device_t dev;
    update_device_self(&dev);
    return dev.hand;
}

// A relay to the other half stops with its session; once its COMMIT is out
// the relay runs on by itself to the end of the hold (update_relay_abort).
static void stop_relay(void) {
    if (s.relay) update_relay_abort();
}

static void reset_session(void) {
    stop_relay();
    update_gesture_disarm();
#ifdef SVAL_UPDATE_TEST_HOOKS
    update_commit_test_halt(UPDATE_HALT_NONE, false);
#endif
    memset(&s, 0, sizeof(s)); // IDLE, last_error OK
    memset(signed_manifest, 0, sizeof(signed_manifest));
    memset(page, 0, sizeof(page));
}

static update_status_t fail(update_status_t status) {
    stop_relay();
    update_gesture_disarm();
    s.state      = UPDATE_STATE_ERROR;
    s.last_error = status;
    return status;
}

// The other half, for a session with its hand: its side and pointing device
// by split presence, this build's floors (both halves run the same release,
// V; the other half checks the manifest again itself, D7). Only a half that
// answers presence as MATCH or VERSION can be updated through this one.
static update_status_t other_device(update_device_t *dev) {
    update_presence_t p;
    uint8_t           st = update_split_other(&p);
    if (st != UPDATE_PRESENCE_MATCH && st != UPDATE_PRESENCE_VERSION) return UPDATE_UNAVAILABLE;
    update_device_self(dev);
    if (p.hand == dev->hand) return UPDATE_UNAVAILABLE;
    dev->hand        = p.hand;
    dev->pointing_id = p.pointing_id;
    return UPDATE_OK;
}

// The bound session, by client ID and (when given) nonce.
static update_status_t session(uint32_t client, const uint8_t *nonce) {
    if (!s.bound) return UPDATE_INVALID;
    if (client != s.client || (nonce && get32(nonce) != s.nonce)) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    return UPDATE_OK;
}

// Uncached slot bytes into RAM, a word at a time (off and n multiples of 4).
static bool slot_copy(uint32_t off, uint8_t *dst, uint32_t n) {
    const volatile uint32_t *p = (const volatile uint32_t *)update_slot_read(off, n);
    if (!p) return false;
    for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)dst)[i] = p[i];
    return true;
}

// ---- ops ------------------------------------------------------------------------------

static update_status_t op_info(uint8_t *rsp) {
    update_device_t dev;
    uint8_t         jedec[UPDATE_JEDEC_BYTES];
    update_device_self(&dev);
    update_flash_jedec(jedec);
    bool die_ok  = update_flash_available();
    bool failing = updater_port_settings_failing();
    rsp[1]       = s.state;
    rsp[2]       = UPDATE_PROTOCOL_VERSION;
    put16(&rsp[3], SVAL_UPDATE_BASE / UPDATE_FLASH_SECTOR);
    put16(&rsp[5], SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    put16(&rsp[7], SVAL_UPDATE_MAX_IMAGE / UPDATE_FLASH_SECTOR);
    memcpy(&rsp[9], jedec, UPDATE_JEDEC_BYTES);
    rsp[12] = dev.pointing_id;
    rsp[13] = dev.hand;
    put32(&rsp[14], SVAL_FW_VERSION);
    rsp[18] = dev.storage_format;
    put16(&rsp[19], dev.security_epoch);
    rsp[21] = (die_ok ? 0x01 : 0) | (failing ? 0x02 : 0) | (dev.release_build ? 0x04 : 0) | (update_key(UPDATE_KEY_TEST) ? 0x10 : 0) | (split_mismatch() ? 0x20 : 0);
#ifdef SVAL_UPDATE_TEST_HOOKS
    rsp[21] |= 0x08;
#endif
    rsp[22] = s.last_error;
    return die_ok && !failing ? UPDATE_OK : UPDATE_UNAVAILABLE;
}

// INFO for the other half, from it over KEYBOARD_UPDATE (see the top of this file).
static update_status_t op_info_other(uint8_t *rsp) {
    uint8_t         pl[USPLIT_INFO_PAYLOAD], state = 0;
    update_status_t st = update_split_other_info(pl, &state);
    if (st != UPDATE_OK) {
        rsp[1] = 0xFF; // no answer
        return st;
    }
    rsp[1] = state;
    rsp[2] = pl[0];
    put16(&rsp[3], SVAL_UPDATE_BASE / UPDATE_FLASH_SECTOR); // the same release on both halves (V)
    put16(&rsp[5], SVAL_UPDATE_SIZE / UPDATE_FLASH_SECTOR);
    put16(&rsp[7], SVAL_UPDATE_MAX_IMAGE / UPDATE_FLASH_SECTOR);
    rsp[12] = pl[2];
    rsp[13] = pl[1];
    memcpy(&rsp[14], &pl[3], 4);
    rsp[18] = pl[11];
    memcpy(&rsp[19], &pl[12], 2);
    rsp[21] = (pl[14] & 0x1F) | (split_mismatch() ? 0x20 : 0);
    return (pl[14] & 0x01) && !(pl[14] & 0x02) ? UPDATE_OK : UPDATE_UNAVAILABLE;
}

static update_status_t manifest_load(uint32_t client, const uint8_t *req, bool to_other) {
    uint8_t off = req[1], n = req[2];
    if (s.state != UPDATE_STATE_IDLE && s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (s.state == UPDATE_STATE_MANIFEST_LOADING && client != s.client) return UPDATE_OTHER_CLIENT;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    if (to_other) {
        update_device_t dev;
        if (other_device(&dev) != UPDATE_OK) return UPDATE_UNAVAILABLE; // the other half must answer presence
    }
    if (n == 0 || n > MANIFEST_DATA_MAX || off + n > UPDATE_SIGNED_MANIFEST_BYTES) return UPDATE_INVALID;
    // In order; offset 0 starts the load again.
    if (off != 0 && (s.state == UPDATE_STATE_IDLE || off != s.filled)) return UPDATE_OUT_OF_ORDER;
    if (s.state == UPDATE_STATE_IDLE) {
        reset_session();
        s.state  = UPDATE_STATE_MANIFEST_LOADING;
        s.client = client;
        s.relay  = to_other;
    }
    memcpy(signed_manifest + off, &req[3], n);
    s.filled = off + n;
    s.op_ms  = now_ms;
    return UPDATE_OK;
}

static update_status_t op_manifest(uint32_t client, const uint8_t *req, uint8_t *rsp, bool to_other) {
    update_status_t st = manifest_load(client, req, to_other);
    rsp[1]             = s.state == UPDATE_STATE_MANIFEST_LOADING ? s.filled : 0;
    return st;
}

static update_status_t op_arm(uint32_t client, uint8_t *rsp) {
    if (s.state == UPDATE_STATE_IDLE) return UPDATE_INVALID;
    if (s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (client != s.client) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    if (s.filled != UPDATE_SIGNED_MANIFEST_BYTES) return UPDATE_INVALID;
    if (!update_flash_available() || updater_port_settings_failing()) return UPDATE_UNAVAILABLE; // D26, D12
    // A Scan Lab sweep sets the scan timing from the host; the chord is only
    // read from normally scanned frames (and scanlab.c refuses to start a
    // sweep while the updater is active).
    if (updater_port_scan_override()) return UPDATE_BUSY;
    // The fields now, so the user is never asked to approve an image this half
    // would refuse; the signature waits for BEGIN (it is the slow part).
    update_device_t dev;
    if (s.relay) {
        if (other_device(&dev) != UPDATE_OK) return UPDATE_UNAVAILABLE; // refused: nothing changes
    } else {
        update_device_self(&dev);
    }
    update_status_t st = update_manifest_check(MANIFEST, &dev);
    if (st != UPDATE_OK) return fail(st);

    s.dev       = dev;
    s.image_len = MANIFEST->image_len;
    s.nonce     = updater_port_random32();
    s.bound     = true;
    s.armed_ms  = now_ms;
    s.state     = UPDATE_STATE_CONFIRM_WAIT;
    update_gesture_arm();
    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512(hash, signed_manifest, UPDATE_MANIFEST_BYTES); // the host shows these bytes (D5)
    put32(&rsp[1], s.nonce);
    memcpy(&rsp[5], hash, 4);
    return UPDATE_OK;
}

static update_status_t op_begin(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state != UPDATE_STATE_CONFIRM_WAIT) return UPDATE_INVALID;
    if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;
    if (!update_gesture_done(NULL)) return UPDATE_NOT_CONFIRMED;
    update_gesture_disarm();
    s.state = UPDATE_STATE_VERIFYING_MANIFEST;
    return UPDATE_ACCEPTED;
}

static update_status_t chunk(uint32_t client, const uint8_t *req) {
    if (!s.bound) return UPDATE_INVALID;
    if (client != s.client) return UPDATE_OTHER_CLIENT;
    s.op_ms = now_ms;
    if (s.state == UPDATE_STATE_VERIFYING_MANIFEST || s.state == UPDATE_STATE_ERASING) return UPDATE_BUSY;
    if (s.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
    uint32_t       off  = get24(&req[1]);
    uint8_t        n    = req[4];
    const uint8_t *data = &req[5];
    if (n == 0 || n > CHUNK_DATA_MAX) return UPDATE_INVALID;
    if (off < s.staged) return off + n <= s.staged ? UPDATE_OK : UPDATE_OUT_OF_ORDER; // a repeat: acknowledged, not rewritten
    if (off != s.staged) return UPDATE_OUT_OF_ORDER;
    if (n > s.image_len - s.staged) return fail(UPDATE_OVERRUN);
    while (n) {
        uint32_t at   = s.staged % UPDATE_FLASH_PAGE;
        uint32_t take = UPDATE_FLASH_PAGE - at < n ? UPDATE_FLASH_PAGE - at : n;
        memcpy(page + at, data, take);
        s.staged += take;
        data += take;
        n -= take;
        if (s.staged % UPDATE_FLASH_PAGE == 0) {
            // image_len is whole pages, so the last chunk always lands here.
            if (update_flash_program_page(s.staged - UPDATE_FLASH_PAGE, page) != UPDATE_OK) return fail(UPDATE_FLASH_ERR);
        }
    }
    return UPDATE_OK;
}

static update_status_t op_chunk(uint32_t client, const uint8_t *req, uint8_t *rsp) {
    update_status_t st = chunk(client, req);
    put24(&rsp[1], s.staged);
    return st;
}

static update_status_t op_end(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state == UPDATE_STATE_VERIFYING_MANIFEST || s.state == UPDATE_STATE_ERASING) return UPDATE_BUSY;
    if (s.state != UPDATE_STATE_RECEIVING) return UPDATE_INVALID;
    if (s.staged != s.image_len) return UPDATE_OUT_OF_ORDER; // END before the last byte
    crypto_sha512_init(&sha);
    s.verify_off = 0;
    s.crc_body   = UPDATE_CRC32_INIT;
    s.work_ms    = now_ms;
    s.state      = UPDATE_STATE_VERIFYING_IMAGE;
    return UPDATE_ACCEPTED;
}

static update_status_t op_status(uint32_t client, uint8_t *rsp) {
    if (s.state != UPDATE_STATE_IDLE && client == s.client) s.op_ms = now_ms;
    rsp[1] = s.state;
    put24(&rsp[2], s.staged);
    put24(&rsp[5], s.image_len);
    put16(&rsp[8], s.erase_off / UPDATE_FLASH_SECTOR);
    put16(&rsp[10], s.erase_end / UPDATE_FLASH_SECTOR);
    rsp[12] = s.last_error;
    put16(&rsp[13], s.sig_ms);
    put16(&rsp[15], s.verify_ms);
    put16(&rsp[17], s.erase_ms_max);
    put16(&rsp[19], s.state == UPDATE_STATE_VERIFIED || s.state == UPDATE_STATE_COMMITTING ? s.crc_body & 0xFFFF : 0);
    rsp[21] = (update_gesture_done(NULL) && s.bound ? 0x01 : 0) | (s.bound ? 0x02 : 0) | (updater_port_settings_failing() ? 0x04 : 0);
    rsp[22] = updater_port_split_presence();
    return UPDATE_OK;
}

// COMMIT in a session for the other half: the relay, then its commit.
static update_status_t relay_commit(const uint8_t *req) {
    if (s.state != UPDATE_STATE_VERIFIED && s.state != UPDATE_STATE_RELAYED) return UPDATE_INVALID;
    if (get16(&req[5]) != (s.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
    if (s.state == UPDATE_STATE_VERIFIED) {
        // Staged and verified here; now from this slot to the other half. The
        // link pauses at once; refused (still VERIFIED) if it is down.
        if (!update_relay_start(signed_manifest, s.image_len, s.crc_body)) return UPDATE_UNAVAILABLE;
        s.state = UPDATE_STATE_RELAYING;
        return UPDATE_ACCEPTED;
    }
    if (!update_relay_commit()) return UPDATE_UNAVAILABLE;
    s.state = UPDATE_STATE_SUBSIDE_COMMITTING;
    return UPDATE_ACCEPTED;
}

static update_status_t op_commit(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.relay) return relay_commit(req);
    if (s.state != UPDATE_STATE_VERIFIED) return UPDATE_INVALID;
    if (get16(&req[5]) != (s.crc_body & 0xFFFF)) return UPDATE_BAD_HASH; // not the image that was verified
    if (updater_port_settings_failing()) return UPDATE_UNAVAILABLE;
    if (!update_commit_available()) return UPDATE_UNSUPPORTED;
    s.state         = UPDATE_STATE_COMMITTING;
    s.commit_due_ms = now_ms + COMMIT_DELAY_MS;
    return UPDATE_ACCEPTED;
}

static update_status_t op_abort(uint32_t client, const uint8_t *req) {
    switch (s.state) {
        case UPDATE_STATE_IDLE:
            return UPDATE_OK;
        case UPDATE_STATE_ERROR:
            break; // anyone
        case UPDATE_STATE_MANIFEST_LOADING:
            if (client != s.client) return UPDATE_OTHER_CLIENT;
            break;
        default: {
            update_status_t st = session(client, &req[1]);
            if (st != UPDATE_OK) return st;
            break;
        }
    }
    reset_session(); // never erases: BEGIN erases anyway
    return UPDATE_OK;
}

#ifdef SVAL_UPDATE_TEST_HOOKS
static update_status_t op_test_halt(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
    if (s.state != UPDATE_STATE_VERIFIED || s.relay) return UPDATE_INVALID;
    return update_commit_test_halt(req[5], req[6] != 0) ? UPDATE_OK : UPDATE_INVALID;
}
#endif

// ---- reset breadcrumbs (M1 hardware debugging) -----------------------------------------------
// Watchdog scratch 0-3 survive every reset except power-on and the RUN pin
// (the boot ROM and the commit use only scratch 4-7). scratch0 marks them
// valid, scratch1 = last op << 24 | state << 16 | status << 8 | op count,
// scratch2 = ms of that op, scratch3 = ms of the last updater_task pass. At
// boot the previous values and the reset reason are latched for DIAG.
#ifndef SVAL_UPDATER_HOST_TEST
#    include "hardware/structs/watchdog.h"
#    include "hardware/structs/vreg_and_chip_reset.h"
#    define CRUMB_MAGIC 0x5C0B0001u
static bool     crumbs_latched;
static uint8_t  reset_flags; // bit0 POR/brown-out, 1 RUN pin, 2 debugger, 3 watchdog timer, 4 watchdog force, 7 crumbs valid
static uint32_t prev_crumb[3];
static uint8_t  crumb_count;

static void crumbs_latch(void) {
    if (crumbs_latched) return;
    crumbs_latched = true;
    uint32_t cr = vreg_and_chip_reset_hw->chip_reset, wr = watchdog_hw->reason;
    reset_flags = (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS ? 0x01 : 0) | (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS ? 0x02 : 0) | (cr & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS ? 0x04 : 0) | (wr & WATCHDOG_REASON_TIMER_BITS ? 0x08 : 0) | (wr & WATCHDOG_REASON_FORCE_BITS ? 0x10 : 0);
    if (watchdog_hw->scratch[0] == CRUMB_MAGIC) {
        reset_flags |= 0x80;
        for (int i = 0; i < 3; i++) prev_crumb[i] = watchdog_hw->scratch[i + 1];
    }
    watchdog_hw->scratch[1] = watchdog_hw->scratch[2] = watchdog_hw->scratch[3] = 0;
    watchdog_hw->scratch[0] = CRUMB_MAGIC;
}

static void crumb_op(uint8_t op, uint8_t st) {
    crumbs_latch();
    watchdog_hw->scratch[1] = (uint32_t)op << 24 | (uint32_t)s.state << 16 | (uint32_t)st << 8 | crumb_count++;
    watchdog_hw->scratch[2] = updater_port_now_ms();
}

static void crumb_pass(void) {
    crumbs_latch();
    watchdog_hw->scratch[3] = updater_port_now_ms();
}

static void crumbs_report(uint8_t *rsp) {
    crumbs_latch();
    rsp[8] = reset_flags;
    for (int i = 0; i < 3; i++) put32(&rsp[9 + 4 * i], prev_crumb[i]);
}
#else
#    define crumb_op(op, st) ((void)0)
#    define crumb_pass() ((void)0)
#    define crumbs_report(rsp) ((void)0)
#endif

static update_status_t op_diag(const uint8_t *req, uint8_t *rsp) {
    uint16_t size   = 0;
    uint16_t unused = updater_port_stack_free(&size);
    put16(&rsp[1], unused);
    put16(&rsp[3], size);
    put16(&rsp[5], pass_max_ms);
    rsp[7] = pass_max_state;
    crumbs_report(rsp);
    if (req[1] == 1) {
        pass_max_ms    = 0;
        pass_max_state = 0;
    }
    return UPDATE_OK;
}

static update_status_t op_relay(uint32_t client, uint8_t *rsp) {
    update_relay_info_t ri;
    update_relay_info(&ri);
    if (s.state != UPDATE_STATE_IDLE && client == s.client) s.op_ms = now_ms;
    rsp[1] = ri.phase;
    rsp[2] = ri.slave_state;
    rsp[3] = ri.slave_error;
    put24(&rsp[4], ri.acked);
    put24(&rsp[7], ri.image_len);
    put16(&rsp[10], ri.slave_erased);
    put16(&rsp[12], ri.slave_to_erase);
    put16(&rsp[14], ri.retries);
    put32(&rsp[16], ri.elapsed_ms);
    rsp[20] = ri.paused ? 0x01 : 0;
    rsp[21] = ri.error;
    rsp[22] = s.state;
    return UPDATE_OK;
}

// The ops that act on a session for one hand (MANIFEST starts one).
static bool session_op(uint8_t op) {
    return op == UPDATE_OP_MANIFEST || op == UPDATE_OP_ARM || op == UPDATE_OP_BEGIN || op == UPDATE_OP_CHUNK || op == UPDATE_OP_END || op == UPDATE_OP_COMMIT || op == UPDATE_OP_TEST_HALT;
}

static update_status_t op_rebind(uint32_t client, const uint8_t *req) {
    if (!s.bound) return UPDATE_INVALID;
    if (get32(&req[1]) != s.nonce) return UPDATE_OTHER_CLIENT;
    s.client = client; // the old ID is no longer accepted
    s.op_ms  = now_ms;
    return UPDATE_OK;
}

void updater_via_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd = data[0], op = data[2];
    uint8_t *value = &data[3];
    if (cmd != id_custom_set_value && cmd != id_custom_get_value) return; // id_custom_save: nothing here
    if (length < 3 + VALUE_BYTES) {
        if (length > 3) value[0] = UPDATE_INVALID;
        return;
    }
    uint8_t req[VALUE_BYTES], rsp[VALUE_BYTES];
    memcpy(req, value, VALUE_BYTES);
    memset(rsp, 0, VALUE_BYTES);
    now_ms = updater_port_now_ms();

    uint32_t        client  = 0;
    bool            wrapped = updater_port_client(&client);
    update_status_t st;
#ifdef SVAL_UPDATE_TEST_HOOKS
    const bool test_halt_op = false;
#else
    const bool test_halt_op = op == UPDATE_OP_TEST_HALT; // not in this build
#endif
    uint8_t me       = self_hand();
    bool    to_other = req[0] == (me ^ 1);
    if (op > UPDATE_OP_RELAY || test_halt_op) {
        st = UPDATE_INVALID;
    } else if (op != UPDATE_OP_INFO && !wrapped) {
        st = UPDATE_INVALID; // unwrapped VIA reaches here too (sval.c); only INFO is open
    } else if (req[0] != me && !to_other && !(op == UPDATE_OP_INFO && req[0] == HAND_SELF)) {
        st = UPDATE_UNSUPPORTED;
    } else if (to_other && (op == UPDATE_OP_TEST_HALT || op == UPDATE_OP_DIAG)) {
        st = UPDATE_UNSUPPORTED; // this half's only
    } else if ((s.state == UPDATE_STATE_COMMITTING || s.state == UPDATE_STATE_SUBSIDE_COMMITTING) && op != UPDATE_OP_INFO && op != UPDATE_OP_STATUS && op != UPDATE_OP_RELAY) {
        st = UPDATE_BUSY;
    } else if (s.state != UPDATE_STATE_IDLE && session_op(op) && to_other != s.relay) {
        st = UPDATE_BUSY; // a session for the other hand is running
    } else if (op == UPDATE_OP_INFO && to_other) {
        st = op_info_other(rsp); // M2: the other half answers
    } else {
        crumb_op(op, 0xFF); // 0xFF: inside the op
        switch (op) {
            case UPDATE_OP_INFO:
                st = op_info(rsp);
                break;
            case UPDATE_OP_MANIFEST:
                st = op_manifest(client, req, rsp, to_other);
                break;
            case UPDATE_OP_ARM:
                st = op_arm(client, rsp);
                break;
            case UPDATE_OP_BEGIN:
                st = op_begin(client, req);
                break;
            case UPDATE_OP_CHUNK:
                st = op_chunk(client, req, rsp);
                break;
            case UPDATE_OP_END:
                st = op_end(client, req);
                break;
            case UPDATE_OP_STATUS:
                st = op_status(client, rsp);
                break;
            case UPDATE_OP_COMMIT:
                st = op_commit(client, req);
                break;
            case UPDATE_OP_ABORT:
                st = op_abort(client, req);
                break;
#ifdef SVAL_UPDATE_TEST_HOOKS
            case UPDATE_OP_TEST_HALT:
                st = op_test_halt(client, req);
                break;
#endif
            case UPDATE_OP_DIAG:
                st = op_diag(req, rsp);
                break;
            case UPDATE_OP_RELAY:
                st = op_relay(client, rsp);
                break;
            default: // UPDATE_OP_REBIND
                st = op_rebind(client, req);
                break;
        }
        if (op == UPDATE_OP_BEGIN || op == UPDATE_OP_END || op == UPDATE_OP_COMMIT || op == UPDATE_OP_ABORT || op == UPDATE_OP_REBIND || op == UPDATE_OP_TEST_HALT) rsp[1] = s.state;
        crumb_op(op, st);
    }
    rsp[0] = st;
    memcpy(value, rsp, VALUE_BYTES);
}

// ---- slow work, one slice per pass ---------------------------------------------------------

static void verify_manifest(void) {
    update_device_t dev;
    if (s.relay) {
        dev = s.dev; // the other half, as ARM found it
    } else {
        update_device_self(&dev);
    }
    uint32_t        t0 = updater_port_now_ms();
    update_status_t st = update_manifest_check(MANIFEST, &dev);
    if (st == UPDATE_OK) st = update_manifest_signature(signed_manifest); // ~1.9 KiB of stack, one call
    s.sig_ms = ms16(updater_port_now_ms() - t0);
    if (st != UPDATE_OK) {
        fail(st);
        return;
    }
    // Erase [0, image_len) rounded up to whole sectors; sectors that already
    // read erased are skipped.
    s.erase_off = 0;
    s.erase_end = (s.image_len + UPDATE_FLASH_SECTOR - 1) / UPDATE_FLASH_SECTOR * UPDATE_FLASH_SECTOR;
    s.state     = UPDATE_STATE_ERASING;
}

static void erase_step(void) {
    if (s.erase_off < s.erase_end) {
        uint32_t        t0 = updater_port_now_ms();
        update_status_t st = update_flash_erase_sector(s.erase_off);
        uint16_t        ms = ms16(updater_port_now_ms() - t0);
        if (ms > s.erase_ms_max) s.erase_ms_max = ms;
        if (st != UPDATE_OK) {
            fail(st == UPDATE_UNAVAILABLE ? UPDATE_UNAVAILABLE : UPDATE_FLASH_ERR);
            return;
        }
        s.erase_off += UPDATE_FLASH_SECTOR;
    }
    if (s.erase_off >= s.erase_end) {
        s.staged = 0;
        s.state  = UPDATE_STATE_RECEIVING;
    }
}

static void verify_image_step(void) {
    uint8_t  buf[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
    uint32_t end = s.image_len - s.verify_off > VERIFY_SLICE ? s.verify_off + VERIFY_SLICE : s.image_len;
    while (s.verify_off < end) {
        uint32_t n = end - s.verify_off < sizeof(buf) ? end - s.verify_off : sizeof(buf);
        if (!slot_copy(s.verify_off, buf, n)) {
            fail(UPDATE_FLASH_ERR);
            return;
        }
        crypto_sha512_update(&sha, buf, n);
        uint32_t skip = s.verify_off < UPDATE_VECTORS_OFFSET ? UPDATE_VECTORS_OFFSET - s.verify_off : 0;
        if (skip < n) s.crc_body = update_crc32_mpeg2_update(s.crc_body, buf + skip, n - skip);
        s.verify_off += n;
    }
    if (s.verify_off < s.image_len) return;

    uint8_t hash[UPDATE_SHA512_BYTES];
    crypto_sha512_final(&sha, hash);
    update_status_t st = memcmp(hash, MANIFEST->sha512, UPDATE_SHA512_BYTES) == 0 ? UPDATE_OK : UPDATE_BAD_HASH;
    if (st == UPDATE_OK) {
        uint8_t head[UPDATE_HEAD_BYTES] __attribute__((aligned(4)));
        st = slot_copy(0, head, sizeof(head)) ? update_image_head_check(head, s.image_len) : UPDATE_FLASH_ERR;
    }
    s.verify_ms = ms16(updater_port_now_ms() - s.work_ms);
    if (st != UPDATE_OK) {
        fail(st);
        return;
    }
    s.state = UPDATE_STATE_VERIFIED;
}

static update_led_mode_t led_mode(void) {
    uint32_t done_ms;
    switch (s.state) {
        case UPDATE_STATE_IDLE:
            // The half without USB, in a session from the other half: the same
            // colours (D24); back to normal when it ends or times out.
            switch (update_split_slave_state()) {
                case UPDATE_STATE_IDLE:
                    break;
                case UPDATE_STATE_ERROR:
                    return UPDATE_LED_ERROR;
                case UPDATE_STATE_COMMITTING:
                    return UPDATE_LED_WRITING;
                default:
                    return UPDATE_LED_PROGRESS;
            }
            return split_mismatch() ? UPDATE_LED_ERROR : UPDATE_LED_NONE;
        case UPDATE_STATE_MANIFEST_LOADING:
            // A version mismatch with the other half shows the red error LED
            // on the half with USB until it is fixed (V).
            return split_mismatch() ? UPDATE_LED_ERROR : UPDATE_LED_NONE;
        case UPDATE_STATE_ERROR:
            return UPDATE_LED_ERROR;
        case UPDATE_STATE_COMMITTING:
        case UPDATE_STATE_SUBSIDE_COMMITTING:
            return UPDATE_LED_WRITING;
        case UPDATE_STATE_RELAYING:
        case UPDATE_STATE_RELAYED:
            return UPDATE_LED_PROGRESS;
        default:
            break;
    }
    if (update_gesture_done(&done_ms)) {
        if (now_ms - done_ms < CHORD_FLASH_MS) return UPDATE_LED_CHORD;
        if (s.state == UPDATE_STATE_CONFIRM_WAIT) return UPDATE_LED_APPROVED;
    } else if (s.state == UPDATE_STATE_CONFIRM_WAIT) {
        return UPDATE_LED_CONFIRM;
    }
    return UPDATE_LED_PROGRESS;
}

void updater_task(void) {
    now_ms                 = updater_port_now_ms();
    uint32_t       pass_t0 = now_ms;
    update_state_t pass_st = s.state;
    // The relay (M2) runs before the session looks at it; once its COMMIT is
    // out it runs to the end of its hold even if the session went away.
    update_relay_task();
    now_ms         = updater_port_now_ms();
    bool hostbound = (s.state >= UPDATE_STATE_MANIFEST_LOADING && s.state <= UPDATE_STATE_VERIFIED) || s.state == UPDATE_STATE_RELAYING || s.state == UPDATE_STATE_RELAYED;
    if (hostbound && now_ms - s.op_ms >= SVAL_UPDATE_SESSION_TIMEOUT_MS) {
        // A host that went away before the other half's COMMIT stops the relay (D23).
        // A half-sent manifest was never shown to the user: just drop it.
        if (s.state == UPDATE_STATE_MANIFEST_LOADING) {
            reset_session();
        } else {
            fail(UPDATE_TIMEOUT);
        }
    }
    if (s.state == UPDATE_STATE_CONFIRM_WAIT && !update_gesture_done(NULL) && now_ms - s.armed_ms >= SVAL_UPDATE_CONFIRM_WINDOW_MS) {
        fail(UPDATE_NOT_CONFIRMED);
    }

    switch (s.state) {
        case UPDATE_STATE_VERIFYING_MANIFEST:
            verify_manifest();
            break;
        case UPDATE_STATE_ERASING:
            erase_step();
            break;
        case UPDATE_STATE_VERIFYING_IMAGE:
            verify_image_step();
            break;
        case UPDATE_STATE_RELAYING:
        case UPDATE_STATE_RELAYED:
        case UPDATE_STATE_SUBSIDE_COMMITTING: {
            update_relay_info_t ri;
            update_relay_info(&ri);
            if (ri.phase == RELAY_FAILED) {
                fail(ri.error);
            } else if (s.state == UPDATE_STATE_RELAYING && ri.phase == RELAY_VERIFIED) {
                s.state = UPDATE_STATE_RELAYED;
            } else if (s.state == UPDATE_STATE_SUBSIDE_COMMITTING && ri.phase == RELAY_DONE) {
                reset_session(); // the other half took COMMIT and reset (or never answered again: V)
            } else if (ri.phase == RELAY_IDLE || ri.phase == RELAY_DONE) {
                fail(UPDATE_INVALID); // cannot happen: the relay ended outside its session
            }
            break;
        }
        case UPDATE_STATE_COMMITTING:
            if (s.relay) {
                fail(UPDATE_INVALID); // cannot happen: never this half's commit for the other hand's image
                break;
            }
            if ((int32_t)(now_ms - s.commit_due_ms) >= 0) {
                // Image bytes in RAM are cleared before the reset (commit step
                // 7, R23); these two are not needed any more.
                memset(page, 0, sizeof(page));
                memset(&sha, 0, sizeof(sha));
                // Returns only when the commit refused before touching the firmware.
                update_status_t st = update_commit_run(MANIFEST, s.crc_body);
                fail(st != UPDATE_OK ? st : UPDATE_FLASH_ERR);
            }
            break;
        default:
            break;
    }
    now_ms = updater_port_now_ms();
    // The 50 ms rule (R13) on the device: the host tests only see flash time.
    if (now_ms - pass_t0 > pass_max_ms) {
        pass_max_ms    = ms16(now_ms - pass_t0);
        pass_max_state = pass_st;
    }
    update_led_show(led_mode());
    crumb_pass();
}

bool updater_active(void) {
    return s.state != UPDATE_STATE_IDLE || update_split_slave_active() || update_relay_busy();
}

update_state_t updater_state(void) {
    return s.state;
}

#ifdef SVAL_UPDATER_HOST_TEST
void updater_host_reset(void) {
    reset_session();
}
#endif

// ---- firmware port (updater_port.h) ---------------------------------------------------------

#ifndef SVAL_UPDATER_HOST_TEST
#    include "timer.h"
#    include "wait.h"
#    include "client_wrapper.h"
#    include "scanlab.h"
#    include "hardware/structs/rosc.h"
#    ifdef EEPROM_WEAR_LEVELING
#        include "wear_leveling.h"
#    else
#        error "updater: settings must be on QMK wear leveling, whose write-failure latch gates ARM and COMMIT (D12)"
#    endif

uint32_t updater_port_now_ms(void) {
    return timer_read32();
}

bool updater_port_client(uint32_t *client_id) {
    if (!client_wrapper_in_via()) return false;
    *client_id = client_wrapper_current_id();
    return *client_id != CLIENT_ID_BOOTSTRAP;
}

bool updater_port_settings_failing(void) {
    return wear_leveling_write_failed();
}

// The ring oscillator, a bit per read as identity.c does, mixed with the timer.
uint32_t updater_port_random32(void) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < 32; i++) {
        v = (v << 1) | (rosc_hw->randombit & 1);
        wait_us(1);
    }
    v ^= timer_read32() * 2654435761u;
    return v ? v : 1;
}

bool updater_port_scan_override(void) {
    return scanlab_active();
}

uint8_t updater_port_split_presence(void) {
    return update_split_presence();
}

// crt0 (ChibiOS crt0_v6m.S, CRT0_INIT_STACKS on by default) fills the process
// stack with this before main(); main() runs on that stack. The count is a
// little optimistic if a used word happens to hold the pattern.
#    define STACK_FILL 0x55555555u
extern uint32_t __process_stack_base__[], __process_stack_end__[];

uint16_t updater_port_stack_free(uint16_t *size) {
    const volatile uint32_t *p = __process_stack_base__;
    while (p < __process_stack_end__ && *p == STACK_FILL) p++;
    *size = (uint16_t)((uintptr_t)__process_stack_end__ - (uintptr_t)__process_stack_base__);
    return (uint16_t)((uintptr_t)p - (uintptr_t)__process_stack_base__);
}
#endif
