POINTING_DEVICE_ENABLE = yes
# Use custom wrapper that calls azoteq functions directly
POINTING_DEVICE_DRIVER = custom

# Include Azoteq driver sources for our custom wrapper to call
SRC += drivers/sensors/azoteq_iqs5xx.c
QUANTUM_LIB_SRC += i2c_master.c

EXTRA_SRC = azoteq/azoteq.c

MH_AUTO_BUTTONS = yes

# Local-pad Windows prototype. No new split wire protocol in this build.
ifeq ($(strip $(SVAL_AZOTEQ_MULTITOUCH)),yes)
  OPT_DEFS += -DSVAL_AZOTEQ_MULTITOUCH -DSVAL_HOST_BOOTLOADER=1 -DI2C1_CLOCK_SPEED=400000
  # Firmware-owned swipe and pinch, and their settings (Sval 0x2E).
  OPT_DEFS += -DSVAL_TOUCH_GESTURES
  SRC += azoteq/multitouch.c azoteq/touch_gestures.c
endif

ifeq ($(strip $(SVAL_TOUCH_TEST_BOARD)),yes)
  OPT_DEFS += -DSVAL_MT_SPLIT
endif

# Unified mode on a board with keys (keyboards/svalboard/rules.mk checks it).
ifeq ($(strip $(SVAL_AZOTEQ_UNIFIED)),yes)
  OPT_DEFS += -DSVAL_MT_SPLIT -DSVAL_AZOTEQ_UNIFIED
endif
