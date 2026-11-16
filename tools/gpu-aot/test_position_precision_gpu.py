"""Check depth/color positions using the actual DXIL bank on D3D12, without a window.

The synthetic inputs align the three UV layouts to compare all six Adventure
Time VTF programs. This tests shader arithmetic, not in-game visual acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--bank', type=Path, required=True)
p.add_argument('--baseline', type=Path, help='Original spec0 DXIL directory, files named by hash')
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
hashes = ['51A51F31865A9BEA', '758F1CBD24D47419', '872E9F9219CC6C3E',
          '98117CD19D383B96', '9B80DE62C017EC70', 'DB9ABE353AF31FB1']
exe = out/'position-test-gpu.exe'
source = Path(__file__).with_suffix('.cpp')
compiled = subprocess.run([a.compiler, str(source), '/std:c++20', '/O2', '/MD', '/EHsc',
    '/Fo'+out.as_posix()+'/', '/Fe'+str(exe), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib'],
    capture_output=True, text=True, timeout=60)
(out/'compile.log').write_text(compiled.stdout+compiled.stderr)
compiled.check_returncode()
blob = (a.bank/'float-blend-indices-bank.bin').read_bytes()
entries = {}
for m in re.finditer(r'\{0x([0-9A-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+)\}',
                    (a.bank/'float-blend-indices-bank.cpp').read_text()):
    h, s, offset, size, kind = m.groups()
    if int(kind) == 1:
        entries[h.upper(), int(s)] = blob[int(offset):int(offset)+int(size)]
records = []

def run(name, folder, expect_mismatch=False):
    result = subprocess.run([str(exe), str(folder)]+(['--expect-mismatch'] if expect_mismatch else []),
        capture_output=True, text=True, timeout=90)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(name+': '+result.stdout+result.stderr)
    records.append(dict(name=name, exit_code=result.returncode, output=result.stdout))
    print(name+': '+result.stdout.strip().splitlines()[-1])

for spec in range(2):
    folder = out/('bank-spec-'+str(spec))
    folder.mkdir(exist_ok=True)
    (folder/'index.txt').write_text('\n'.join(hashes)+'\n')
    for h in hashes:
        data = entries[h, spec]
        assert data[:4] == b'DXBC'
        (folder/(h+'.dxil')).write_bytes(data)
    run('precise-spec-'+str(spec), folder)
if a.baseline:
    folder = out/'production-baseline'
    folder.mkdir(exist_ok=True)
    (folder/'index.txt').write_text('\n'.join(hashes)+'\n')
    for h in hashes:
        shutil.copy2(a.baseline/(h+'.dxil'), folder/(h+'.dxil'))
    run('original-production-negative', folder, True)
    # Omitting one partner must be detected, even if the other five are precise.
    folder = out/'omitted-partner'
    shutil.copytree(out/'bank-spec-0', folder, dirs_exist_ok=True)
    shutil.copy2(a.baseline/(hashes[3]+'.dxil'), folder/(hashes[3]+'.dxil'))
    run('omitted-partner-negative', folder, True)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,
    bank_sha256=hashlib.sha256(blob).hexdigest(), results=records), indent=2)+'\n')
