"""Compile production read handlers and wrapper; no device or flash is accessed."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'modules/svalboard/core'


def function(source, name):
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def harness():
    source = (CORE / 'sval.c').read_text()
    header = (CORE / 'sval.h').read_text()
    declarations = '\n'.join(re.findall(r'enum \w+\s*\{.*?\};', header, re.S))
    version = re.search(r'#define SVAL_PROTOCOL_VERSION[^\n]+', header).group()
    cases = ''
    for command in ('get_info', 'layer_state_get'):
        start = source.index('        case sval_cmd_' + command + ':')
        end = source.index('\n        case ', start + 1)
        cases += source[start:end]
    wrapper = '\n'.join(line for line in (CORE / 'client_wrapper.c').read_text().splitlines() if not line.startswith('#include'))
    return r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "client_wrapper.h"
#define SVAL_PREFIX 0xDF
#define SVAL_KEYBOARD_UID {1,2,3,4,5,6,7,8}
static uint32_t layer_state, default_layer_state;
static uint8_t sent[32];
static uint32_t timer_read32(void) { return 100; }
static void host_raw_hid_send(uint8_t *p,uint8_t n) { assert(n==32); memcpy(sent,p,n); }
''' + declarations + '\n' + version + '\n' + function(
        source, 'sval_get_feature_flags'
    ) + '''
bool sval_handle_command(uint8_t *data,uint8_t length) {
 switch(data[1]) {
''' + cases + '''
 default: assert(false);
 } return true;
}
''' + wrapper + r'''
static uint32_t get32(uint8_t *p) {
 return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void request(uint8_t command) {
 uint8_t packet[32]; memset(packet,0xA5,sizeof(packet));
 packet[0]=0xDD;packet[1]=1;packet[2]=packet[3]=packet[4]=0;
 packet[5]=0xDF;packet[6]=command;
 assert(client_wrapper_receive(packet,sizeof(packet)));
 assert(sent[0]==0xDD && get32(sent+1)==1 && sent[5]==0xDF && sent[6]==command);
}
int main(void) {
 assert(sval_cmd_layer_state_get==0x16);
 assert(sval_flag_default_layer_state==0x40);
 request(sval_cmd_get_info);
 assert(get32(sent+7)==3); // No major version change for an additive capability.
 for(int i=0;i<8;i++)assert(sent[11+i]==i+1);
 assert(sent[19] & 0x40);
#ifdef CAPS_WORD_ENABLE
 assert(sent[19]==0x6F); // Context layers (bit 5) and existing flags remain independent.
#else
 assert(sent[19]==0x60); // Default-layer reporting (bit 6) plus context layers (bit 5).
#endif
 uint32_t masks[]={0,1,2,0x80000000u,0x80000005u,0xFFFFFFFFu};
 for(unsigned a=0;a<sizeof(masks)/sizeof(masks[0]);a++) {
  for(unsigned d=0;d<sizeof(masks)/sizeof(masks[0]);d++) {
   layer_state=masks[a];default_layer_state=masks[d];
   request(sval_cmd_layer_state_get);
   assert(get32(sent+7)==masks[a]); // Existing Keybard decoder's exact offset.
   assert(get32(sent+11)==masks[d]);
   assert(layer_state==masks[a] && default_layer_state==masks[d]);
   for(int i=15;i<32;i++)assert(sent[i]==0xA5);
  }
 }
 for(uint8_t length=2;length<10;length++) {
  uint8_t short_packet[32];memset(short_packet,0xA5,sizeof(short_packet));
  short_packet[0]=0xDF;short_packet[1]=0x16;
  assert(!sval_handle_command(short_packet,length));
  assert(short_packet[1]==0xFF);
  for(int i=2;i<32;i++)assert(short_packet[i]==0xA5);
 }
}
'''


class LayerReportingTests(unittest.TestCase):
    def test_wrapped_masks_capability_legacy_offsets_and_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / 'layers.c'
            source.write_text(harness())
            for flags in ([], ['-DCAPS_WORD_ENABLE', '-DLAYER_LOCK_ENABLE', '-DONESHOT_ENABLE', '-DLEADER_ENABLE']):
                with self.subTest(flags=flags):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-I', str(CORE), *flags, str(source), '-o', str(path / 'layers')], check=True)
                    subprocess.run([str(path / 'layers')], check=True)
