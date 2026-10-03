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

# One-time migration of a user's setup from the shipped Vial firmware
# (svalboard/vial-qmk v2025-11-01). Set to no to build without it.
SVAL_MIGRATE_VIAL ?= yes
ifeq ($(strip $(SVAL_MIGRATE_VIAL)), yes)
  SRC += migrate_vial.c
  OPT_DEFS += -DSVAL_MIGRATE_VIAL
endif

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
