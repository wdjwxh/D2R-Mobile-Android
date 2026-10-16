"""Generate upstream touch adapters; only host ABI/includes differ from iOS."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent.parent
up=root/'d2r-mobile/dm/ios'
out=root/'android-host/generated'
out.mkdir(exist_ok=True)
bridge=(up/'Sources/dm_touch_game_bridge.c').read_text(encoding='utf-8')
bridge=bridge.replace('#include "dm_touch_game_state.h"','#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_state.h"')
bridge=bridge.replace('#include "dm_touch_game_identity.h"','#include "../../d2r-mobile/dm/ios/Sources/dm_touch_game_identity.h"')
bridge=bridge.replace('#include "../../host/dm.h"','#include "../src/dm_host.h"')
# The complete off-hand action pump has not yet been wired. Retain a binding
# when its ownership is unknown, exactly as the upstream unreadable-player path.
bridge=bridge.replace('int dm_swapskill_equipped(int32_t); return item!=-1 && dm_swapskill_equipped(item);',
 'int dm_swapskill_equipped(int32_t) __attribute__((weak)); return item!=-1 && (!dm_swapskill_equipped || dm_swapskill_equipped(item));')
(out/'touch_game_bridge.inc').write_text(bridge,encoding='utf-8')
man=json.loads((up/'Sources/TouchArt/native_art_manifest.json').read_text(encoding='utf-8'))
rows=[]
for line in (root/'private/ios-app/dm/derived.tsv').read_text(encoding='utf-8').splitlines():
 if line.startswith('TAROW\t'): rows.append([int(x,0) for x in line.split('\t')[1:]])
assert len(rows)==man['descriptor_rows']
assert hashlib.sha256(json.dumps(rows,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()).hexdigest()==man['descriptor_index_sha256'], 'IPA skill table differs from frozen upstream index'
atl=man['assets'][:9]
text=['/* IPA derived.tsv TAROW verified against upstream frozen descriptor_index_sha256. */',
 '#include "../../d2r-mobile/dm/ios/Sources/dm_touch_native_art.h"',
 'const dm_ta_atlas dm_ta_atlases[DM_TA_ATLASES]={']
for a in atl:text.append(' {"%s",%d,%d,%d,%d,%d},'%(a['asset'][:-4],*a['atlas_px'],a['frame_width'],a['frame_pitch'],a['frame_count']))
text+=['};','static const struct {uint32_t id;int atlas,frame;} rows[]={']
text+=[' {%d,%d,%d},'%tuple(r) for r in rows]
text+=['};','#include "../../d2r-mobile/dm/ios/tools/native_art/implementation.c.inc"']
(out/'touch_native_art.inc').write_text('\n'.join(text)+'\n',encoding='utf-8')
print('Upstream touch bridge and verified %d skill atlas rows prepared'%len(rows))

# Keep HUD readout functions sourced from diag.c without importing diagnostics,
# cheats or unrelated gameplay pumps. Tick timing and guest offsets are unchanged.
diag=(root/'d2r-mobile/dm/host/diag.c').read_text(encoding='utf-8')
def function(name):
 start=diag.index('static ',diag.rfind('\n',0,diag.index(name+'('))+1)
 brace=diag.index('{',start)
 # One-line helpers finish on their own line; multi-line functions use a
 # column-zero closing brace. This avoids counting braces in bilingual comments.
 line_end=diag.index('\n',brace)
 if '}' in diag[brace:line_end].split('//',1)[0]:end=line_end
 else:end=diag.index('\n}',brace)+2
 return diag[start:end]+'\n'
exp=[]
for line in (root/'private/ios-app/dm/derived.tsv').read_text(encoding='utf-8').splitlines():
 if line.startswith('EXP\t'):exp.extend(int(x,0) for x in line.split('\t')[1:])
assert len(exp)==100 and exp[0]==0
parts=['/* Selected readout functions directly generated from upstream host/diag.c. */',
 'static const uint32_t k_exp[100]={'+','.join(str(x)+'u' for x in exp)+'};',
 'static int g_players_applied; static dm_hud_info g_hud;',
 function('main_mod'),function('players_obj'),function('players_apply'),function('stat_read'),
 'typedef struct { uint32_t id,cls,mode,page,x,y;unsigned ord;} state_item;',function('state_read_item'),
 'enum {CLS_TBK=533,CLS_TSC=544};',function('local_player_id')]
start=diag.index('static void hud_tick(')
end=diag.index('    static int logged;',start)
hud=diag[start:end]
# The only omitted lines in the readout prefix are an opt-in death test and
# a pointer published exclusively to the screenshot-burst diagnostic.
hud='\n'.join(l for l in hud.splitlines() if 'static int hp1' not in l and 'atomic_store(&g_player_path' not in l)+'\n'
tail_start=diag.index('    if (main && h.valid) h.quest_unread',end)
tail_end=diag.index('    g_hud = h; dm_hud_publish(&h);',tail_start)
hud+='    static int logged;\n'+diag[tail_start:tail_end]+'    g_hud = h; dm_hud_publish(&h);\n}\n'
parts.append(hud)
(out/'hud_sample.inc').write_text('\n'.join(parts),encoding='utf-8')
print('Upstream HUD readout and 100 experience thresholds prepared')

# Native-touch frontend state machine, copied from dm_cursor.c. Controller-mode
# switching and diagnostic scripts are not part of this Android adapter.
# Quick restart is retained verbatim, including save/exit and name verification.
import re
cursor=(root/'d2r-mobile/dm/host/dm_cursor.c').read_text(encoding='utf-8')

# This stack is also used by the iOS overlay: unknown panels hide the HUD,
# repeated OpenPanel is idempotent, and NPC speech/dialog alone returns -1.
# Keep it in the Android input adapter's translation unit, not a second copy
# of the complete cursor implementation (which is staged separately below).
stack_begin=cursor.index('int dm_ui_panel_resident(')
stack_end=cursor.index('void dm_hud_stack_dump(',stack_begin)
stack=cursor[stack_begin:stack_end]
stack=stack.replace('int dm_ui_panel_resident(', 'static int dm_android_panel_resident(')
stack=stack.replace('dm_ui_panel_resident(', 'dm_android_panel_resident(')
stack=stack.replace('int dm_hud_stack_msg(', 'static int dm_android_hud_stack_msg(')
(out/'touch_hud_stack.inc').write_text(
 '/* Generated upstream panel stack; call only from the guest UI observer under touch_lock. */\n'
 'int dm_ui_panel_shown(const char *name);\n'+stack,encoding='utf-8')
def cursor_function(name):
 match=re.search(r'^(?:static [^\n();{}]+|(?:void|int) )\b'+re.escape(name)+r'\([^;{}]*?\)\s*\{',cursor,re.M)
 assert match,name
 start=match.start();brace=match.end()-1;line_end=cursor.index('\n',brace)
 end=line_end if '}' in cursor[brace:line_end].split('//',1)[0] else cursor.index('\n}',brace)+2
 return cursor[start:end]+'\n'
pieces=['/* Generated touch-mode frontend flow from upstream host/dm_cursor.c. */',
 cursor_function('has_character'),cursor_function('ui_find'),cursor_function('ui_show_ours'),cursor_function('fe_ms'),
 cursor_function('ui_send_mouse_arg'),cursor_function('ui_send_mouse'),cursor_function('fe_key'),cursor_function('fe_send'),
 cursor_function('sel_cur'),cursor_function('sel_keep'),cursor_function('dm_ui_fe_name_wants_input'),cursor_function('dm_ui_text_entered')]
# Keep the entire upstream restart state machine, rather than a platform rewrite.
rs_begin=cursor.index('enum { RS_IDLE, RS_EXIT, RS_FRONT, RS_LOAD, RS_SHOW };')
rs_end=cursor.index('// 按键脚本的',rs_begin)
pieces.append(cursor[rs_begin:rs_end])
# UI messages that drive frontend flow: naming, mode choice, ribbon/list, tutorial.
a=cursor.index('    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterCreateNamePanel"))',cursor.index('void dm_ui_on_uimsg('))
b=cursor.index('    rs_on_uimsg(cat, act, arg);',a)
obs=cursor[a:b]+'    rs_on_uimsg(cat, act, arg);\n'
obs+='    { extern int g_swkbd_name_panel; g_swkbd_name_panel=atomic_load(&g_fe_name_open); }\n'
a=cursor.index('    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "MainMenuButtonRibbon")',b)
b=cursor.index('    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterSelectPanel") && !strcmp(act, "OpenPanel") && atomic_exchange',a)
obs+=cursor[a:b]
a=cursor.index('    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterSelectPanel"))',b)
b=cursor.index('\n}',a)
obs+=cursor[a:b]
pieces+=['void dm_android_frontend_message(const char *cat,const char *act,const char *arg) {\n    sel_keep(cat,act);\n'+obs+'\n}']
a=cursor.index('    static uint64_t next; uint64_t fr = dm_guest_frame();',cursor.index('void dm_ui_frontend_pump('))
b=cursor.index('    // 手柄模式的建角模式页',a)
pump=cursor[a:b]
a=cursor.index('    static int fa = -1;',b);b=cursor.index('    // 手柄模式走原版自己的前端流程',a)
pump+=cursor[a:b]
a=cursor.index('    if (atomic_exchange(&g_fe_skip_tut, 0))',b);b=cursor.index('\n}',a)
pump+=cursor[a:b]
pieces+=['static void dm_ui_frontend_pump(void) {\n    rs_pump(); if (atomic_load(&g_rs) != RS_IDLE) return;\n'+pump+'\n}']
text='\n'.join(pieces).replace('getenv("DM_SAVE")','frontend_save_dir()')
(out/'frontend_flow.inc').write_text(text,encoding='utf-8')
print('Upstream native-touch frontend navigation flow prepared')

# Read-only UI state from upstream. Deliberately exclude the subsequent
# layout mutation/freeze pumps until their complete dependencies are ported.
header=(root/'d2r-mobile/dm/host/dm_cursor.h').read_text(encoding='utf-8')
parts=['/* Generated upstream panel-tree and targeting readout; no layout mutations. */',
 next(l for l in header.splitlines() if l.startswith('enum { DM_PS_BANK')),
 '#define TAB_SEL(w) (*(int32_t *)((w) + 0xdc4))',
 cursor_function('has_visible_leaf'),cursor_function('root_panel_shown'),cursor_function('ui_panel'),
 cursor_function('dm_ui_panel_shown'),
 'static _Atomic int g_pstate, g_targeting_pub;',cursor_function('dm_ui_panel_state'),cursor_function('dm_ui_targeting')]
actions=(root/'d2r-mobile/dm/host/dm_actions.c').read_text(encoding='utf-8')

# v1.0.17 moved held-item truth from the server HUD tick to the client's
# main-thread cursor. Android does not link the full action pump yet, so keep
# this exact readout at the same HID sampling boundary as the panel readout.
a=actions.index('    uint64_t pl =',actions.index('void dm_actions_pump('))
b=actions.index('    test_bindtbl(b, pl);',a)
parts.append('static void dm_android_held_pump(void) {\n'
 '    module_t *m=main_module();if(!m){dm_hud_set_held(0);return;}uint8_t *b=m->base;\n'+
 '    if(!*(uint64_t *)(b+0x2f05458)){dm_hud_set_held(0);return;}\n'+
 actions[a:b]+'}\n')
a=actions.index('        uint64_t fm =',actions.index('void dm_actions_pump('));b=actions.index('        if (s.open)',a)
vendor=actions[a:b].replace('a_panel(', 'ui_panel(')
parts.append('static int dm_android_shop_open(uint64_t root) {\n'
 '    module_t *m=main_module();if(!m)return 0;uint8_t *b=(uint8_t *)m->base;\n'
 '    uint64_t pl=((uint64_t (*)(uint32_t))(b+0x45f9d0))(*(uint32_t *)(b+0x2d20e4c));if(!pl)return 0;\n'
 '    struct {int open;} s={0};\n'+vendor+'    return s.open;\n}')
item=(root/'d2r-mobile/dm/host/dm_cursor_item.c').read_text(encoding='utf-8')
parts.append('int dm_item_targeting(void);')
a=cursor.index('    int st =',cursor.index('static void ui_panel_state_pump('));b=cursor.index('    // 模式切换完成',a)
state=cursor[a:b].replace('dm_shop_open()', 'dm_android_shop_open(root)')
parts.append('static void dm_android_panel_state_pump(void) {\n'
 '    static uint64_t last=UINT64_MAX;uint64_t fr=dm_guest_frame();if(last==fr)return;last=fr;\n'
 '    dm_android_held_pump();\n'
 '    module_t *m=main_module();uint64_t root=0;if(m)memcpy(&root,m->base+0x2f05458,8);\n'
 '    if(!root){atomic_store(&g_pstate,0);atomic_store(&g_targeting_pub,0);return;}\n'+state+
 '    atomic_store(&g_pstate,st);atomic_store(&g_targeting_pub,dm_item_targeting()!=0);\n}')
(out/'ui_state_readout.inc').write_text('\n'.join(parts)+'\n',encoding='utf-8')
print('Upstream UI panel-tree, vendor-open and item-targeting readout prepared')

# Full source imports: game algorithms remain upstream; only includes and the
# platform-selector header change. Item inspection is linked by touch_game;
# the other standalone objects are staged until their real dependencies link.
for name in ('dm_cursor_item', 'dm_actions', 'dm_world', 'dm_uiscreen', 'dm_cursor'):
 source=(root/'d2r-mobile/dm/host'/f'{name}.c').read_text(encoding='utf-8')
 source=source.replace('#include "hle.h"', '#include "../src/dm_host.h"')
 source=source.replace('#include "dm.h"', '#include "../src/dm_host.h"')
 source=source.replace('#include <TargetConditionals.h>', '#define TARGET_OS_OSX 0\n#define TARGET_OS_IOS 1')
 source=re.sub(r'#include "(dm_[^"/]+\.h)"',r'#include "../../d2r-mobile/dm/host/\1"',source)
 prefix = '#include <stdint.h>\n#include <time.h>\nuint64_t dm_guest_frame(void);\n'
 if name == 'dm_cursor': prefix += 'static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }\n'
 if name == 'dm_uiscreen': prefix += 'enum { OBS_UI_PROJECT=8 };\nint gate_obs_present(int);\nvoid gate_obs_set(int,void (*)(const uint64_t *));\n'
 (out/f'{name}_android.c').write_text('/* Generated complete upstream source; Android host ABI only. */\n'+prefix+source,encoding='utf-8')
print('Complete upstream cursor/item/actions/world/uiscreen sources prepared')
