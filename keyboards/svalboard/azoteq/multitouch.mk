# Native Precision Touchpad (keyboards/svalboard/docs/native-touchpad.md): the
# USB half presents the Azoteq pads as one touchpad on its own HID interface.
# Included by the auto builds (pointing_auto/pointing_auto.mk), which ship it
# and turn it on unless SVAL_NATIVE_PTP=no, and by the per-module Azoteq build
# (azoteq/rules.mk), which turns it on only with SVAL_NATIVE_PTP=yes.
ifeq ($(strip $(SVAL_NATIVE_PTP)),yes)
  # SVAL_AZOTEQ_MULTITOUCH: the pads' contact data (drivers/sensors/azoteq_iqs5xx.c).
  # SVAL_MT_SPLIT: the other half's pad over the split link (azoteq/multitouch.c).
  OPT_DEFS += -DSVAL_NATIVE_PTP -DSVAL_AZOTEQ_MULTITOUCH -DSVAL_MT_SPLIT
  # Fast mode: one read of every contact is 45 bytes.
  OPT_DEFS += -DI2C1_CLOCK_SPEED=400000
  SRC += azoteq/multitouch.c azoteq/ptp.c
  # The touch page of Sval 0x2D (docs/protocol.md "Touchpad touch").
  SRC += azoteq/touch_stream.c pad_touch.c
  ifeq ($(strip $(SVAL_PTP_KEY_BUTTONS)),yes)
    OPT_DEFS += -DSVAL_PTP_KEY_BUTTONS=1
  endif
  ifeq ($(strip $(SVAL_PTP_MOUSE_BEFORE_MODE3)),no)
    OPT_DEFS += -DSVAL_PTP_MOUSE_BEFORE_MODE3=0
  endif
  # Dev builds only: the touchpad dev command (raw HID 0xFC op 0x42: owner
  # and status; tools/multitouch/ptp_owner.py). Not a released protocol.
  ifeq ($(strip $(SVAL_TOUCH_DEV)),yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)),yes)
      $(error SVAL_TOUCH_DEV cannot be part of a release build)
    endif
    OPT_DEFS += -DSVAL_TOUCH_DEV
    # Split-link diagnostics (dev builds only): another baud rate, another
    # receive timeout, and aborting a receive at once on a framing error.
    ifneq ($(strip $(SVAL_SPLIT_BAUD)),)
      OPT_DEFS += -DSVAL_SPLIT_BAUD=$(strip $(SVAL_SPLIT_BAUD))
    endif
    ifneq ($(strip $(SVAL_SPLIT_TIMEOUT_MS)),)
      OPT_DEFS += -DSERIAL_USART_TIMEOUT=$(strip $(SVAL_SPLIT_TIMEOUT_MS))
    endif
    ifeq ($(strip $(SVAL_SPLIT_ABORT)),yes)
      OPT_DEFS += -DSVAL_SPLIT_ABORT
    endif
    ifeq ($(strip $(SVAL_SPLIT_SLEWSLOW)),yes)
      OPT_DEFS += -DSVAL_SPLIT_SLEWSLOW
    endif
    ifneq ($(strip $(SVAL_SPLIT_DRIVE)),)
      OPT_DEFS += -DSVAL_SPLIT_DRIVE=$(strip $(SVAL_SPLIT_DRIVE))
    endif
  endif
else
  ifeq ($(strip $(SVAL_TOUCH_DEV)),yes)
    $(error SVAL_TOUCH_DEV needs SVAL_NATIVE_PTP=yes)
  endif
endif
