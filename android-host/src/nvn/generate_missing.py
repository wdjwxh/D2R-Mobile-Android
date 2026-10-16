"""Regenerate diagnostic fallbacks from the vendored upstream NVN name list."""
from pathlib import Path

root = Path(__file__).resolve().parent
names = root.joinpath('names.txt').read_text().splitlines()
assert len(names) == len(set(names)) == 523
lines = ['// Generated from names.txt; every missing entry terminates with its exact name.']
for i, name in enumerate(names):
    assert name.isidentifier()
    lines.append(f'static _Noreturn void missing_{i}(void) {{ die("NVN not implemented: {name}"); }}')
lines.append('static const struct { const char *name; void *fn; } missing[] = {')
lines.extend(f'    {{"{name}", (void *)missing_{i}}},' for i, name in enumerate(names))
lines.append('};')
root.joinpath('missing.inc').write_text('\n'.join(lines) + '\n')
