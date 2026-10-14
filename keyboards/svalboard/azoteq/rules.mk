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
  SRC += azoteq/multitouch.c
endif

ifeq ($(strip $(SVAL_TOUCH_TEST_BOARD)),yes)
  OPT_DEFS += -DSVAL_MT_SPLIT
endif
