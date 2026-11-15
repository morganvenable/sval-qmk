// Click diagnostics (SVAL_CLICK_DIAG builds only, x4.6.6-clickdiag): a RAM ring on each half
// of every event that can press a mouse button or change the layer, read by Keybard's
// Click diagnostics panel (Sval 0x2C with [0xC1]). Recording only: no behavior changes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CD_OP 0xC1 // Sval 0x2C sub-op, and the KEYBOARD_SYNC_B op for the sub's ring: no other op uses it

// Event types. Each entry: [time ms u32] [type] [7 payload bytes].
enum {
    CD_BOOT        = 1,  // [left] [build bits] [0...]
    CD_KEY         = 2,  // [layer] [row] [col] [kc lo] [kc hi] [bit0 press, bit1 combo, bits4-7 tap count] [mods]
    CD_TAP_DANCE   = 3,  // [index] [step] [on_tap lo] [on_tap hi] [on_hold lo] [on_hold hi] [mods]
    CD_OVERRIDE    = 4,  // [index] [layer] [trigger lo] [trigger hi] [replacement lo] [replacement hi] [mods]
    CD_PAD_GESTURE = 5,  // [ge0] [ge1] [fingers] [x1/16] [y1/16] [x2/16] [y2/16]  (gesture bits changed)
    CD_PAD_TOUCH   = 6,  // [fingers now] [fingers before] [x/16] [y/16] [strength/16, 255 max] [area] [sysinfo0]
    CD_HAND_BTN    = 7,  // [hand 0 left 1 right] [buttons] [before] [ms since a key press lo] [hi] [mods] [layer]
    CD_MOUSE_SENT  = 8,  // [buttons] [before] [mods] [layer] [ms since a key press lo] [hi] [ptp native]
    CD_PTP_BTN     = 9,  // [buttons byte] [before] [contacts] [mods] [layer] [0] [0]
    CD_LAYER       = 10, // [old lo] [old hi] [new lo] [new hi] [cause] [cause kc lo] [cause kc hi]
    CD_AUTOMOUSE   = 11, // [on] [why: 0 key/pointer, 1 timeout] [layer state lo] [hi] [0...]
    CD_PAD_STATUS  = 12, // [sysinfo0] [sysinfo1] [what: 1 reset shown, 2 re-ATI, 3 read failed after ok] [0...]
    CD_PAD_INIT    = 13, // [init count] [ok] [0...]
};

// Why a layer changed (CD_LAYER cause).
enum {
    CD_CAUSE_OTHER     = 0, // not a key: firmware (auto mouse, pointing) or unknown
    CD_CAUSE_KEY       = 1, // during a key event (the keycode follows)
    CD_CAUSE_AUTOMOUSE = 2, // mouse_mode() (Auto Mouse layer)
    CD_CAUSE_HOST      = 3, // Keybard / host command (Sval 0x17)
};

extern uint8_t  cd_cause;
extern uint16_t cd_cause_kc;

void cd_log(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g);
void cd_key_press(void); // any key press (master): its time only, no keycode
uint16_t cd_ms_since_press(void);
uint8_t  cd_mods(void);
uint8_t  cd_layer(void);

// Hooks (each a no-op unless this build records).
void cd_pad_read(const uint8_t *raw, uint8_t slots, bool ok); // azoteq_iqs5xx.c: one pad read
void cd_pad_init(bool ok);
void cd_mouse_sent(uint8_t buttons); // host_mouse_send
void cd_hand_buttons(uint8_t hand, uint8_t buttons);
void cd_ptp_buttons(uint8_t buttons, uint8_t contacts);
bool cd_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
void keyboard_post_init_click_diag(void);
void cd_command(uint8_t *data, uint8_t length); // Sval 0x2C [0xC1] ...
