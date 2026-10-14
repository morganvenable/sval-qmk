#pragma once

#define I2C_DRIVER I2CD0
#define I2C1_SDA_PIN GP16
#define I2C1_SCL_PIN GP17

#define AZOTEQ_IQS5XX_TPS43
#define AZOTEQ_IQS5XX_TIMEOUT_MS 10
#ifndef SVAL_NATIVE_PTP
#define AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE true
#endif
//This decreases the required 'wait' time to activate dragging/selecting on a touchpad from default 300ms to 20ms. Big QoL. 
#define AZOTEQ_IQS5XX_HOLD_TIME 20
//#define POINTING_DEVICE_MOTION_PIN GP18

#define SPLIT_POINTING_ENABLE
#define POINTING_DEVICE_COMBINED
#define POINTING_DEVICE_AUTO_MOUSE_MH_ENABLE

#ifdef SVAL_NATIVE_PTP
// Native Precision Touchpad experiment (azoteq/ptp.c). Build-time tunables.
#    define SVAL_TOUCH_OWNER_DEFAULT 3  // NATIVE; runtime: tools/multitouch/ptp_owner.py
#    define SVAL_PTP_GAP_MM 10          // unreachable surface between the pads, 0..50
#    define SVAL_PTP_FLIP_L_X 0
#    define SVAL_PTP_FLIP_L_Y 0
#    define SVAL_PTP_FLIP_R_X 0
#    define SVAL_PTP_FLIP_R_Y 0
#    define SVAL_PTP_PAD_PERIOD_MS 8    // chip report period on both halves (125 Hz)
#    define SVAL_PTP_REMOTE_POLL_MS 5   // poll of the other half's snapshot
#    define SVAL_PTP_REMOTE_STALE_MS 50 // other half silent this long: lift its contacts
#    define SVAL_PTP_MIN_FRAME_US 4000  // at most 250 frames a second
#    define SVAL_PTP_KEEPALIVE_MS 50    // repeat while down (Linux releases after 100 ms)
#    define SVAL_PTP_PALM_AREA 0        // 0 off; contact area above this is a palm
#    ifndef SVAL_PTP_KEY_BUTTONS
#        define SVAL_PTP_KEY_BUTTONS 0  // 1: MS_BTN1/2 are touchpad Buttons 2/3 while it is active
#    endif
#    ifndef SVAL_PTP_MOUSE_BEFORE_MODE3
#        define SVAL_PTP_MOUSE_BEFORE_MODE3 1 // 0: pads silent until the host writes mode 3
#    endif
#    define SVAL_MT_DEFAULT_PERIOD_MS SVAL_PTP_PAD_PERIOD_MS
#    define AZOTEQ_IQS5XX_REPORT_RATE SVAL_PTP_PAD_PERIOD_MS
// The OS recognizes gestures; the chip's own are off (MOUSE owner is motion only).
#    define AZOTEQ_IQS5XX_TAP_ENABLE false
#    define AZOTEQ_IQS5XX_TWO_FINGER_TAP_ENABLE false
#    define AZOTEQ_IQS5XX_PRESS_AND_HOLD_ENABLE false
#    define AZOTEQ_IQS5XX_SWIPE_X_ENABLE false
#    define AZOTEQ_IQS5XX_SWIPE_Y_ENABLE false
#    define AZOTEQ_IQS5XX_SCROLL_ENABLE false
#    define AZOTEQ_IQS5XX_ZOOM_ENABLE false
// A new bcdDevice so Windows re-reads the interface set it may have cached.
#    define DEVICE_VER 0x0F02
#endif
