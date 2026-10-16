"""Validate all Android shader modules against the Vulkan 1.1 baseline."""
import argparse
import os
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
parser.add_argument('--validator', default=os.path.join(os.environ.get('D2R_SHADER_TOOLS', ''), 'spirv-val.exe'))
args = parser.parse_args()
files = sorted(args.directory.glob('*.spv'))
if not files:
    raise SystemExit('No SPIR-V modules found')

def validate(path):
    result = subprocess.run([args.validator, '--target-env', 'vulkan1.1', str(path)],
                            capture_output=True, text=True)
    return (path.name, result.stderr) if result.returncode else None

with ThreadPoolExecutor(max_workers=8) as pool:
    errors = [error for error in pool.map(validate, files) if error]
report = {'target': 'vulkan1.1', 'total': len(files), 'failed': len(errors), 'errors': errors}
(args.directory / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'Vulkan 1.1 SPIR-V: {len(files)} modules, {len(errors)} failed')
for name, message in errors[:3]:
    print(name, message)
raise SystemExit(bool(errors))
