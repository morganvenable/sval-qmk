// Click diagnostics (click_diag.h). Recording only.
#include QMK_KEYBOARD_H
#include <string.h>
#include "click_diag.h"
#include "sync_timer.h"
#include "transactions.h"
bool sval_other_half_connected(void); // svalboard.c
#ifdef SVAL_UPDATER
#    include "split_pause.h"
#endif
#ifdef SVAL_AZOTEQ_MULTITOUCH
#    include "azoteq/multitouch.h"
#endif

#define CD_N 128      // events, 12 bytes each
#define CD_FRAMES 256 // pad contact frames, 12 bytes each (about 2.5 s of touching)
#define CD_PRESSES 32 // key press times (no keycodes)
#define CD_TYPING_MS 500

typedef struct {
    uint32_t t;
    uint8_t  type;
    uint8_t  p[7];
} cd_entry_t;
_Static_assert(sizeof(cd_entry_t) == 12, "12-byte entries");

static cd_entry_t cd_ring[CD_N], cd_frames[CD_FRAMES];
static uint16_t   cd_seq, cd_frame_seq, cd_press_seq;
static uint32_t   cd_total, cd_frame_total, cd_press_total;
static uint32_t   cd_press[CD_PRESSES];
static uint32_t   cd_last_press;
static bool       cd_pressed_once;
uint8_t           cd_cause    = CD_CAUSE_OTHER;
uint16_t          cd_cause_kc = 0;

// Pad health since boot.
static uint8_t  cd_resets, cd_inits, cd_reati, cd_ati_err;
static uint16_t cd_read_fail, cd_touch_downs, cd_typing_touches, cd_btn_reports, cd_gesture_clicks;

static uint32_t cd_now(void) {
    return sync_timer_read32();
}

void cd_log(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g) {
    cd_entry_t *x = &cd_ring[(uint16_t)(cd_seq + 1) % CD_N];
    x->t          = cd_now();
    x->type       = type;
    x->p[0] = a, x->p[1] = b, x->p[2] = c, x->p[3] = d, x->p[4] = e, x->p[5] = f, x->p[6] = g;
    cd_seq++;
    cd_total++;
}

void cd_key_press(void) {
    cd_last_press   = cd_now();
    cd_pressed_once = true;
    cd_press[(uint16_t)(cd_press_seq + 1) % CD_PRESSES] = cd_last_press;
    cd_press_seq++;
    cd_press_total++;
}

uint16_t cd_ms_since_press(void) {
    if (!cd_pressed_once) return 0xFFFF;
    uint32_t a = cd_now() - cd_last_press;
    return a > 0xFFFE ? 0xFFFE : (uint16_t)a;
}

uint8_t cd_mods(void) {
    return get_mods() | get_oneshot_mods() | get_weak_mods();
}

uint8_t cd_layer(void) {
    return get_highest_layer(layer_state | default_layer_state);
}

// ---- layers ---------------------------------------------------------------------------

layer_state_t layer_state_set_kb(layer_state_t state) {
    if (state != layer_state) cd_log(CD_LAYER, layer_state & 0xFF, (layer_state >> 8) & 0xFF, state & 0xFF, (state >> 8) & 0xFF, cd_cause, cd_cause_kc & 0xFF, cd_cause_kc >> 8);
    return layer_state_set_user(state);
}

void post_process_record_kb(uint16_t keycode, keyrecord_t *record) {
    post_process_record_user(keycode, record);
    cd_cause    = CD_CAUSE_OTHER;
    cd_cause_kc = 0;
}

// ---- the pad --------------------------------------------------------------------------

static uint8_t cd_b(uint16_t v, uint8_t shift) {
    v >>= shift;
    return v > 255 ? 255 : (uint8_t)v;
}

// Any key down on this half's own rows (the sub knows only its own): a touch now or
// within CD_TYPING_MS of one counts as a touch while typing.
static uint32_t cd_local_key_at;
static bool     cd_local_key_seen;
static void     cd_local_keys(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        if (matrix_get_row(r)) {
            cd_local_key_at   = cd_now();
            cd_local_key_seen = true;
            return;
        }
}
static bool cd_typing(void) {
    uint32_t now = cd_now();
    if (cd_local_key_seen && now - cd_local_key_at < CD_TYPING_MS) return true;
    return cd_pressed_once && now - cd_last_press < CD_TYPING_MS;
}

static void cd_slot(const uint8_t *raw, uint8_t i, uint8_t *o3) {
    const uint8_t *p = raw + 10 + 7 * i;
    o3[0]            = cd_b((uint16_t)((p[0] << 8) | p[1]), 4);
    o3[1]            = cd_b((uint16_t)((p[2] << 8) | p[3]), 4);
    o3[2]            = cd_b((uint16_t)((p[4] << 8) | p[5]), 4);
}

// One frame of what the pad reported: [fingers | sysinfo0 high bits] [ge0 | ge1 << 6]
// [slot 0: x/16 y/16 strength/16] [slot 1: same], then [down slot mask], kept while
// anything is down and once at the lift, so idle time does not push the history out.
static void cd_frame(const uint8_t *raw, uint8_t slots, uint8_t mask) {
    cd_entry_t *x = &cd_frames[(uint16_t)(cd_frame_seq + 1) % CD_FRAMES];
    x->t          = cd_now();
    x->type       = (uint8_t)((raw[5] & 7) | (raw[3] & 0xF8)); // fingers, sysinfo0 bits 3-7
    uint8_t s[6]  = {0};
    uint8_t k = 0;
    for (uint8_t i = 0; i < slots && k < 2; i++)
        if (mask & (1u << i)) cd_slot(raw, i, &s[3 * k++]);
    x->p[0] = (uint8_t)((raw[1] & 0x3F) | ((raw[2] & 3) << 6));
    memcpy(&x->p[1], s, 6);
    cd_frame_seq++;
    cd_frame_total++;
}

void cd_pad_read(const uint8_t *raw, uint8_t slots, bool ok) {
    static bool    pok = true;
    static uint8_t pf, pg, psys, pmask, pmask_frame;
    if (!ok) {
        cd_read_fail++;
        if (pok) cd_log(CD_PAD_STATUS, psys, 0, 3, 0, 0, 0, 0);
        pok = false;
        return;
    }
    pok = true;
    cd_local_keys();
    const uint8_t ge0 = raw[1] & 0x3F, ge1 = raw[2] & 0x05; // taps, hold, swipes; two-finger tap, zoom (not scroll)
    const uint8_t g = (uint8_t)(ge0 | (ge1 << 6)), sys0 = raw[3], fingers = raw[5];
    uint8_t       mask = 0;
    for (uint8_t i = 0; i < slots && i < 5; i++) {
        const uint8_t *p = raw + 10 + 7 * i;
        if (p[4] || p[5]) mask |= (uint8_t)(1u << i);
    }
    if (fingers || mask || (raw[1] & 0x3F) || (raw[2] & 7) || pf || pmask_frame) cd_frame(raw, slots, mask);
    pmask_frame = (uint8_t)(fingers || mask);
    if (g && g != pg) {
        uint8_t s[6] = {0}, k = 0;
        for (uint8_t i = 0; i < slots && k < 2; i++)
            if (mask & (1u << i)) {
                cd_slot(raw, i, &s[3 * k]);
                k++;
            }
        cd_log(CD_PAD_GESTURE, raw[1], raw[2], fingers, s[0], s[1], s[3], s[4]);
        if (ge0 & 3 || ge1 & 1) cd_gesture_clicks++;
    }
    if (fingers > pf || (mask & ~pmask)) {
        cd_touch_downs++;
        bool typing = cd_typing();
        if (typing) cd_typing_touches++;
        uint8_t        nw = (uint8_t)(mask & ~pmask), i = 0, x = 0, y = 0, st = 0, area = 0;
        const uint8_t *p  = NULL;
        for (i = 0; i < slots && i < 5; i++)
            if (nw & (1u << i)) break;
        if (i < slots && i < 5) {
            p    = raw + 10 + 7 * i;
            x    = cd_b((uint16_t)((p[0] << 8) | p[1]), 4);
            y    = cd_b((uint16_t)((p[2] << 8) | p[3]), 4);
            st   = cd_b((uint16_t)((p[4] << 8) | p[5]), 4);
            area = p[6];
        }
        cd_log(CD_PAD_TOUCH, (uint8_t)(fingers | (typing ? 0x80 : 0)), pf, x, y, st, area, sys0);
    }
    if ((sys0 & 0x80) && !(psys & 0x80)) {
        cd_resets++;
        cd_log(CD_PAD_STATUS, sys0, raw[4], 1, 0, 0, 0, 0);
    }
    if ((sys0 & 0x10) && !(psys & 0x10)) {
        cd_reati++;
        cd_log(CD_PAD_STATUS, sys0, raw[4], 2, 0, 0, 0, 0);
    }
    if ((sys0 & 0x08) && !(psys & 0x08)) {
        cd_ati_err++;
        cd_log(CD_PAD_STATUS, sys0, raw[4], 4, 0, 0, 0, 0);
    }
    pf = fingers, pg = g, psys = sys0, pmask = mask;
}

// A chip report the gesture guard ignored (azoteq/pad_gesture_guard.h): the
// gesture bytes as the chip sent them, and the rest of its base bytes.
void sval_pad_guard_ignored_kb(uint8_t why, const uint8_t *base, uint8_t ge0, uint8_t ge1, bool woke) {
    cd_log(CD_PAD_IGNORED, (uint8_t)(why | (woke ? 0x80 : 0)), base[0], ge0, ge1, base[3], base[4], base[5]);
}

void cd_pad_init(bool ok) {
    if (cd_inits < 255) cd_inits++;
    cd_log(CD_PAD_INIT, cd_inits, ok, 0, 0, 0, 0, 0);
}

// ---- what went out --------------------------------------------------------------------

void cd_hand_buttons(uint8_t hand, uint8_t buttons) {
    static uint8_t prev[2];
    if (hand > 1 || buttons == prev[hand]) return;
    uint16_t ms = cd_ms_since_press();
    cd_log(CD_HAND_BTN, hand, buttons, prev[hand], ms & 0xFF, ms >> 8, cd_mods(), cd_layer());
    prev[hand] = buttons;
}

void cd_mouse_sent(uint8_t buttons) {
    static uint8_t prev;
    if (buttons) cd_btn_reports++;
    if (buttons == prev) return;
    uint16_t ms     = cd_ms_since_press();
    bool     native = false;
#ifdef SVAL_AZOTEQ_MULTITOUCH
    native = sval_mt_os_touchpad(0) || sval_mt_os_touchpad(1);
#endif
    cd_log(CD_MOUSE_SENT, buttons, prev, cd_mods(), cd_layer(), ms & 0xFF, ms >> 8, native);
    prev = buttons;
}

void cd_ptp_buttons(uint8_t buttons, uint8_t contacts) {
    static uint8_t prev, prevc;
    if (buttons == prev && !(contacts >= 2 && prevc < 2)) {
        prevc = contacts;
        return;
    }
    cd_log(CD_PTP_BTN, buttons, prev, contacts, cd_mods(), cd_layer(), 0, 0);
    prev  = buttons;
    prevc = contacts;
}

// ---- readout --------------------------------------------------------------------------

// [cmd] [ring] [seq lo] [seq hi] -> 26 bytes; returns the count (cmd 1) or 1 (cmd 0).
// cmd 0: [ev seq u16] [press seq u16] [now u32] [left] [resets] [inits] [re-ATI] [read
//        fails u16] [touch-downs u16] [touches while typing u16] [button reports u16]
//        [gesture clicks u16] [flags: 1 master, 2 multitouch build, 4 OS touchpad now]
//        [ATI errors] [frame seq u16]
// cmd 1: ring 0 events, 2 per reply (seq, seq-1); ring 1 key press times, 6 per reply;
//        ring 2 pad frames, 2 per reply.
static void put16(uint8_t *o, uint16_t v) {
    o[0] = v & 0xFF;
    o[1] = v >> 8;
}
static void put32(uint8_t *o, uint32_t v) {
    put16(o, v & 0xFFFF);
    put16(o + 2, v >> 16);
}
static bool cd_has(uint16_t s, uint16_t newest, uint32_t total, uint16_t n) {
    uint16_t back = (uint16_t)(newest - s);
    return back < n && back < total;
}
static uint8_t cd_fill(uint8_t cmd, uint8_t ring, uint16_t seq, uint8_t *o) {
    memset(o, 0, 26);
    if (cmd == 0) {
        put16(o, cd_seq);
        put16(o + 2, cd_press_seq);
        put32(o + 4, cd_now());
        o[8]  = is_keyboard_left();
        o[9]  = cd_resets;
        o[10] = cd_inits;
        o[11] = cd_reati;
        put16(o + 12, cd_read_fail);
        put16(o + 14, cd_touch_downs);
        put16(o + 16, cd_typing_touches);
        put16(o + 18, cd_btn_reports);
        put16(o + 20, cd_gesture_clicks);
        o[22] = is_keyboard_master() ? 1 : 0;
#ifdef SVAL_AZOTEQ_MULTITOUCH
        o[22] |= 2;
        if (sval_mt_os_touchpad(0) || sval_mt_os_touchpad(1)) o[22] |= 4;
#endif
        o[23] = cd_ati_err;
        put16(o + 24, cd_frame_seq);
        return 1;
    }
    if (cmd != 1) return 0;
    uint8_t n = 0;
    if (ring == 0 || ring == 2) {
        const cd_entry_t *r = ring ? cd_frames : cd_ring;
        uint16_t          N = ring ? CD_FRAMES : CD_N, newest = ring ? cd_frame_seq : cd_seq;
        uint32_t          total = ring ? cd_frame_total : cd_total;
        for (; n < 2; n++) {
            uint16_t s = (uint16_t)(seq - n);
            if (!cd_has(s, newest, total, N)) break;
            memcpy(o + 12 * n, &r[s % N], 12);
        }
    } else if (ring == 1) {
        for (; n < 6; n++) {
            uint16_t s = (uint16_t)(seq - n);
            if (!cd_has(s, cd_press_seq, cd_press_total, CD_PRESSES)) break;
            put32(o + 4 * n, cd_press[s % CD_PRESSES]);
        }
    }
    return n;
}

// The sub's side: [CD_OP] [cmd] [ring] [seq lo] [seq hi] -> [CD_OP] [n] [26 bytes].
bool cd_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 5 || in[0] != CD_OP || out_len < 28) return false;
    out[0] = CD_OP;
    out[1] = cd_fill(in[1], in[2], (uint16_t)(in[3] | (in[4] << 8)), &out[2]);
    return true;
}

// Sval 0x2C: [0xDF] [0x2C] [0xC1] [half: 0 this, 1 the other] [cmd] [ring] [seq lo] [seq hi]
// -> [0xDF] [0x2C] [0xC1] [half] [cmd] [n, 0xFF other half unavailable] [26 bytes].
void cd_command(uint8_t *data, uint8_t length) {
    if (length < 32) return;
    uint8_t  half = data[3], cmd = data[4], ring = data[5];
    uint16_t seq  = (uint16_t)(data[6] | (data[7] << 8));
    if (half == 0) {
        data[5] = cd_fill(cmd, ring, seq, &data[6]);
        return;
    }
    memset(&data[6], 0, 26);
    data[5] = 0xFF;
#ifdef SVAL_UPDATER
    if (sval_split_paused()) return;
#endif
    if (!sval_other_half_connected()) return;
    uint8_t req[5] = {CD_OP, cmd, ring, (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8)}, rsp[28] = {0};
    if (transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, sizeof(rsp), rsp) && rsp[0] == CD_OP) {
        data[5] = rsp[1];
        memcpy(&data[6], &rsp[2], 26);
    }
}

void keyboard_post_init_click_diag(void) {
    cd_log(CD_BOOT, is_keyboard_left(), is_keyboard_master(), 0, 0, 0, 0, 0);
}
