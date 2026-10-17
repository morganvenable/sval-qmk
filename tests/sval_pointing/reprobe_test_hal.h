// Host test stand-ins for pointer_reprobe.c: the identity type and flags (as svalboard.h has
// them), the clock, and the hooks the drivers supply (reprobe_test.c mocks them).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum sval_pointer_sensor {
    SVAL_SENSOR_NONE        = 0,
    SVAL_SENSOR_PMW3360     = 1,
    SVAL_SENSOR_PMW3389     = 2,
    SVAL_SENSOR_NO_RESPONSE = 3,
    SVAL_SENSOR_UNKNOWN     = 4,
    SVAL_SENSOR_AZOTEQ      = 5,
    SVAL_SENSOR_TRACKPOINT  = 6,
};
#define SVAL_POINTER_REPORTED 0x01
#define SVAL_POINTER_LEFT 0x02
#define SVAL_POINTER_INIT_OK 0x04
#define SVAL_POINTER_AUTO 0x08

typedef struct {
    uint8_t flags, built_for, detected, product_id, inverse_product_id;
} sval_pointer_identity_t;

void     sval_pointer_identity(sval_pointer_identity_t *id);
uint8_t  sval_pointer_own_bus(void);
uint32_t timer_read32(void);
uint32_t timer_elapsed32(uint32_t last);
