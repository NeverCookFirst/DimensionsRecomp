"""Verify complete alpha candidate coverage; optionally install backed-up banks.

Run installation only while the native game is closed. Never launches a game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('candidate', type=Path)
p.add_argument('backup', type=Path)
p.add_argument('--install', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
native = root / 'rexlego/out/native-gpu'

def entries(p):
    # hash, DXIL offset/length, SPIRV offset/length, specialization mask.
    return {(m[0], int(m[1])) for m in re.findall(
        r'\{ 0x([A-Fa-f0-9]+), \d+, \d+, \d+, \d+, (\d+) \}', p.read_text())}

report = {}
for name, old_name, new_name, n in [('main', 'lego-fixed16-cache.cpp', 'lego-cache.cpp', 9290),
                                    ('runtime', 'runtime-cache.cpp', 'runtime-cache.cpp', 118)]:
    old, new = a.backup/old_name, a.candidate/new_name
    prev, updated = entries(old), entries(new)
    assert len(prev) == len(updated) == n and prev == updated, (name, len(prev), len(updated))
    report[name] = {'containers': n, 'hash_and_specialization_masks_unchanged': True,
                    'sha256': hashlib.sha256(new.read_bytes()).hexdigest()}
main = (a.candidate/'main-compiler-color0.log').read_text()
run = (a.candidate/'runtime-compiler-color0.log').read_text()
assert 'Recompile failures: 1 of 9291' in main
assert re.findall(r'hash=0x([A-Fa-f0-9]+) reason=', main) == ['A58EBA123EFB541C']
assert 'AOT coverage: 9290 of 9291' in main and 'AOT coverage: 118 of 118' in run
report.update({'known_excluded_memexport': 'A58EBA123EFB541C', 'shared_bytes': 624,
               'alpha_function_offset': 600, 'game_visual_validation': False})
(a.candidate/'coverage-proof.json').write_text(json.dumps(report, indent=2))
if a.install:
    shutil.copyfile(a.candidate/'lego-cache.cpp', native/'lego-fixed16-cache.cpp')
    shutil.copyfile(a.candidate/'runtime-cache.cpp', native/'runtime-cache.cpp')
print(json.dumps(report, indent=2))
