"""Host regression tests using the production C handlers and migration function.

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

    def test_migration_retry_and_commit(self):
        migration = function('keyboards/svalboard/migrate_vial.c', 'sval_migrate_vial')
        identity = (ROOT / 'keyboards/svalboard/identity.h').read_text()
        flags = '\n'.join(line for line in identity.splitlines() if line.startswith('#define IDENTITY_FLAG_'))
        declarations = '''
#define SVAL_LEGACY_STORE_LOGICAL_SIZE 65536
#define EECONFIG_MAGIC ((void*)0)
#define EECONFIG_MAGIC_NUMBER 0x1234
#define EECONFIG_BASE_SIZE 37
#define VIAL_KEYMAP_ADDR 95
#define VIAL_KEYMAP_SIZE 1920
#define VIAL_LAYERS 16
#define VIAL_KEYCODES_VERSION 7
#define VIAL_KB_DATA_ADDR 37
#define VIAL_QMK_SETTINGS_ADDR 2015
#define VIAL_QMK_SETTINGS_SIZE 40
#define VIAL_TAP_DANCE_ADDR 2055
#define VIAL_COMBO_ADDR 2555
#define VIAL_KEY_OVERRIDE_ADDR 3055
#define VIAL_ALT_REPEAT_ADDR 3355
#define VIAL_MACRO_ADDR 3547
#define VIAL_TAP_DANCE_ENTRIES 50
#define VIAL_COMBO_ENTRIES 50
#define VIAL_KEY_OVERRIDE_ENTRIES 30
#define VIAL_ALT_REPEAT_ENTRIES 32
#define SVAL_TAP_DANCE_ENTRIES 256
#define SVAL_COMBO_ENTRIES 256
#define SVAL_KEY_OVERRIDE_ENTRIES 256
#define SVAL_ALT_REPEAT_KEY_ENTRIES 256
#define SVAL_QMK_SETTINGS_OFFSET 0
#define SVAL_TAP_DANCE_OFFSET 0
#define SVAL_COMBO_OFFSET 0
#define SVAL_KEY_OVERRIDE_OFFSET 0
#define SVAL_ALT_REPEAT_KEY_OFFSET 0
struct layer_hsv { uint8_t hue,sat,val; };
'''
        old_source = (ROOT / 'keyboards/svalboard/migrate_vial.c').read_text()
        declarations += old_source[old_source.index('typedef struct'):old_source.index('// Diagnostics')]
        declarations = declarations.replace('_Static_assert(sizeof(vial_saved_values_t) == VIAL_KB_DATA_SIZE, "Vial saved_values is 54 bytes");', '')
        header = (ROOT / 'modules/svalboard/core/sval.h').read_text()
        import re
        for name in ['sval_tap_dance_entry_t', 'sval_combo_entry_t', 'sval_key_override_entry_t', 'sval_alt_repeat_key_entry_t']:
            declarations += re.search(r'typedef struct[^{}]*\{[^}]*\}\s*' + name + ';', header).group() + '\n'
        run_c(
            PRELUDE + flags + '\n' + declarations + '''
struct { uint8_t flags; } current;
static bool save_ok=true, alloc_ok=true, match=true;
static unsigned saved_flags, copies, stamps, allocations;
static uint16_t magic;
static int interrupt_stage;
static jmp_buf restart;
static uint32_t diagnostics[4];
#define MIGRATE_DIAG diagnostics
enum { STAGE_START=1, STAGE_NO_MATCH, STAGE_MATCH, STAGE_SNAPSHOT, STAGE_SVAL, STAGE_KEYMAP, STAGE_MACROS, STAGE_SETTINGS, STAGE_DONE, STAGE_ALREADY_CHECKED };
void identity_init(void) {}
bool save(void) {
 assert(!(current.flags & IDENTITY_FLAG_LEGACY_STORE_CHECKED) || !copies || stamps==3);
 if(!save_ok) return false;
 saved_flags=current.flags; return true;
}
''' + '\n'.join(function('keyboards/svalboard/identity.c', name) for name in ['identity_legacy_store_checked', 'identity_legacy_store_pending', 'identity_mark_legacy_store_pending', 'identity_mark_legacy_store_checked']) + '''
void diag_stage(int stage) { if(stage==interrupt_stage) longjmp(restart,1); }
uint16_t eeprom_read_word(void *p) { return magic; }
void *test_malloc(size_t n) { allocations++; return alloc_ok ? calloc(1,n) : NULL; }
#define malloc test_malloc
void legacy_store_read(uint8_t *p) {}
bool vial_layout_present(uint8_t *p) { return match; }
uint32_t vial_macro_length(uint8_t *p) { return 50; }
void eeprom_update_block(void *p,void *a,unsigned n) { copies++; magic=EECONFIG_MAGIC_NUMBER; }
void eeconfig_init_kb_datablock(void) {}
void via_eeprom_set_valid(bool valid) { if(valid) stamps++; }
void sval_qmk_settings_reset(void) {}
void eeconfig_update_kb_datablock(void *p,unsigned a,unsigned n) {}
uint16_t translate_keycode(uint16_t kc) { return kc; }
void dynamic_keymap_set_buffer(unsigned a,unsigned n,void *p) {}
void translate_macros(void *p,unsigned n) {}
void dynamic_keymap_macro_reset(void) {}
uint32_t dynamic_keymap_macro_get_buffer_size(void) { return 100000; }
uint32_t fit_macros(void *p,uint32_t n,uint32_t room) { return n; }
void dynamic_keymap_macro_set_buffer(unsigned a,unsigned n,void *p) {}
void vial_upgrade(vial_saved_values_t *v) {}
struct { uint8_t left_scroll,right_scroll,axis_scroll_lock,auto_mouse,left_dpi_index,right_dpi_index,mh_timer_index,turbo_scan; struct layer_hsv layer_colors[16]; } global_saved_values;
void svalboard_saved_values_defaults(void) {}
void write_eeprom_kb(void) {}
void sval_eeprom_set_valid(void) { stamps++; }
void svalboard_eeprom_set_valid(void) { stamps++; }
void nvm_via_update_keycodes_version(unsigned n) {}
''' + migration + '''
void reset(void) { current.flags=saved_flags=0; magic=0; copies=stamps=allocations=0; save_ok=alloc_ok=match=true; interrupt_stage=0; }
int main(void) {
 reset(); alloc_ok=false; sval_migrate_vial(); assert(saved_flags==IDENTITY_FLAG_LEGACY_STORE_PENDING);
 magic=EECONFIG_MAGIC_NUMBER; current.flags=saved_flags; alloc_ok=true;
 sval_migrate_vial(); assert(copies==1 && stamps==3 && saved_flags==IDENTITY_FLAG_LEGACY_STORE_CHECKED);
 sval_migrate_vial(); assert(copies==1);
 reset(); magic=EECONFIG_MAGIC_NUMBER; sval_migrate_vial(); assert(!allocations && !copies && saved_flags==IDENTITY_FLAG_LEGACY_STORE_CHECKED);
 reset(); save_ok=false; sval_migrate_vial(); assert(!allocations && !copies && !current.flags);
 reset(); match=false; sval_migrate_vial(); assert(!copies && saved_flags==IDENTITY_FLAG_LEGACY_STORE_CHECKED);
 // Interrupt after the copied core magic, then replay from the preserved store.
 for(int stage=STAGE_SVAL; stage<=STAGE_SETTINGS; stage++) {
  reset(); interrupt_stage=stage;
  if(!setjmp(restart)) sval_migrate_vial();
  assert(copies==1 && magic==EECONFIG_MAGIC_NUMBER && saved_flags==IDENTITY_FLAG_LEGACY_STORE_PENDING);
  current.flags=saved_flags; interrupt_stage=0; sval_migrate_vial();
  assert(copies==2 && stamps==3 && saved_flags==IDENTITY_FLAG_LEGACY_STORE_CHECKED);
 }
 reset(); assert(identity_mark_legacy_store_pending()); save_ok=false;
 identity_mark_legacy_store_checked(); assert(current.flags==IDENTITY_FLAG_LEGACY_STORE_PENDING);
 return 0;
}
'''
        )


if __name__ == '__main__':
    unittest.main()
