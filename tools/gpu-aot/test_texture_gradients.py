"""Synthetic microcode -> HLSL -> validated DXIL gradient candidate checks."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('compiler', type=Path)
p.add_argument('common', type=Path)
p.add_argument('scratch', type=Path)
a = p.parse_args()

def fixture(swizzle=4, dst=0x688, predicate=None, relative=0, vertex=False):
    data = bytearray(164)
    struct.pack_into('>9I', data, 0, 0x102A1100 | int(vertex), 128, 36, 0, 36, 0, 80, 0, 0)
    struct.pack_into('>8I', data, 36, 32, 28, 0, 0, 0, 0, 0, 0)
    # r7 receives SV_Position, so non-predicated XY derivative fixtures operate
    # on a varying input rather than only compiling derivatives of zero.
    struct.pack_into('>8I', data, 80, 0, 36, 0, 7 << 8, 0, 0, 0, 1)
    struct.pack_into('>3I', data, 128, 1 | (2 << 12) | (1 << 16), 2 << 12, 0)
    # getGradients r2, r7.<two source components>; no sampler needed.
    struct.pack_into('>3I', data, 140, 18 | (7 << 5) | (2 << 12) | (swizzle << 26) | relative,
                     dst | (int(predicate is not None) << 31),
                     (1 << 14) | (int(bool(predicate)) << 31))
    struct.pack_into('>3I', data, 152, (62 if vertex else 0) | (1 << 15) | (15 << 16), 0,
                     2 | (2 << 8) | (2 << 16) | (2 << 24) | (7 << 29))
    return data

inputs = a.scratch/'input'
hlsl = a.scratch/'hlsl'
inputs.mkdir(parents=True, exist_ok=True)
cases = {}
for swizzle in range(16):
    for predicate in (None, False, True):
        name = f'swizzle{swizzle}_predicate{predicate}'
        cases[name] = (swizzle, predicate)
        (inputs/(name+'.bin')).write_bytes(fixture(swizzle=swizzle, predicate=predicate))
for name, dst in [('reversed', 0x053), ('constants', 4 | (5 << 3) | (7 << 6) | (7 << 9))]:
    (inputs/(name+'.bin')).write_bytes(fixture(dst=dst))
env = os.environ.copy()
env.update(XENOS_RECOMP_DXIL_ONLY='1', XENOS_RECOMP_LEGO_NATIVE_SCALE='1', XENOS_RECOMP_VERBOSE_DXC='1')
env.pop('XENOS_RECOMP_ONLY_HASH', None)

def run(directory, output, hlsl_dir=None):
    cmd = [str(a.compiler.resolve()), str(directory.resolve()), str(output.resolve()), str(a.common.resolve())]
    if hlsl_dir is not None:
        cmd.append(str(hlsl_dir.resolve()))
    return subprocess.run(cmd, env=env, capture_output=True, text=True)

result = run(inputs, a.scratch/'fixture-cache.cpp', hlsl)
if result.returncode:
    raise RuntimeError(result.stdout+result.stderr)
for name, (swizzle, predicate) in cases.items():
    text = (hlsl/(name+'.hlsl')).read_text()
    assert 'float4 r7 = float4((iPos.xy - 0.5)' in text, name
    x, y = 'xyzw'[swizzle & 3], 'xyzw'[(swizzle >> 2) & 3]
    statement = f'r2.xyzw = float4(ddx_coarse(r7.{x}), ddy_coarse(r7.{x}), ddx_coarse(r7.{y}), ddy_coarse(r7.{y})).xyzw;'
    assert statement in text, name
    if predicate is not None:
        assert re.search(r'if \(' + ('' if predicate else '!') + r'p0\)\s*\{\s*' + re.escape(statement), text), name
reversed_text = (hlsl/'reversed.hlsl').read_text()
assert ')).wzyx;' in reversed_text
constant_text = (hlsl/'constants.hlsl').read_text()
assert 'r2.x = 0.0;' in constant_text and 'r2.y = 1.0;' in constant_text
assert 'ddx_coarse(' not in constant_text
for name, opts in [('relative_source', {'relative':1 << 11}),
                   ('relative_destination', {'relative':1 << 18}), ('vertex', {'vertex':True})]:
    directory = a.scratch/name
    directory.mkdir(exist_ok=True)
    (directory/'fixture.bin').write_bytes(fixture(**opts))
    rejected = run(directory, a.scratch/(name+'.cpp'))
    assert rejected.returncode and 'vertex/relative addressing unsupported' in rejected.stderr, name
(a.scratch/'verification.json').write_text(json.dumps({'passed':True, 'hlsl_dxil_fixtures':50,
    'source_swizzles':16, 'predication':[None,False,True], 'refusal_cases':3,
    'shared_bytes':624, 'installed':False, 'game_visual_validation':False}, indent=2)+'\n')
print('PASS: 50 gradient microcode/HLSL/DXIL fixtures and 3 unsupported-mode refusals')
