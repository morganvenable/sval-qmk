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

# In-firmware updater (docs/updater-plan.md); opt in with -e SVAL_UPDATER=yes.
# SVAL_UPDATE_TEST_HOOKS adds the commit halt points for hardware tests, and
# SVAL_UPDATE_RELEASE makes a release updater build: no test key, and images
# signed with it or flagged DIAGNOSTIC are refused.
SVAL_UPDATER ?= no
SVAL_UPDATE_TEST_HOOKS ?= no
SVAL_UPDATE_RELEASE ?= no
ifeq ($(strip $(SVAL_UPDATER)), yes)
  ifeq ($(strip $(SVAL_KEYTEST)), yes)
    $(error SVAL_UPDATER and SVAL_KEYTEST cannot be combined: keytest injects key events (R11))
  endif
  OPT_DEFS += -DSVAL_UPDATER
  SRC += updater/update_flash.c updater/update_image.c updater/update_keys.c
  SRC += updater/vendor/monocypher.c updater/vendor/optional/monocypher-ed25519.c
  EXTRAINCDIRS += keyboards/svalboard/updater/vendor
  # main()'s stack: 2 KiB by default (platforms/chibios/platform.mk); the
  # Ed25519 check alone needs about 1.9 KiB. SRAM4 (4 KiB) also holds the 1 KiB
  # exception stack and ChibiOS's 0x120-byte ch0, so 0xAE0 is the most that
  # fits (the plan's 0xC00 overflows ram4 by 288 bytes). The link fails if
  # anything else lands in SRAM4.
  USE_PROCESS_STACKSIZE = 0xAE0
  ifeq ($(strip $(SVAL_UPDATE_TEST_HOOKS)), yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
      $(error SVAL_UPDATE_TEST_HOOKS cannot be part of a release build)
    endif
    OPT_DEFS += -DSVAL_UPDATE_TEST_HOOKS
  endif
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    OPT_DEFS += -DSVAL_UPDATE_RELEASE
  endif
else
  ifneq ($(filter yes,$(strip $(SVAL_UPDATE_TEST_HOOKS)) $(strip $(SVAL_UPDATE_RELEASE))),)
    $(error SVAL_UPDATE_TEST_HOOKS and SVAL_UPDATE_RELEASE need SVAL_UPDATER=yes)
  endif
endif
