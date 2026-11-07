// Trackball diagnostics (SVAL_BALL_DIAG builds only): counters for a field bug.
#pragma once
#include <stdint.h>
// Boots since power-on (a not-cleared RAM word survives a soft reset), 0.5 s units
// when the split link first answered (14 = later, 15 = never), the first SPI probe result,
// the search that started the driver (14 = the boot probe, 15 = none yet) and when (100 ms).
extern uint8_t diag_boots, diag_link_halfs, diag_first_spi, diag_start_search, diag_start_tenths;
// The running-sensor health check: last result (bits 0-1: 0 never, 1 ok, 2 bad once then
// ok, 3 bad twice) and what was wrong on the last bad look (bits 4-6: 1 Product_ID,
// 2 Inverse_Product_ID, 4 Config2); recoveries started; checks run (wraps).
extern uint8_t diag_health, diag_recoveries, diag_checks;
