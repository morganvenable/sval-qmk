"""Host regression tests using the production C handlers.

Run: python3 -m unittest discover -s tests/sval_storage -v
Hardware dependencies are mocked; no board or flash is accessed.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fsanitize=undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


PRELUDE = '''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
'''


class StorageRegressions(unittest.TestCase):
    def test_macro_packets_and_storage_bounds(self):
        source = (ROOT / 'modules/svalboard/core/sval.c').read_text()
        start = source.index('        case sval_cmd_macro_buffer_get:')
        end = source.index('        case sval_cmd_label_get:', start)
        handler = 'bool handle(uint8_t *data, uint8_t length) { switch(data[1]) {\n' + source[start:end] + '\n} return true; }'
        run_c(
            PRELUDE + '''
#define DYNAMIC_KEYMAP_MACRO_EEPROM_SIZE 70000u
#define DYNAMIC_KEYMAP_MACRO_EEPROM_ADDR 20000u
#define sval_cmd_macro_buffer_get 31
#define sval_cmd_macro_buffer_set 32
#define sval_cmd_error 255
static uint8_t memory[90000];
static unsigned reads, writes;
uint8_t eeprom_read_byte(void *p) { uintptr_t a=(uintptr_t)p; assert(a>=20000 && a<90000); reads++; return memory[a]; }
void eeprom_update_byte(void *p, uint8_t b) { uintptr_t a=(uintptr_t)p; assert(a>=20000 && a<90000); writes++; memory[a]=b; }
uint32_t dynamic_keymap_macro_get_buffer_size(void) { return 70000; }
''' + function('quantum/nvm/eeprom/nvm_dynamic_keymap.c', 'nvm_dynamic_keymap_macro_read_buffer') + '\n' + function('quantum/nvm/eeprom/nvm_dynamic_keymap.c', 'nvm_dynamic_keymap_macro_update_buffer') + '''
void dynamic_keymap_macro_get_buffer(uint32_t a,uint16_t n,uint8_t *b) { nvm_dynamic_keymap_macro_read_buffer(a,n,b); }
void dynamic_keymap_macro_set_buffer(uint32_t a,uint16_t n,uint8_t *b) { nvm_dynamic_keymap_macro_update_buffer(a,n,b); }
''' + handler + '''
void packet(uint8_t *p,uint8_t cmd,uint32_t offset,uint8_t count) {
 memset(p,0,32); p[1]=cmd; memcpy(p+2,&offset,4); p[6]=count; p[7]=42;
}
int main(void) {
 uint8_t p[32], b[70000];
 for(unsigned cmd=31;cmd<=32;cmd++) {
  packet(p,cmd,UINT32_MAX,2); assert(handle(p,32)); assert(!reads && !writes);
  assert(cmd==31 ? p[6]==0 : p[2]==1);
  packet(p,cmd,69999,2); assert(handle(p,32)); assert(!reads && !writes);
  packet(p,cmd,0,26); assert(handle(p,32)); assert(!reads && !writes);
  packet(p,cmd,0,1); assert(!handle(p,6)); assert(!reads && !writes);
  packet(p,cmd,70000,0); assert(handle(p,32)); assert(!reads && !writes);
 }
 packet(p,32,69999,1); assert(handle(p,32)); assert(writes==1 && memory[89999]==42);
 packet(p,31,69999,1); assert(handle(p,32)); assert(reads==1 && p[7]==42);
 unsigned before=writes;
 nvm_dynamic_keymap_macro_update_buffer(UINT32_MAX,2,p); assert(writes==before);
 nvm_dynamic_keymap_macro_read_buffer(UINT32_MAX,2,p); assert(reads==1 && p[0]==0 && p[1]==0);
 memset(b,7,sizeof(b)); nvm_dynamic_keymap_macro_update_buffer(0,sizeof(b),b);
 memset(b,0,sizeof(b)); nvm_dynamic_keymap_macro_read_buffer(0,sizeof(b),b);
 assert(b[0]==7 && b[65536]==7 && b[69999]==7);
 return 0;
}
'''
        )


if __name__ == '__main__':
    unittest.main()
