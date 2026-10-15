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

# Experiment: the USB half presents both pads as one native Windows Precision
# Touchpad (its own HID interface). Off by default; needs both pads.
ifeq ($(strip $(SVAL_NATIVE_PTP)),yes)
  ifneq ($(strip $(SVAL_AZOTEQ_MULTITOUCH)),yes)
    $(error SVAL_NATIVE_PTP requires SVAL_AZOTEQ_MULTITOUCH=yes)
  endif
  ifneq ($(strip $(SVAL_TOUCH_TEST_BOARD)),yes)
    $(error SVAL_NATIVE_PTP requires SVAL_TOUCH_TEST_BOARD=yes (two pads))
  endif
  OPT_DEFS += -DSVAL_NATIVE_PTP
  SRC += azoteq/ptp.c
  ifeq ($(strip $(SVAL_PTP_KEY_BUTTONS)),yes)
    OPT_DEFS += -DSVAL_PTP_KEY_BUTTONS=1
  endif
  ifeq ($(strip $(SVAL_PTP_MOUSE_BEFORE_MODE3)),no)
    OPT_DEFS += -DSVAL_PTP_MOUSE_BEFORE_MODE3=0
  endif
endif

# Timing counters (main loop, matrix frames, pad I2C, split RPC, touchpad
# frames), read with tools/multitouch/perf.py. Off by default.
ifeq ($(strip $(SVAL_PERF_STATS)),yes)
  ifneq ($(strip $(SVAL_AZOTEQ_MULTITOUCH)),yes)
    $(error SVAL_PERF_STATS requires SVAL_AZOTEQ_MULTITOUCH=yes)
  endif
  OPT_DEFS += -DSVAL_PERF_STATS
  SRC += azoteq/perf.c
endif
