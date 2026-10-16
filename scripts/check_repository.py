"""Inspect staged source boundaries. This is NOT a complete game-content scan."""
from pathlib import Path, PurePosixPath
import re
import subprocess

root = Path(__file__).resolve().parent.parent
index = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=root)
bad = []
count = 0
total = 0
blocked = {'.bin','.data','.spv','.spvasm','.vkpack','.metallib','.ipa','.apk','.aab',
           '.so','.dylib','.nsp','.nca','.xci','.nso','.zip','.7z','.ttc','.ttf','.otf',
           '.png','.jpg','.jpeg','.d2s','.d2i','.key','.keys','.pem','.p12','.pfx','.keystore','.jks','.log'}
private = ('private/', 'android-work/', 'android-host/generated/', 'android-host/apk-out/', 'ipa-extract/')
for entry in index.split(b'\0'):
    if not entry: continue
    meta, raw_path = entry.split(b'\t', 1)
    mode, oid, stage = meta.decode().split()
    path = raw_path.decode('utf-8')
    if mode == '160000':
        if path != 'd2r-mobile': bad.append(f'Unexpected submodule: {path}')
        continue
    count += 1
    if path.startswith(private) or PurePosixPath(path).suffix.lower() in blocked:
        bad.append(f'Private/generated/binary path: {path}')
    if PurePosixPath(path).name in ('.env', 'prod.keys', 'title.keys'):
        bad.append(f'Credential path: {path}')
    content = subprocess.check_output(['git', 'cat-file', 'blob', oid], cwd=root)
    total += len(content)
    if len(content) > 2*1024*1024 or b'\0' in content:
        bad.append(f'Unexpected binary/large file: {path}')
    if re.search(rb'(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,}|-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----)', content):
        bad.append(f'Possible credential: {path}')
if bad:
    raise SystemExit('\n'.join(bad))
print(f'Index boundary check passed: {count} text files, {total:,} bytes, upstream gitlink checked.')
print('Manual staged review is still required; this is not a proof of absence of game content.')
