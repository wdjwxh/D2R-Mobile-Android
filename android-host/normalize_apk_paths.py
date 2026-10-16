"""Canonical ZIP paths before zipalign/signing (Windows aapt2 uses backslashes)."""
import copy
import os
import shutil
import sys
import zipfile
from pathlib import Path

path = Path(sys.argv[1])
temporary = path.with_suffix('.paths.tmp')
with zipfile.ZipFile(path) as source:
    if not any('\\' in entry.orig_filename for entry in source.infolist()):
        sys.exit(0)
    names = set()
    with zipfile.ZipFile(temporary, 'w') as target:
        for entry in source.infolist():
            normalized = copy.copy(entry)
            normalized.filename = entry.orig_filename.replace('\\', '/')
            normalized.orig_filename = normalized.filename
            if normalized.filename in names:
                raise ValueError(f'Duplicate APK path: {normalized.filename}')
            names.add(normalized.filename)
            with source.open(entry) as reader, target.open(normalized, 'w') as writer:
                shutil.copyfileobj(reader, writer, 1024 * 1024)
os.replace(temporary, path)
print('Normalized APK directory separators')
