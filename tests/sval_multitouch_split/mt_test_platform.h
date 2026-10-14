#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define RAW_EPSIZE 64
#define KEYBOARD_MT 17
#define I2C_STATUS_TIMEOUT -2
#define I2C_STATUS_SUCCESS 0
typedef int i2c_status_t;
static uint32_t clock_ms;
static bool master, connected, busy;
static uint8_t rpc_data[60];
static bool is_keyboard_master(void) { return master; }
static bool is_keyboard_left(void) { return !master; }
static bool is_transport_connected(void) { return connected; }
static bool updater_active(void) { return busy; }
static uint32_t timer_read32(void) { return clock_ms; }
static uint32_t timer_elapsed32(uint32_t t) { return clock_ms-t; }
static void osalSysLock(void) {}
static void osalSysUnlock(void) {}
static bool transaction_rpc_exec(int id, int n, const void *in, int m, void *out) {
 assert(id==KEYBOARD_MT && n==1 && m==60 && *(const uint8_t *)in==1);
 memcpy(out,rpc_data,60); return connected;
}
static void transaction_register_rpc(int id, void (*fn)(uint8_t,const void *,uint8_t,void *)) { assert(id==KEYBOARD_MT && fn); }
bool azoteq_iqs5xx_mt_config(uint8_t maximum, uint16_t period) { (void)maximum; (void)period; return true; }
