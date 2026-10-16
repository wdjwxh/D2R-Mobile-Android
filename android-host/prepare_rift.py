"""Generate the upstream Rift core without enabling runtime hooks.

The generated objects deliberately retain strong references to save transaction,
observation and UI integration. Missing integration must fail linking, not silently
pretend to save. Run separately until the complete runtime chain is integrated.
"""
from pathlib import Path
import hashlib
import json
import re

root = Path(__file__).resolve().parent.parent
up = root / 'd2r-mobile/dm/host'
out = root / 'android-host/generated/rift'
out.mkdir(parents=True, exist_ok=True)
manifest = {}
for name in ('dm_rift.c', 'dm_bossrush.c', 'dm_rift_ui.c', 'dm_rift.h', 'dm_bossrush.h',
             'dm_unit.h', 'dm_doorspot.h', 'dm_hud.h', 'dm_i18n.h', 'dm_cursor.h',
             'dm_widget.h', 'dm_input.h'):
    source = (up / name).read_bytes()
    text = source.decode('utf-8').replace('\r\n', '\n')
    text = text.replace('#include "hle.h"', '#include "../../src/rift_compat.h"')
    text = text.replace('#include "dm.h"', '#include "../../src/dm_host.h"')
    text = text.replace('st_mtimespec', 'st_mtim')
    text = text.replace('#include <TargetConditionals.h>', '#define TARGET_OS_OSX 0\n#define TARGET_OS_IPHONE 0')
    # Android enables this explicitly only after tick/C2S/UI and transaction
    # integration is ready. Merely linking the core must not install save hooks.
    text = text.replace('__attribute__((constructor)) static void rec_hook_install(void)',
                        'void dm_android_rift_install_save_hook(void)')
    (out / name).write_text(text, encoding='utf-8')
    manifest[name] = hashlib.sha256(source).hexdigest()
# Reuse the actual autosave request and event-message logic from diag.c. Keep
# these separate from HUD sampling so Android can compose its OBS_TICK owner.
diag_bytes = (up / 'diag.c').read_bytes()
diag = diag_bytes.decode('utf-8').replace('\r\n', '\n')
def function(name, source=None):
    source = diag if source is None else source
    match = re.search(r'^(?:static )?(?:void|int) ' + name + r'\([^\n]*\) \{', source, re.M)
    if not match:
        raise RuntimeError('Upstream function changed: ' + name)
    start = match.start()
    line_end = source.index('\n', match.end())
    if '}' in source[match.end():line_end].split('//', 1)[0]:
        return source[start:line_end] + '\n'
    return source[start:source.index('\n}', match.end()) + 2] + '\n'
support = '''/* Generated from upstream diag.c; attach explicitly after integration. */
#include "../../src/rift_compat.h"
#include <stdatomic.h>
int dm_tg_vm_read(uint64_t, void *, size_t);
static int tk_rd(uint64_t a, void *p, size_t n) { return dm_tg_vm_read(a,p,n); }
static uint64_t g_lastG;
static _Atomic int g_quiet_diff;
void dm_android_rift_observe_game(uint64_t game) { g_lastG = game; }
'''
for name in ('game_live', 'in_game', 'dm_request_save', 'dm_diag_difficulty', 'dm_quiet_difficulty_msg', 'event_msg_obs'):
    support += function(name)
support += 'void dm_android_rift_attach_events(void) { if (gate_obs_present(15)) gate_obs_set(15,event_msg_obs); }\n'
support = support.replace('tk_rd(', 'rift_diag_read(')
(out / 'rift_diag.c').write_text(support, encoding='utf-8')
manifest['diag.c'] = hashlib.sha256(diag_bytes).hexdigest()
time_bytes = (up / 'hle_time.c').read_bytes()
time_source = time_bytes.decode('utf-8').replace('\r\n', '\n')
tz = function('tz_site', time_source)
# Keep upstream's instruction pattern, but safely probe translated callers.
tz = tz.replace('uint32_t w = *(const uint32_t *)(uintptr_t)(lr + 4 * k);',
                'uint32_t w; if (!dm_tg_vm_read(lr + 4 * k, &w, sizeof w)) return 0;')
time_adapter = '''/* Upstream terror-zone call-site policy, Android clock adapter. */
#include "../../src/rift_compat.h"
#include <stdatomic.h>
int dm_tg_vm_read(uint64_t,void *,size_t);
_Atomic int64_t g_dm_tz_time;
_Atomic uint64_t g_dm_tz_live_ns;
''' + tz + '''
int64_t dm_android_tz_now(uint64_t caller) {
    int64_t tz = atomic_load(&g_dm_tz_time);
    if (tz && clock_gettime_nsec_np(CLOCK_MONOTONIC) < atomic_load(&g_dm_tz_live_ns) && tz_site(caller)) return tz;
    return (int64_t)time(NULL);
}
'''
(out / 'rift_time.c').write_text(time_adapter, encoding='utf-8')
manifest['hle_time.c'] = hashlib.sha256(time_bytes).hexdigest()
(out / 'upstream-sha256.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print('Upstream Rift and boss rush prepared (runtime integration required)')
