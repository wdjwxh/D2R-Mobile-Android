"""Extract exactly the shader package basenames referenced by pinned upstream."""
import argparse
from pathlib import Path, PurePosixPath
import sys

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root))
from tvfs_get import Storage

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--data', type=Path, nargs='+', required=True, help='Your base RomFS data directory/directories')
a = p.parse_args()
storage = Storage(str(root/'private/ios-app/romfs_overlay'), [str(x) for x in a.data])
names = set()
for line in (root/'d2r-mobile/dm/gfx/nvn/shader_map.tsv').read_text(encoding='utf-8').splitlines():
    fields = line.split('\t')
    if len(fields) > 4 and fields[0].endswith('.bin'):
        names.add(PurePosixPath(fields[0]).name)
paths = {}
for value in storage.files:
    name = PurePosixPath(value.decode('latin-1')).name
    if name in names: paths.setdefault(name, []).append(value)
bad = [name for name in sorted(names) if len(paths.get(name, [])) != 1]
if bad:
    p.error('Missing or ambiguous package paths: ' + ', '.join(bad))
out = root/'android-work/shaders/packs'
out.mkdir(parents=True, exist_ok=True)
for name in sorted(names):
    content, _ = storage.lookup(paths[name][0])
    (out/name).write_bytes(content)
print(f'Extracted {len(names)} local shader packages into ignored {out.relative_to(root)}')
