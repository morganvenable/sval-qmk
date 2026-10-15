#pragma once
#include <stdbool.h>
#include <stdint.h>
bool sval_other_half_connected(void);
enum { SVAL_SENSOR_NONE = 0, SVAL_SENSOR_PMW3389 = 2, SVAL_SENSOR_AZOTEQ = 5, SVAL_SENSOR_TRACKPOINT = 6 };
uint8_t sval_pointer_sensor_for_hand(bool left);
typedef struct { char bytes[72]; } saved_values_t;
typedef struct {
    uint8_t global_bits;
    uint8_t axis_state;
    bool    left_scroll, right_scroll, left_automouse, right_automouse;
} sval_pointing_modes_t;
void sval_pointing_modes(sval_pointing_modes_t *m);
