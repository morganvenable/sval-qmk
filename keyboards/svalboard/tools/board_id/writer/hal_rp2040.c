// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The ID-writer on the RP2040: a RAM image the station loads with picotool
// (load -x) into a board in BOOTSEL. The boot ROM enters it at its lowest
// address, either as a function call (PICOBOOT EXEC) or after a reboot into
// RAM; it works the same either way. It leaves its report in RAM and then
// reboots into BOOTSEL through the boot ROM, so the board comes back as the
// same BOOTSEL device and the station reads the report and the record.
//
// Flash goes through the boot ROM's own routines, as identity.c does, with
// one SPI command helper ported from the Pico SDK's flash_do_cmd. There is no
// erase anywhere in this program: the ROM's erase routine is never looked up.

#include "writer.h"

// ---- registers (RP2040 datasheet) -------------------------------------------------
#define REG(a) (*(volatile uint32_t *)(a))
#define RESETS_RESET_CLR 0x4000F000u // RESETS_BASE 0x4000C000 + atomic clear alias 0x3000
#define RESETS_RESET_DONE 0x4000C008u
#define RESET_IO_BANK0 (1u << 5)
#define RESET_IO_QSPI (1u << 6)
#define RESET_PADS_BANK0 (1u << 8)
#define RESET_PADS_QSPI (1u << 9)
#define IO_BANK0_GPIO3_CTRL (0x40014000u + 8u * 3u + 4u)
#define PADS_BANK0_GPIO3 (0x4001C000u + 4u + 4u * 3u)
#define PAD_IE (1u << 6)
#define PAD_DRIVE_4MA (1u << 4)
#define PAD_PUE (1u << 3)
#define PAD_PDE (1u << 2)
#define PAD_SCHMITT (1u << 1)
#define PAD_RESET_VALUE 0x56u // IE, 4 mA, pull-down, Schmitt: the reset state
#define FUNCSEL_SIO 5u
#define FUNCSEL_NULL 0x1Fu
#define SIO_GPIO_IN 0xD0000004u
#define SIO_GPIO_OE_CLR 0xD0000028u
#define IO_QSPI_SS_CTRL (0x40018000u + 8u * 1u + 4u)
#define QSPI_OUTOVER_LSB 8
#define QSPI_OUTOVER_BITS (3u << 8)
#define QSPI_OUTOVER_LOW 2u
#define QSPI_OUTOVER_HIGH 3u
#define SSI_SR 0x18000028u
#define SSI_DR0 0x18000060u
#define SSI_SR_TFNF (1u << 1)
#define SSI_SR_RFNE (1u << 3)
#define XIP_NOCACHE_NOALLOC 0x13000000u
#define GP3 (1u << 3)

// ---- boot ROM -------------------------------------------------------------------------
typedef void *(*rom_lookup_fn)(uint16_t *table, uint32_t code);
// A halfword of the ROM's header (low addresses, which GCC takes for NULL).
static uintptr_t rom_hword(uintptr_t a) {
    uint32_t v;
    __asm volatile("ldrh %0, [%1]" : "=l"(v) : "l"(a));
    return v;
}
#define ROM_HWORD_PTR(a) rom_hword(a)
#define ROM_CODE(a, b) ((uint32_t)(b) << 8 | (uint32_t)(a))

static void *rom_func(uint32_t code) {
    rom_lookup_fn lookup = (rom_lookup_fn)ROM_HWORD_PTR(0x18);
    uint16_t     *table  = (uint16_t *)ROM_HWORD_PTR(0x14);
    return lookup(table, code);
}

typedef void (*rom_void_fn)(void);
typedef void (*rom_program_fn)(uint32_t, const uint8_t *, uint32_t);

static rom_void_fn connect_flash, exit_xip, flush_cache, enter_cmd_xip;

static void unreset(uint32_t bits) {
    REG(RESETS_RESET_CLR) = bits;
    while ((REG(RESETS_RESET_DONE) & bits) != bits) {}
}

static void flash_cmd_mode(void) {
    unreset(RESET_IO_QSPI | RESET_PADS_QSPI); // already out of reset unless entered straight after a reboot
    if (!connect_flash) {
        connect_flash = (rom_void_fn)rom_func(ROM_CODE('I', 'F'));
        exit_xip      = (rom_void_fn)rom_func(ROM_CODE('E', 'X'));
        flush_cache   = (rom_void_fn)rom_func(ROM_CODE('F', 'C'));
        enter_cmd_xip = (rom_void_fn)rom_func(ROM_CODE('C', 'X'));
    }
    __asm volatile("" ::: "memory");
    connect_flash();
    exit_xip();
}

static void flash_read_mode(void) {
    flush_cache();
    enter_cmd_xip(); // serial 03h reads through XIP: enough for the station's reads too
    __asm volatile("" ::: "memory");
}

static void cs_force(uint32_t level) {
    uint32_t v = REG(IO_QSPI_SS_CTRL);
    REG(IO_QSPI_SS_CTRL) = (v & ~QSPI_OUTOVER_BITS) | (level << QSPI_OUTOVER_LSB);
    (void)REG(IO_QSPI_SS_CTRL);
}

// As the Pico SDK's flash_do_cmd, in command mode.
static void flash_do_cmd(const uint8_t *tx, uint8_t *rx, uint32_t count) {
    cs_force(QSPI_OUTOVER_LOW);
    uint32_t tx_left = count, rx_left = count;
    while (tx_left || rx_left) {
        uint32_t sr = REG(SSI_SR);
        if ((sr & SSI_SR_TFNF) && tx_left && rx_left - tx_left < 14) {
            REG(SSI_DR0) = *tx++;
            tx_left--;
        }
        if ((sr & SSI_SR_RFNE) && rx_left) {
            *rx++ = (uint8_t)REG(SSI_DR0);
            rx_left--;
        }
    }
    cs_force(QSPI_OUTOVER_HIGH);
}

static uint8_t jedec;
static uint8_t uid[BOARD_RECORD_UID_BYTES];
static bool    probed;

static void probe(void) {
    if (probed) return;
    probed = true;
    uint8_t tx[13] = {0x9F}, rx[13];
    flash_cmd_mode();
    flash_do_cmd(tx, rx, 4);
    jedec = rx[3];
    tx[0] = 0x4B; // Read Unique ID: 4 dummy bytes, then 8 bytes of ID
    for (uint8_t i = 1; i < sizeof(tx); i++) tx[i] = 0;
    flash_do_cmd(tx, rx, sizeof(tx));
    for (uint8_t i = 0; i < BOARD_RECORD_UID_BYTES; i++) uid[i] = rx[5 + i];
    flash_read_mode();
}

uint8_t hal_jedec_capacity(void) {
    probe();
    return jedec;
}

void hal_unique_id(uint8_t id[BOARD_RECORD_UID_BYTES]) {
    probe();
    for (uint8_t i = 0; i < BOARD_RECORD_UID_BYTES; i++) id[i] = uid[i];
}

const uint8_t *hal_block(void) {
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC + BOARD_RECORD_BASE);
}

void hal_program_page(uint16_t page, const uint8_t *data) {
    if (page >= BOARD_RECORD_PAGES) return;
    uint32_t       offset  = BOARD_RECORD_BASE + (uint32_t)page * BOARD_RECORD_PAGE;
    rom_program_fn program = (rom_program_fn)rom_func(ROM_CODE('R', 'P'));
    if (offset < BOARD_RECORD_BASE || offset + BOARD_RECORD_PAGE > BOARD_RECORD_END) return;
    flash_cmd_mode();
    program(offset, data, BOARD_RECORD_PAGE);
    flash_read_mode();
}

// ---- the GP3 strap ----------------------------------------------------------------

static void spin(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {}
}

static uint8_t sample(uint32_t pad, uint8_t samples) {
    REG(PADS_BANK0_GPIO3) = pad;
    spin(20000); // well over the pull's settling time at any boot ROM clock
    uint8_t high = 0;
    for (uint8_t i = 0; i < samples; i++) {
        if (REG(SIO_GPIO_IN) & GP3) high++;
        spin(200);
    }
    return high;
}

void hal_strap_sample(uint8_t *pu_high, uint8_t *pd_high, uint8_t *np_high, uint8_t samples) {
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0);
    REG(SIO_GPIO_OE_CLR)     = GP3; // input only: the pin is never driven
    REG(IO_BANK0_GPIO3_CTRL) = FUNCSEL_SIO;
    *pu_high                 = sample(PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT | PAD_PUE, samples);
    *pd_high                 = sample(PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT | PAD_PDE, samples);
    *np_high                 = sample(PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT, samples);
    REG(PADS_BANK0_GPIO3)    = PAD_RESET_VALUE;
    REG(IO_BANK0_GPIO3_CTRL) = FUNCSEL_NULL;
}

// ---- the C library calls GCC may emit -----------------------------------------------

void *memset(void *d, int c, unsigned int n);
void *memset(void *d, int c, unsigned int n) {
    volatile uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned int n);
void *memcpy(void *d, const void *s, unsigned int n) {
    volatile uint8_t *p = d;
    const uint8_t    *q = s;
    while (n--) *p++ = *q++;
    return d;
}

// ---- entry ------------------------------------------------------------------------

extern uint8_t __bss_start__[], __bss_end__[];

typedef void (*rom_usb_boot_fn)(uint32_t gpio_activity_mask, uint32_t disable_interface_mask);

void writer_entry(void) __attribute__((section(".entry"), used, noinline, noreturn));
void writer_entry(void) {
    __asm volatile("cpsid i");
    for (uint8_t *p = __bss_start__; p < __bss_end__; p++) *(volatile uint8_t *)p = 0;
    writer_run((const writer_params_t *)WRITER_PARAMS_ADDR, (writer_report_t *)WRITER_REPORT_ADDR);
    // Back to BOOTSEL, both interfaces on. RAM is kept across this reset.
    ((rom_usb_boot_fn)rom_func(ROM_CODE('U', 'B')))(0, 0);
    for (;;) {}
}

// The params block's default: valid magic, no CRC, so an image the station has
// not patched reports BAD_PARAMS and writes nothing.
const writer_params_t writer_params __attribute__((section(".params"), used)) = {.magic = WRITER_PARAMS_MAGIC, .version = 1, .expect = WRITER_EXPECT_ANY};
