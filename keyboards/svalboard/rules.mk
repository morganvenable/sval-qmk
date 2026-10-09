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
# SVAL_UPDATE_TEST_KEY accepts images signed with the TEST-ONLY key, whose seed
# is in the repository: test boards only. SVAL_UPDATE_TEST_HOOKS adds the
# commit halt points for hardware tests and implies the test key.
# SVAL_UPDATE_RELEASE makes a release updater build: images signed with the
# test key or flagged DIAGNOSTIC are refused. Until M3 adds release keys, an
# updater build without the test key accepts no image at all.
SVAL_UPDATER ?= no
SVAL_UPDATE_TEST_KEY ?= no
SVAL_UPDATE_TEST_HOOKS ?= no
SVAL_UPDATE_RELEASE ?= no
ifeq ($(strip $(SVAL_UPDATER)), yes)
  ifeq ($(strip $(SVAL_KEYTEST)), yes)
    $(error SVAL_UPDATER and SVAL_KEYTEST cannot be combined: keytest injects key events (R11))
  endif
  OPT_DEFS += -DSVAL_UPDATER -DCLIENT_WRAPPER_ID_GETTER
  SRC += updater/updater.c updater/update_gesture.c updater/update_led.c updater/update_commit.c
  SRC += updater/update_flash.c updater/update_image.c updater/update_keys.c
  # M2: the split pause (D15, a matrix_scan() override) and the split relay.
  SRC += split_pause.c updater/update_split.c updater/update_split_wire.c
  SRC += updater/vendor/monocypher.c updater/vendor/optional/monocypher-ed25519.c
  EXTRAINCDIRS += keyboards/svalboard/updater/vendor
  # The commit's RAM code must not branch into flash (R2): no jump tables, no
  # loops turned into memcpy/memset calls. -fstack-usage writes
  # update_commit.su for tools/check_ram_funcs.py.
  $(INTERMEDIATE_OUTPUT)/updater/update_commit.o: FILE_SPECIFIC_CFLAGS += -fno-jump-tables -fno-tree-loop-distribute-patterns -fstack-usage
  # main()'s stack: 2 KiB by default (platforms/chibios/platform.mk); the
  # Ed25519 check alone needs about 1.9 KiB. SRAM4 (4 KiB) also holds the 1 KiB
  # exception stack and ChibiOS's 0x120-byte ch0, so 0xAE0 is the most that
  # fits (the plan's 0xC00 overflows ram4 by 288 bytes). The link fails if
  # anything else lands in SRAM4.
  USE_PROCESS_STACKSIZE = 0xAE0
  # LED takeover (D20): rgblight's driver becomes the gate in
  # updater/update_led.c, which forwards to the WS2812 driver except while the
  # updater shows its own colours. 'override' because svalboard/right/rules.mk
  # sets RGBLIGHT_DRIVER again after this file. Only the ws2812 driver choice
  # pulls in the WS2812 driver (builddefs/common_features.mk), so ask for it
  # here; config.h supplies the LED count that choice would have set.
  override RGBLIGHT_DRIVER = custom
  WS2812_DRIVER_REQUIRED = yes
  ifeq ($(strip $(SVAL_UPDATE_TEST_HOOKS)), yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
      $(error SVAL_UPDATE_TEST_HOOKS cannot be part of a release build)
    endif
    OPT_DEFS += -DSVAL_UPDATE_TEST_HOOKS
    override SVAL_UPDATE_TEST_KEY = yes
  endif
  ifeq ($(strip $(SVAL_UPDATE_TEST_KEY)), yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
      $(error SVAL_UPDATE_TEST_KEY cannot be part of a release build)
    endif
    $(warning SVAL_UPDATE_TEST_KEY: this build accepts updates signed with the TEST-ONLY key, whose seed is public. Test boards only; never ship it.)
    OPT_DEFS += -DSVAL_UPDATE_TEST_KEY
  endif
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    OPT_DEFS += -DSVAL_UPDATE_RELEASE
  endif
else
  ifneq ($(filter yes,$(strip $(SVAL_UPDATE_TEST_KEY)) $(strip $(SVAL_UPDATE_TEST_HOOKS)) $(strip $(SVAL_UPDATE_RELEASE))),)
    $(error SVAL_UPDATE_TEST_KEY, SVAL_UPDATE_TEST_HOOKS and SVAL_UPDATE_RELEASE need SVAL_UPDATER=yes)
  endif
endif
