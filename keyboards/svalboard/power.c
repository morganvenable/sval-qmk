/*
Copyright 2026 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
// Deep-idle power control for the RP2040: run the core from the USB PLL at
// 48 MHz with the system PLL off, and stop clocking blocks nobody uses while
// the core sits in WFI. USB keeps working because it runs from the USB PLL
// regardless; the ChibiOS system timer and wait_us run from the 1 MHz TIMER
// tick (clk_ref), so scan timing is unaffected. PIO state machines (split
// serial, WS2812) derive their rate from clk_sys, so their dividers are
// rescaled on every switch to keep the baud and LED timing identical.

#include "quantum.h"
#include "svalboard.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/pio.h"
#include "hardware/structs/clocks.h"

#define SVAL_CLK_FULL_HZ 125000000u
#define SVAL_CLK_LOW_HZ   48000000u

static bool     clock_low = false;
static uint32_t pio_div_saved[2][4];

// Scale the divider of every enabled state machine so (clk_sys / div) stays
// constant across a clock change; restore the exact saved values on the way back.
static void pio_reclock(uint32_t from_hz, uint32_t to_hz, bool restore) {
    PIO pios[2] = {pio0, pio1};
    for (int p = 0; p < 2; p++) {
        for (int sm = 0; sm < 4; sm++) {
            if (!(pios[p]->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + sm)))) continue;
            if (restore) {
                pios[p]->sm[sm].clkdiv = pio_div_saved[p][sm];
                continue;
            }
            uint32_t d = pios[p]->sm[sm].clkdiv;
            pio_div_saved[p][sm] = d;
            uint64_t fixed  = d >> 8; // 16.8 fixed-point divider
            uint64_t scaled = (fixed * to_hz + from_hz / 2) / from_hz;
            if (scaled < 0x100) scaled = 0x100; // never below 1.0
            pios[p]->sm[sm].clkdiv = (uint32_t)(scaled << 8);
        }
    }
}

bool    sval_clock_is_low(void) { return clock_low; }
uint8_t sval_clock_mhz(void) { return clock_low ? (SVAL_CLK_LOW_HZ / 1000000u) : (SVAL_CLK_FULL_HZ / 1000000u); }

void sval_clock_low(void) {
    if (clock_low) return;
    // Same sequence as the pico-sdk's set_sys_clock_48mhz().
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, SVAL_CLK_LOW_HZ, SVAL_CLK_LOW_HZ);
    pll_deinit(pll_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, SVAL_CLK_LOW_HZ, SVAL_CLK_LOW_HZ);
    pio_reclock(SVAL_CLK_FULL_HZ, SVAL_CLK_LOW_HZ, false);
    clock_low = true;
}

void sval_clock_full(void) {
    if (!clock_low) return;
    // Same PLL settings as the pico-sdk's clocks_init(): 1500 MHz VCO / 6 / 2 = 125 MHz.
    pll_init(pll_sys, 1, 1500000000u, 6, 2);
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, SVAL_CLK_FULL_HZ, SVAL_CLK_FULL_HZ);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, SVAL_CLK_FULL_HZ, SVAL_CLK_FULL_HZ);
    pio_reclock(SVAL_CLK_LOW_HZ, SVAL_CLK_FULL_HZ, true);
    clock_low = false;
}

uint32_t sval_deep_nap_us(void) {
    return is_keyboard_master() ? SVAL_DEEP_NAP_MASTER_US : SVAL_DEEP_NAP_SLAVE_US;
}

// SLEEP_EN only matters while the core is in WFI. Everything a sleeping board
// still needs (USB, TIMER, the PIOs, DMA, the SPI the sensor is on, SRAM, the
// bus fabric) stays on; blocks this build never uses are gated.
void sval_sleep_gating_init(void) {
    uint32_t en0 = CLOCKS_SLEEP_EN0_CLK_SYS_JTAG_BITS;
    uint32_t en1 = CLOCKS_SLEEP_EN1_CLK_SYS_TBMAN_BITS;
#if !defined(HAL_USE_ADC) || !HAL_USE_ADC
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_ADC_BITS | CLOCKS_SLEEP_EN0_CLK_ADC_ADC_BITS;
#endif
#if !defined(HAL_USE_PWM) || !HAL_USE_PWM
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_PWM_BITS;
#endif
#if !defined(HAL_USE_I2C) || !HAL_USE_I2C
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_I2C0_BITS | CLOCKS_SLEEP_EN0_CLK_SYS_I2C1_BITS;
#endif
#if !defined(HAL_USE_RTC) || !HAL_USE_RTC
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_RTC_BITS | CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS;
#endif
#if !defined(RP_SPI_USE_SPI0) || !RP_SPI_USE_SPI0
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_SPI0_BITS | CLOCKS_SLEEP_EN0_CLK_PERI_SPI0_BITS;
#endif
#if !defined(RP_SPI_USE_SPI1) || !RP_SPI_USE_SPI1
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_SPI1_BITS | CLOCKS_SLEEP_EN0_CLK_PERI_SPI1_BITS;
#endif
#if (!defined(HAL_USE_SERIAL) || !HAL_USE_SERIAL) && (!defined(HAL_USE_SIO) || !HAL_USE_SIO) && (!defined(HAL_USE_UART) || !HAL_USE_UART)
    en1 |= CLOCKS_SLEEP_EN1_CLK_SYS_UART0_BITS | CLOCKS_SLEEP_EN1_CLK_PERI_UART0_BITS | CLOCKS_SLEEP_EN1_CLK_SYS_UART1_BITS | CLOCKS_SLEEP_EN1_CLK_PERI_UART1_BITS;
#endif
    hw_clear_bits(&clocks_hw->sleep_en0, en0);
    hw_clear_bits(&clocks_hw->sleep_en1, en1);
}
