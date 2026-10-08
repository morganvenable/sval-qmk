# MCU name
MCU = RP2040
BOOTLOADER = rp2040
BOARD = GENERIC_RP_RP2040

# we want some pretty lights (RGBLIGHT_ENABLE is in info.json)
RGBLIGHT_SPLIT = yes
RGBLIGHT_DRIVER = ws2812
WS2812_DRIVER   = vendor

CUSTOM_MATRIX = lite

SRC += axis_scale.c matrix.c scanlab.c power.c identity.c

# Settings start at 0x160000; keep firmware below them.
LDFLAGS += -Wl,-T,keyboards/svalboard/flash_reservation.ld

SERIAL_DRIVER = vendor

POINTING_DEVICE_ENABLE = yes
POINTING_DEVICE_DRIVER = custom

# Use shared endpoint for proper hires scroll feature report handling
MOUSE_SHARED_EP = yes

LAYER_LOCK_ENABLE = yes

# this turns on Manna-Harbour's automousekeys:
MH_AUTO_BUTTONS = yes

OS_DETECTION_ENABLE = yes
NO_USB_STARTUP_CHECK = yes

ifeq ($(strip $(MH_AUTO_BUTTONS)), yes)
  OPT_DEFS += -DMH_AUTO_BUTTONS
endif

# Enable VIA (we'll use VIA3 custom values instead of Vial)
VIA_ENABLE = yes

# Allow VIA to read matrix state for Matrix Tester (note: enables keylogger attack vector)
VIA_INSECURE = yes

# Note: Sval module is enabled via keymap.json with "modules": ["svalboard/core"]

# On-device action-path tests; opt in with -e SVAL_KEYTEST=yes.
ifeq ($(strip $(SVAL_KEYTEST)), yes)
  SRC += keytest.c
  OPT_DEFS += -DSVAL_KEYTEST
endif
