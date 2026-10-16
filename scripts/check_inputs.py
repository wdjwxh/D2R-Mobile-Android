"""Fail before rebuilding if required private inputs are missing."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parent.parent
required = [root/'d2r-mobile/dm/host/dm.h', root/'private/ios-app/dm/derived.tsv',
            root/'private/ios-app/romfs_overlay/data/.build.config',
            root/'private/ios-app/hud_v4_ui', root/'private/ios-app/dm_touch_joy.png']
for name in ('main', 'sdk'):
    required += [root/f'android-work/modules/{name}.android.bin']
    required += [root/f'android-work/dm-tables/{name}.{kind}.tsv' for kind in ('fixups', 'gates')]
missing = [str(x.relative_to(root)) for x in required if not x.exists()]
if not list((root/'private/ios-app').glob('dm_native_*.png')): missing.append('private/ios-app/dm_native_*.png')
if not list((root/'android-work/shaders/packs').glob('*.bin')): missing.append('android-work/shaders/packs/*.bin')
if not list((root/'android-work/shaders/spv-adreno').glob('*.spv')): missing.append('android-work/shaders/spv-adreno/*.spv')
if not list((root/'android-work/shaders/spv-adreno').glob('*.res.json')): missing.append('android-work/shaders/spv-adreno/*.res.json')
if missing:
    print('Private inputs missing; see docs/BUILD.md and docs/GAME_FILES.md:\n- ' + '\n- '.join(missing), file=sys.stderr)
    raise SystemExit(1)
print('Required local input paths exist (not a complete content/version validation).')
