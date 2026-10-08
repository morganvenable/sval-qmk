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
// (UPDATE_HAND_*), else UPDATE_UNSUPPORTED, since M1 has no split relay. INFO
// also takes hand 0xFF for "this half". Multi-byte fields are little-endian.
//
//  op           request after [hand]             reply after [status]
//  0x00 INFO     -                               state, protocol, slot base/4K u16, slot size/4K u16,
//                                                max image/4K u16, JEDEC[3], pointing_id, hand,
//                                                fw_version u32, storage format, security epoch u16,
//                                                flags (bit0 die is 16 MiB, bit1 settings writes failing,
//                                                bit2 release build, bit3 test hooks), last error.
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
//                                                failing), 0
//  0x07 COMMIT   nonce u32, CRC low 16 bits      state
//  0x08 ABORT    nonce u32                       state
//  0x09 REBIND   nonce u32                       state
//
// The image CRC is CRC-32/MPEG-2 over image bytes [0x100, image_len): the
// range the commit checks after copying (page 0 is written last).
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
// Refusals of a request (INVALID, OTHER_CLIENT, BUSY, OUT_OF_ORDER, a chord not
// made yet, a wrong COMMIT CRC, UNAVAILABLE at ARM/BEGIN/COMMIT) change no
// state. Failures of the update itself latch ERROR with last_error set: a
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
#include "optional/monocypher-ed25519.h"

#ifdef SVAL_UPDATER_HOST_TEST
enum { id_custom_set_value = 0x07, id_custom_get_value = 0x08 };
#else
#    include "via.h"
#endif

#ifndef SVAL_FW_VERSION
#    define SVAL_FW_VERSION 0 // numeric release version: M3 sets it from the tag (D17)
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
} session_t;

static session_t         s;
static uint32_t          now_ms;
static uint8_t           signed_manifest[UPDATE_SIGNED_MANIFEST_BYTES] __attribute__((aligned(4)));
static uint8_t           page[UPDATE_FLASH_PAGE] __attribute__((aligned(4)));
static crypto_sha512_ctx sha;

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

static uint8_t self_hand(void) {
    update_device_t dev;
    update_device_self(&dev);
    return dev.hand;
}

static void reset_session(void) {
    update_gesture_disarm();
    memset(&s, 0, sizeof(s)); // IDLE, last_error OK
    memset(signed_manifest, 0, sizeof(signed_manifest));
    memset(page, 0, sizeof(page));
}

static update_status_t fail(update_status_t status) {
    update_gesture_disarm();
    s.state      = UPDATE_STATE_ERROR;
    s.last_error = status;
    return status;
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
    rsp[21] = (die_ok ? 0x01 : 0) | (failing ? 0x02 : 0) | (dev.release_build ? 0x04 : 0);
#ifdef SVAL_UPDATE_TEST_HOOKS
    rsp[21] |= 0x08;
#endif
    rsp[22] = s.last_error;
    return die_ok && !failing ? UPDATE_OK : UPDATE_UNAVAILABLE;
}

static update_status_t manifest_load(uint32_t client, const uint8_t *req) {
    uint8_t off = req[1], n = req[2];
    if (s.state != UPDATE_STATE_IDLE && s.state != UPDATE_STATE_MANIFEST_LOADING) return UPDATE_BUSY;
    if (s.state == UPDATE_STATE_MANIFEST_LOADING && client != s.client) return UPDATE_OTHER_CLIENT;
    if (!update_flash_available()) return UPDATE_UNAVAILABLE;
    if (n == 0 || n > MANIFEST_DATA_MAX || off + n > UPDATE_SIGNED_MANIFEST_BYTES) return UPDATE_INVALID;
    // In order; offset 0 starts the load again.
    if (off != 0 && (s.state == UPDATE_STATE_IDLE || off != s.filled)) return UPDATE_OUT_OF_ORDER;
    if (s.state == UPDATE_STATE_IDLE) {
        reset_session();
        s.state  = UPDATE_STATE_MANIFEST_LOADING;
        s.client = client;
    }
    memcpy(signed_manifest + off, &req[3], n);
    s.filled = off + n;
    s.op_ms  = now_ms;
    return UPDATE_OK;
}

static update_status_t op_manifest(uint32_t client, const uint8_t *req, uint8_t *rsp) {
    update_status_t st = manifest_load(client, req);
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
    // The fields now, so the user is never asked to approve an image this half
    // would refuse; the signature waits for BEGIN (it is the slow part).
    update_device_t dev;
    update_device_self(&dev);
    update_status_t st = update_manifest_check(MANIFEST, &dev);
    if (st != UPDATE_OK) return fail(st);

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
    return UPDATE_OK;
}

static update_status_t op_commit(uint32_t client, const uint8_t *req) {
    update_status_t st = session(client, &req[1]);
    if (st != UPDATE_OK) return st;
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
    if (op > UPDATE_OP_REBIND) {
        st = UPDATE_INVALID;
    } else if (op != UPDATE_OP_INFO && !wrapped) {
        st = UPDATE_INVALID; // unwrapped VIA reaches here too (sval.c); only INFO is open
    } else if (req[0] != self_hand() && !(op == UPDATE_OP_INFO && req[0] == HAND_SELF)) {
        st = UPDATE_UNSUPPORTED; // the other half: M2
    } else if (s.state == UPDATE_STATE_COMMITTING && op != UPDATE_OP_INFO && op != UPDATE_OP_STATUS) {
        st = UPDATE_BUSY;
    } else {
        switch (op) {
            case UPDATE_OP_INFO:
                st = op_info(rsp);
                break;
            case UPDATE_OP_MANIFEST:
                st = op_manifest(client, req, rsp);
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
            default: // UPDATE_OP_REBIND
                st = op_rebind(client, req);
                break;
        }
        if (op == UPDATE_OP_BEGIN || op == UPDATE_OP_END || op == UPDATE_OP_COMMIT || op == UPDATE_OP_ABORT || op == UPDATE_OP_REBIND) rsp[1] = s.state;
    }
    rsp[0] = st;
    memcpy(value, rsp, VALUE_BYTES);
}

// ---- slow work, one slice per pass ---------------------------------------------------------

static void verify_manifest(void) {
    update_device_t dev;
    update_device_self(&dev);
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
        case UPDATE_STATE_MANIFEST_LOADING:
            return UPDATE_LED_NONE;
        case UPDATE_STATE_ERROR:
            return UPDATE_LED_ERROR;
        case UPDATE_STATE_COMMITTING:
            return UPDATE_LED_WRITING;
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
    now_ms = updater_port_now_ms();
    if (s.state >= UPDATE_STATE_MANIFEST_LOADING && s.state <= UPDATE_STATE_VERIFIED && now_ms - s.op_ms >= SVAL_UPDATE_SESSION_TIMEOUT_MS) {
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
        case UPDATE_STATE_COMMITTING:
            if ((int32_t)(now_ms - s.commit_due_ms) >= 0) {
                // Returns only when the commit refused before touching the firmware.
                update_status_t st = update_commit_run(MANIFEST, s.crc_body);
                fail(st != UPDATE_OK ? st : UPDATE_FLASH_ERR);
            }
            break;
        default:
            break;
    }
    now_ms = updater_port_now_ms();
    update_led_show(led_mode());
}

bool updater_active(void) {
    return s.state != UPDATE_STATE_IDLE;
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
#endif
