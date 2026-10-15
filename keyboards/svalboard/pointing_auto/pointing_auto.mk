# One firmware for every pointing module (pointing_auto/pointing_auto.h).
# Included by the plain left and right builds.
OPT_DEFS += -DSVAL_POINTING_AUTO
SRC += pointing_auto/pointing_auto.c \
       pointing_auto/trackball_pmw3360.c pointing_auto/trackball_pmw3389.c \
       pointing_auto/azoteq.c drivers/sensors/azoteq_iqs5xx.c \
       pointing_auto/trackpoint.c pointing_auto/ps2_module.c
QUANTUM_LIB_SRC += spi_master.c i2c_master.c
PS2_ENABLE = yes
PS2_DRIVER = vendor
VPATH += modules/svalboard/pointing_device_ps2

# Native Precision Touchpad for the Azoteq pads (azoteq/multitouch.mk): on
# unless SVAL_NATIVE_PTP=no.
SVAL_NATIVE_PTP ?= yes
include keyboards/svalboard/azoteq/multitouch.mk
