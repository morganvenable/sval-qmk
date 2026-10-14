// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include <cstring>
extern "C" {
#include "azoteq_iqs5xx.h"
}

static uint8_t source[45], captured[45];
static uint16_t address, length, end_address, read_timeout;
static int reads, ends;
static bool valid;
static i2c_status_t read_status, end_status;
static i2c_status_t observed_read, observed_end;
static bool recover_second, retried, recovered;
static uint32_t error_flags;
static int fault_calls;

extern "C" {
void i2c_init(void) {}
uint32_t sval_mt_i2c_error_flags(void) { return error_flags; }
void sval_mt_acquisition_fault(uint32_t, bool retry, bool ok) { fault_calls++; retried=retry; recovered=ok; }
i2c_status_t i2c_ping_address(uint8_t, uint16_t) { return I2C_STATUS_SUCCESS; }
i2c_status_t i2c_read_register16(uint8_t, uint16_t reg, uint8_t *data, uint16_t len, uint16_t timeout) {
    reads++; if (recover_second && reads==2) read_status=I2C_STATUS_SUCCESS; address = reg; length = len;
    read_timeout = timeout;
    if (read_status == I2C_STATUS_SUCCESS) memcpy(data, source, len);
    return read_status;
}
i2c_status_t i2c_write_register16(uint8_t, uint16_t reg, const uint8_t *, uint16_t, uint16_t) {
    ends++; end_address = reg; return end_status;
}
void sval_mt_sample(const uint8_t *raw, bool ok) { memcpy(captured, raw, 45); valid = ok; }
void sval_mt_read_status(i2c_status_t read, i2c_status_t end, uint32_t) { observed_read = read; observed_end = end; }
uint8_t pointing_device_handle_buttons(uint8_t buttons, bool pressed, pointing_device_buttons_t button) {
    return pressed ? buttons | (1 << button) : buttons & ~(1 << button);
}
}

class AzoteqMultitouch : public testing::Test {
    void SetUp() override {
        memset(source, 0, sizeof(source)); memset(captured, 0, sizeof(captured));
        recover_second=retried=recovered=false; error_flags=4; fault_calls=0;
        reads = ends = 0; read_status = end_status = I2C_STATUS_SUCCESS; valid = false;
    }
};

TEST_F(AzoteqMultitouch, OneCoherentReadIncludesFifthSlotAndClosesOnce) {
    source[5] = 1; source[7] = 12; source[9] = 34;
    source[38] = 0x06; source[39] = 0x23; source[42] = 0x01; source[43] = 0x80; source[44] = 7;
    report_mouse_t report = azoteq_iqs5xx_get_report((report_mouse_t){0});
    EXPECT_EQ(reads, 1); EXPECT_EQ(address, 0x000C); EXPECT_EQ(length, 45);
    EXPECT_EQ(read_timeout, 30);
    EXPECT_EQ(ends, 1); EXPECT_EQ(end_address, 0xEEEE); EXPECT_TRUE(valid);
    EXPECT_EQ(memcmp(captured, source, 45), 0);
    EXPECT_EQ(report.x, 12); EXPECT_EQ(report.y, 34);
}

TEST_F(AzoteqMultitouch, ReadFailureCannotPublishStaleContacts) {
    read_status = -1; memset(source, 0xff, sizeof(source));
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_FALSE(valid); EXPECT_EQ(ends, 0);
    EXPECT_EQ(observed_read, read_status); EXPECT_EQ(observed_end, I2C_STATUS_SUCCESS);
    for (auto value : captured) EXPECT_EQ(value, 0);
}

TEST_F(AzoteqMultitouch, SessionCloseFailureInvalidatesSample) {
    end_status = -1;
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_FALSE(valid); EXPECT_EQ(ends, 1);
    EXPECT_EQ(observed_read, I2C_STATUS_SUCCESS); EXPECT_EQ(observed_end, end_status);
}

TEST_F(AzoteqMultitouch, CpiCannotChangeAbsoluteCoordinateDomain) {
    azoteq_iqs5xx_set_cpi(400);
    EXPECT_EQ(reads, 0); EXPECT_EQ(ends, 0);
}

TEST_F(AzoteqMultitouch, WakeNackRetriesOneCoherentReadWithoutInvalidatingContacts) {
    read_status=I2C_STATUS_ERROR; recover_second=true;
    source[5]=1; source[14]=1;
    azoteq_iqs5xx_base_data_t base={};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base),I2C_STATUS_SUCCESS);
    EXPECT_EQ(reads,2); EXPECT_EQ(ends,1); EXPECT_TRUE(valid);
    EXPECT_TRUE(retried); EXPECT_TRUE(recovered); EXPECT_EQ(fault_calls,1);
    EXPECT_EQ(memcmp(captured,source,45),0);
}
TEST_F(AzoteqMultitouch, PersistentNackStopsAfterOneRetry) {
    read_status=I2C_STATUS_ERROR;
    azoteq_iqs5xx_base_data_t base={};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base),I2C_STATUS_ERROR);
    EXPECT_EQ(reads,2); EXPECT_EQ(ends,0); EXPECT_FALSE(valid);
    EXPECT_TRUE(retried); EXPECT_FALSE(recovered);
}
TEST_F(AzoteqMultitouch, TimeoutAndOtherBusFaultsAreNotRetried) {
    for (auto status : {I2C_STATUS_TIMEOUT,I2C_STATUS_ERROR}) {
        reads=ends=0;error_flags=1;read_status=status;
        azoteq_iqs5xx_base_data_t base={};
        EXPECT_NE(azoteq_iqs5xx_get_base_data(&base),I2C_STATUS_SUCCESS);
        EXPECT_EQ(reads,1); EXPECT_FALSE(retried); EXPECT_FALSE(valid);
    }
}
