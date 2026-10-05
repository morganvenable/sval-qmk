"""Characterize defects found in the 2026-10-04 firmware review.

Run from any directory: python3 keyboards/svalboard/docs/reviews/characterize-findings.py
Requires a host C compiler and the checked-out reviewed source. Uses production
functions with mocked hardware; never accesses or writes board flash. Assertions
confirm the observed defects and should fail as those defects are corrected.
This is review evidence, not a correctness test suite for CI.
"""
import importlib.util, pathlib, subprocess, tempfile

root = pathlib.Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('reg', root / 'tests/sval_storage/test_regressions.py')
reg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reg)


def f(path, name):
    import re
    source = (root / path).read_text()
    match = re.search(r'^[a-zA-Z_][^\n;{}]*\b' + name + r'\([^;\n]*\)\s*\{', source, re.M)
    if not match:
        raise ValueError(name)
    start = match.start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


base = reg.PRELUDE + '\n#include <stdio.h>\n'


def run(name, source):
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d)
        (p / 'p.c').write_text(base + source)
        subprocess.run(['cc', '-std=gnu11', '-fsanitize=undefined', '-Wno-unused-parameter', str(p / 'p.c'), '-o', str(p / 'p')], check=True)
        print(name, flush=True)
        subprocess.run([str(p / 'p')], check=True)


run(
    'Session TTL', '''
#define CLIENT_ID_BOOTSTRAP 0
#define CLIENT_ID_ERROR UINT32_MAX
#define CLIENT_WRAPPER_TTL_SECS 120
static uint32_t now; static uint16_t id_counter=42;
uint32_t timer_read32(void){return now;}
''' + f('modules/svalboard/core/client_wrapper.c', 'client_wrapper_allocate_id') + '\n' + f('modules/svalboard/core/client_wrapper.c', 'client_wrapper_valid_id') + '''
int main(void){now=65535; uint32_t id=client_wrapper_allocate_id(); now=131072; printf("ID issued at 65535ms: valid at age 65537ms=%d; advertised TTL=120000ms\\n",client_wrapper_valid_id(id)); assert(!client_wrapper_valid_id(id));}
'''
)
run(
    'Identity validation and failed save', '''
#define IDENTITY_NAME_MAX_BYTES 64
typedef enum {IDENTITY_OK,IDENTITY_INVALID,IDENTITY_UNAVAILABLE,IDENTITY_WRITE_FAILED} identity_status_t;
static struct { char name[64]; uint8_t name_len;} current={.name="old",.name_len=3};
static char name_z[65]="old"; static bool region_ok=true; static int saves;
void identity_init(void){} bool save(void){saves++;return false;}
''' + f('keyboards/svalboard/identity.c', 'utf8_valid') + '\n' + f('keyboards/svalboard/identity.c', 'identity_set_name') + '''
int main(void){uint8_t bad[][4]={{0xC0,0xAF},{0xED,0xA0,0x80},{0xF4,0x90,0x80,0x80}}; uint8_t n[]={2,3,4};for(int i=0;i<3;i++){printf("invalid UTF8 case %d accepted=%d\\n",i,utf8_valid(bad[i],n[i]));assert(utf8_valid(bad[i],n[i]));} int first=identity_set_name("new",3),second=identity_set_name("new",3); printf("failed name save: first=%d retry=%d flash attempts=%d visible name=%s\\n",first,second,saves,name_z);assert(first==IDENTITY_WRITE_FAILED && second==IDENTITY_OK && saves==1 && !strcmp(name_z,"old"));}
'''
)
run(
    'Alt repeat modifier and default semantics', '''
#include "''' + str(root / 'quantum/keycodes.h') + '''"
#define MOD_BIT(kc) (1u << ((kc) & 7))
#define SVAL_ALT_REPEAT_KEY_ENTRIES 1
#define sval_ark_option_default_to_alt 1
#define sval_ark_option_bidirectional 2
#define sval_ark_option_ignore_mod_handedness 4
static bool sval_ark_entry_enabled[]={true};
typedef struct { uint16_t keycode,alt_keycode; uint8_t allowed_mods,options;} sval_alt_repeat_key_entry_t;
static sval_alt_repeat_key_entry_t sval_alt_repeat_entries[]={{KC_A,KC_B,0,2}};
''' + '\n'.join(f('modules/svalboard/core/sval_alt_repeat_key.c', n) for n in ['sval_get_alt_repeat_keycode', 'sval_get_reverse_alt_repeat_keycode', 'get_alt_repeat_key_keycode_user']) + '''
int main(void){
 uint16_t forward=get_alt_repeat_key_keycode_user(KC_A,MOD_BIT(KC_LCTL));
 uint16_t reverse=get_alt_repeat_key_keycode_user(KC_B,MOD_BIT(KC_LCTL));
 printf("allowed_mods=0 with Ctrl: forward=%u reverse=%u (both should fall through)\\n",forward,reverse);
 assert(forward==KC_B && reverse==KC_A);
 sval_alt_repeat_entries[0].allowed_mods=MOD_BIT(KC_LSFT);
 uint16_t plain=get_alt_repeat_key_keycode_user(KC_A,0);
 printf("allowing Shift incorrectly requires it: plain A maps to %u (transparent)\\n",plain);
 assert(plain==KC_TRANSPARENT);
 sval_alt_repeat_entries[0].allowed_mods=0;
 sval_alt_repeat_entries[0].options=sval_ark_option_default_to_alt;
 uint16_t match=get_alt_repeat_key_keycode_user(KC_A,0),fallback=get_alt_repeat_key_keycode_user(KC_C,0);
 printf("default-to-alt: A maps to %u (itself), unrelated C maps to %u (transparent)\\n",match,fallback);
 assert(match==KC_A && fallback==KC_TRANSPARENT);
}
'''
)
run(
    'Tap dance editing during a hold', '''
#define SVAL_TAP_DANCE_ENTRIES 1
#define TD_ENABLED(e) ((e).custom_tapping_term & 0x8000)
#define TAP_CODE_DELAY 0
enum {SINGLE_TAP=1,SINGLE_HOLD,DOUBLE_TAP,DOUBLE_HOLD,DOUBLE_SINGLE_TAP,MORE_TAPS};
typedef struct {uint8_t count;bool interrupted,pressed;} tap_dance_state_t;
typedef struct {uint16_t on_tap,on_hold,on_double_tap,on_tap_hold,custom_tapping_term;} sval_tap_dance_entry_t;
static sval_tap_dance_entry_t td_entry,stored={4,0,0,0,0x8000};static uint8_t dance_state[1];static bool held[256];
int sval_get_tap_dance(uint16_t i,sval_tap_dance_entry_t *e){*e=stored;return 0;}
void sval_keycode_down(uint16_t k){held[k]=true;}void sval_keycode_up(uint16_t k){held[k]=false;}void sval_keycode_tap(uint16_t k){}void wait_ms(int n){}
''' + '\n'.join(f('modules/svalboard/core/sval_tap_dance.c', n) for n in ['dance_step', 'on_dance_finished', 'on_dance_reset']) + '''
int main(void){tap_dance_state_t s={1,false,true};on_dance_finished(&s,0); stored.on_tap=5;on_dance_reset(&s,0);printf("A remains held after editing dance to B before reset=%d\\n",held[4]);assert(held[4]);}
'''
)
run(
    'Sniper divisor overflow', '''
#include "''' + str(root / 'keyboards/svalboard/axis_scale.h') + '''"
static uint8_t sniper_hold_2=1,sniper_hold_3=1,sniper_hold_5=1;static bool sniper_toggle_2=true,sniper_toggle_3=true,sniper_toggle_5=true;
static axis_scale_t sniper_x={1,2,0},sniper_y={1,2,0},sniper_h={1,2,0},sniper_v={1,2,0};
''' + f('keyboards/svalboard/axis_scale.c', 'set_div_axis') + '\n' + f('keyboards/svalboard/keymaps/keymap_support.c', 'update_sniper_divisor') + '''
int main(void){update_sniper_divisor();printf("2x,3x,5x hold plus toggle: divisor=%u instead of 900\\n",sniper_x.div);assert(sniper_x.div==132);}
'''
)
kp = (root / 'keyboards/svalboard/keymaps/keymap_support.c').read_text()
prefix = kp[kp.index('#define SCROLL_FREQUENCY_MS'):kp.index('report_mouse_t pointing_device_task_combined_user')]
prefix = prefix.replace(f('keyboards/svalboard/keymaps/keymap_support.c', 'process_detected_host_os_kb'), '')
run(
    'Scroll tail flush', '''
#include "''' + str(root / 'keyboards/svalboard/axis_scale.h') + '''"
typedef struct {int16_t x,y,h,v;uint8_t buttons;} report_mouse_t;
static struct {bool left_scroll,right_scroll,left_automouse,right_automouse,axis_scroll_lock,natural_scroll;uint8_t automouse_decay;uint16_t automouse_threshold;} global_saved_values={.left_scroll=true};
static uint16_t now;
uint16_t timer_read(void){return now;}uint16_t timer_elapsed(uint16_t t){return now-t;}
void mouse_mode(bool on){}int16_t get_left_dpi(void){return 800;}int16_t get_right_dpi(void){return 800;}
report_mouse_t pointing_device_combine_reports(report_mouse_t a,report_mouse_t b){a.h+=b.h;a.v+=b.v;return a;}
report_mouse_t pointing_device_task_user(report_mouse_t a){return a;}
''' + f('keyboards/svalboard/axis_scale.c', 'set_div_axis') + '\n' + f('keyboards/svalboard/axis_scale.c', 'add_to_axis') + '\n' + f('keyboards/svalboard/axis_scale.c', 'set_mult_axis') + '\n' + prefix +
    f('keyboards/svalboard/keymaps/keymap_support.c', 'pointing_device_task_combined_user') + '''
int main(void){report_mouse_t a={.y=-8},zero={0};now=1;pointing_device_task_combined_user(a,zero);now=20;report_mouse_t out=pointing_device_task_combined_user(zero,zero);printf("idle frame after deadline: wheel output=%d buffered ticks=%d\\n",out.v,scroll_accumulator_v);assert(!out.v && scroll_accumulator_v>0);}
'''
)
h = (root / 'modules/svalboard/pointing_device_ps2/pointing_device_ps2.h').read_text()
type_decl = h[h.index('typedef struct __attribute__'):h.index('// Default configuration')]
run(
    'PS2 button hold', '''
#define PS2_MOUSE_X_MULTIPLIER 1
#define PS2_MOUSE_Y_MULTIPLIER 1
#define MOUSE_BTN1 1
#define MOUSE_BTN2 2
#define MOUSE_BTN3 4
typedef struct {int16_t x,y,h,v;uint8_t buttons;} report_mouse_t;
static bool has=true;static unsigned byte;
bool pbuf_has_data(void){return has;}uint8_t ps2_host_recv_response(void){return byte++==0?9:0;}
''' + type_decl + f('modules/svalboard/pointing_device_ps2/pointing_device_ps2.c', 'ps2_mouse_convert_report_to_hid') + '\n' + f('modules/svalboard/pointing_device_ps2/pointing_device_ps2.c', 'ps2_mouse_get_report_core') + '''
int main(void){report_mouse_t zero={0};report_mouse_t first=ps2_mouse_get_report_core(zero);has=false;report_mouse_t next=ps2_mouse_get_report_core(first);printf("physical button held: initial buttons=%u next poll without packet=%u\\n",first.buttons,next.buttons);assert(first.buttons==1 && next.buttons==0);}
'''
)
run(
    'Reset validity and subsequent boot', '''
#define SVAL_EEPROM_SIZE 128
static uint8_t mem[128];static int keymap_resets;static uint16_t keymap_value=42;
void sval_write_eeprom(uint16_t i,void *v,uint16_t n){memcpy(mem+i,v,n);}
void dynamic_keymap_reset(void){keymap_resets++;keymap_value=0;}void dynamic_keymap_macro_reset(void){}
void sval_reload_tap_dance(void){}void sval_reload_combo(void){}void sval_reload_key_override(void){}void sval_reload_alt_repeat_key(void){}void sval_reload_leader(void){}void sval_reload_labels(void){}
void client_wrapper_init(void){}bool sval_eeprom_is_valid(void){return mem[100]==0xA5;}
void sval_qmk_settings_reset(void){}void sval_qmk_settings_init(void){}void sval_eeprom_set_valid(void){mem[100]=0xA5;}
''' + f('modules/svalboard/core/sval.c', 'sval_reset') + '\n' + f('modules/svalboard/core/sval.c', 'sval_init') + '''
int main(void){sval_eeprom_set_valid();sval_reset();assert(!sval_eeprom_is_valid());keymap_value=7;sval_init();printf("reset then boot: keymap reset calls=%d (second reset erases post-reset edits)\\n",keymap_resets);assert(keymap_resets==2 && keymap_value==0);}
'''
)
run(
    'Boost motion arithmetic', '''
#include "''' + str(root / 'keyboards/svalboard/axis_scale.h') + '''"
''' + f('keyboards/svalboard/axis_scale.c', 'add_to_axis') + '''
int main(void){axis_scale_t a={255,1,0};int16_t out=add_to_axis(&a,200);printf("positive input 200 at boost 255 becomes %d rather than a saturated positive report\\n",out);assert(out<0);}
'''
)
run(
    'Disable auto mouse while active', '''
#define MH_AUTO_BUTTONS_LAYER 5
static struct {bool auto_mouse;} global_saved_values={false};static bool mouse_mode_enabled=true;static uint16_t mh_auto_buttons_timer;static int mouse_keys_pressed;static uint32_t layer_state=1u<<5;
void layer_on(int l){layer_state|=1u<<l;}void layer_off(int l){layer_state&=~(1u<<l);}uint16_t timer_read(void){return 1;}
''' + f('keyboards/svalboard/keymaps/keymap_support.c', 'mouse_mode') + '''
int main(void){mouse_mode(false);printf("auto mouse flag disabled while active: mouse layer remains=%d\\n",!!(layer_state & (1u<<5)));assert(layer_state & (1u<<5));}
'''
)
run(
    'Migration macro no-op encoding', '''
#define SS_QMK_PREFIX 1
#define SS_TAP_CODE 1
#define SS_DOWN_CODE 2
#define SS_UP_CODE 3
#define SS_DELAY_CODE 4
#define VIAL_MACRO_EXT_TAP 5
#define VIAL_MACRO_EXT_UP 7
#define VIAL_SV_FIRST 0x7E00
#define VIAL_SV_COUNT 20
#define VIAL_KB_LAST 0x7FFF
#define QK_USER_0 0x7E40
#define KC_NO 0
''' + f('keyboards/svalboard/migrate_vial.c', 'translate_keycode') + '\n' + f('keyboards/svalboard/migrate_vial.c', 'translate_macros') + '''
int main(void){uint8_t b[]={1,5,0x14,0x7E,'X',0,'Y',0};translate_macros(b,sizeof(b));printf("unsupported custom action translated to bytes %02X %02X: new internal NUL=%d\\n",b[2],b[3],b[2]==0);assert(b[2]==0);}
'''
)

run(
    'Migration settings reset side effect', '''
#define TAPPING_TERM 200
#define NKRO_ENABLE
typedef struct __attribute__((packed)) {
    uint16_t auto_shift_timeout;
    uint16_t osk_timeout;
    uint16_t mousekey_delay;
    uint16_t mousekey_interval;
    uint16_t mousekey_move_delta;
    uint16_t mousekey_max_speed;
    uint16_t mousekey_time_to_max;
    uint16_t mousekey_wheel_delay;
    uint16_t mousekey_wheel_interval;
    uint16_t mousekey_wheel_max_speed;
    uint16_t mousekey_wheel_time_to_max;
    uint16_t combo_term;
    uint16_t tapping_term;
    uint8_t  grave_esc_override;
    uint8_t  auto_shift;
    uint8_t  osk_tap_toggle;
    uint8_t  tapping_v2;
    uint16_t tap_code_delay;
    uint16_t tap_hold_caps_delay;
    uint8_t  tapping_toggle;
    uint8_t  unused;
    uint16_t quick_tap_term;
    uint16_t flow_tap_term;
    uint16_t leader_timeout;
    uint8_t  leader_options;  // bit 0 = per-key timing
    uint8_t  unused2;
} sval_qmk_settings_t;
typedef union keymap_config_t {
    uint16_t raw;
    struct {
        bool swap_control_capslock : 1;
        bool capslock_to_control : 1;
        bool swap_lalt_lgui : 1;
        bool swap_ralt_rgui : 1;
        bool no_gui : 1;
        bool swap_grave_esc : 1;
        bool swap_backslash_backspace : 1;
        bool nkro : 1;
        bool swap_lctl_lgui : 1;
        bool swap_rctl_rgui : 1;
        bool oneshot_enable : 1;
        bool swap_escape_capslock : 1;
        bool autocorrect_enable : 1;
    };
} keymap_config_t;


static sval_qmk_settings_t settings;static keymap_config_t keymap_config;static uint16_t saved;
void sval_qmk_settings_save(void){}void sval_qmk_settings_apply(void){}void clear_keyboard(void){}void eeconfig_update_keymap(keymap_config_t *k){saved=k->raw;}
''' + f('modules/svalboard/core/sval_qmk_settings.c', 'sval_qmk_settings_reset') + '''
int main(void){keymap_config.raw=0x101;saved=keymap_config.raw;sval_qmk_settings_reset();printf("copied magic options 0101 become %04X when migration calls real settings reset\\n",saved);assert(!(saved & 0x101));}
'''
)

run(
    'Interrupted keycode upgrade at byte boundary', '''
#include "''' + str(root / 'quantum/keycode_upgrade.h') + '''"
#define DYNAMIC_KEYMAP_LAYER_COUNT 1
#define MATRIX_ROWS 1
#define MATRIX_COLS 1
static uint8_t persisted[2]={0x74,0xF0},version=7;
static int writes;static bool interrupt_write=true;static jmp_buf power_cut;
void *dynamic_keymap_key_to_eeprom_address(uint8_t l,uint8_t r,uint8_t c){return persisted;}
uint8_t eeprom_read_byte(const void *p){return *(const uint8_t *)p;}
void eeprom_update_byte(void *p,uint8_t v){*(uint8_t *)p=v;writes++;if(interrupt_write)longjmp(power_cut,1);}
''' + '\n'.join(f('quantum/nvm/eeprom/nvm_dynamic_keymap.c', n) for n in ['nvm_dynamic_keymap_read_keycode', 'nvm_dynamic_keymap_update_keycode']) + '''
uint16_t dynamic_keymap_get_keycode(uint8_t l,uint8_t r,uint8_t c){return nvm_dynamic_keymap_read_keycode(l,r,c);}
void dynamic_keymap_set_keycode(uint8_t l,uint8_t r,uint8_t c,uint16_t k){nvm_dynamic_keymap_update_keycode(l,r,c,k);}
void via_init_kb(void){}void via_set_layout_options_kb(uint32_t o){}uint32_t via_get_layout_options(void){return 0;}
bool via_eeprom_is_valid(void){return true;}void eeconfig_init_via(void){assert(false);}
uint8_t nvm_via_read_keycodes_version(void){return version;}void nvm_via_update_keycodes_version(uint8_t v){version=v;}
void via_keycodes_upgrade_kb(uint8_t from){}
''' + f('quantum/dynamic_keymap.c', 'dynamic_keymap_upgrade_keycodes') + '\n' + f('quantum/via.c', 'via_init') + '''
int main(void){
 assert(keycode_upgrade(0x74F0,7)==QK_STENO_MODE_BOLT);
 if(!setjmp(power_cut))via_init();
 assert(version==7 && dynamic_keymap_get_keycode(0,0,0)==0x75F0);
 interrupt_write=false;via_init();
 printf("power cut after first byte: reboot retains %04X instead of %04X, stamps version %u, total byte writes=%d\\n",dynamic_keymap_get_keycode(0,0,0),QK_STENO_MODE_BOLT,version,writes);
 assert(dynamic_keymap_get_keycode(0,0,0)==0x75F0 && version==KEYCODE_UPGRADE_CURRENT && writes==1);
}
'''
)
