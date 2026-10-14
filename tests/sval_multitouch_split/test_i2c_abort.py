"""Exercise the actual RP HAL abort handler with register fixtures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "platforms/chibios/drivers/sval_rp_i2c_lld.c").read_text()
start = source.index("uint32_t sval_mt_rp_abort_source;")
end = source.index("/**", start)
prefix = r"""
#include <stdint.h>
#include <assert.h>
#define I2C_IC_TX_ABRT_SOURCE_ARB_LOST (1U<<12)
#define I2C_ARBITRATION_LOST 2
#define I2C_ACK_FAILURE 4
#define I2C_BUS_ERROR 1
#define I2C_OVERRUN 8
#define I2C_OVERRUN_ERRORS 0x22
#define _i2c_wakeup_error_isr(p) ((void)(p))
typedef struct {uint32_t TXABRTSOURCE, RAWINTRSTAT, INTRMASK, CLRINTR, CLRTXABRT;} I2C_TypeDef;
typedef struct {I2C_TypeDef *i2c; uint32_t errors;} I2CDriver;

"""
cases = r"""
int main(void) {
  const uint32_t causes[]={1,8,4096,4097,9,0,0x1000001};
  const uint32_t expected[]={4,1,2,6,5,0,4};
  for(unsigned i=0;i<7;i++) {
    I2C_TypeDef regs={.TXABRTSOURCE=causes[i],.INTRMASK=123};
    I2CDriver d={.i2c=&regs};i2c_lld_handle_errors(&d);
    assert(d.errors==expected[i]);assert(regs.INTRMASK==0);
  }
  I2C_TypeDef regs={.TXABRTSOURCE=1,.RAWINTRSTAT=2};
  I2CDriver d={.i2c=&regs};i2c_lld_handle_errors(&d);assert(d.errors==12);
}
"""
with tempfile.TemporaryDirectory() as work:
    cfile = Path(work) / "test.c"
    binary = Path(work) / "test"
    cfile.write_text(prefix + source[start:end] + cases)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("RP I2C abort classification: NACK, FIFO flush count, arbitration, data abort and overrun passed")
