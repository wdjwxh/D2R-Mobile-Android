"""Import only build inputs from an iOS app you built from your own game files.

No download, network access, game keys or save import. All outputs are ignored.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ios-app', type=Path, required=True, help='Your locally built DmIOS.app directory')
a = p.parse_args()
app = a.ios_app.resolve()
guest = app / 'Frameworks/Guest.framework/Guest'
tables = [f'{name}.{kind}.tsv' for name in ('main', 'sdk') for kind in ('fixups', 'gates')]
arts = sorted(app.glob('dm_native_*.png')) + [app / 'dm_touch_joy.png']
required = [guest, app/'dm/derived.tsv', app/'hud_v4_ui', app/'romfs_overlay/data/.build.config']
required += [app/'dm'/name for name in tables] + arts
missing = [str(x) for x in required if not x.exists()]
if not list(app.glob('dm_native_*.png')):
    missing.append('dm_native_*.png')
if missing:
    p.error('Missing local inputs:\n' + '\n'.join(missing))
dest = root/'private/ios-app'
dm = root/'android-work/dm-tables'
modules = root/'android-work/modules'
for d in (dest/'dm', dm, modules): d.mkdir(parents=True, exist_ok=True)
for name in tables: shutil.copy2(app/'dm'/name, dm/name)
shutil.copy2(app/'dm/derived.tsv', dest/'dm/derived.tsv')
for image in arts: shutil.copy2(image, dest/image.name)
for name in ('hud_v4_ui', 'romfs_overlay'):
    shutil.copytree(app/name, dest/name, dirs_exist_ok=True)
subprocess.run([sys.executable, str(root/'extract_guest.py'), str(guest), str(dm), str(modules)], check=True)
for name in ('main', 'sdk'):
    subprocess.run([sys.executable, str(root/'patch_android.py'), str(modules/f'{name}.patched.bin'),
                    str(dm/f'{name}.fixups.tsv'), str(modules/f'{name}.android.bin')], check=True)
print('Private build inputs prepared. Nothing has been added to Git.')
