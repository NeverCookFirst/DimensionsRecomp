"""End-to-end explicit-LOD fixtures for the isolated compiler candidate.

Checks actual microcode -> HLSL -> validated DXIL, using only synthetic inputs.
All output stays in scratch; this does not establish game image correctness.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess


def fixture(*, computed=False, register=True, predicated=False, bias=0,
            vertex=False, relative=False):
    data = bytearray(176)
    struct.pack_into('>9I', data, 0, 0x102A1100 | int(vertex), 128, 48, 0, 36, 0, 80, 0, 0)
    struct.pack_into('>8I', data, 36, 32, 28, 0, 0, 0, 0, 0, 0)
    struct.pack_into('>8I', data, 80, 0, 48, 0, 0xFF00, 0, 0, 0, 1)
    # EXEC_END: setTexLOD, tfetch2D, ALU export. CF1 is NOP.
    struct.pack_into('>3I', data, 128, 1 | (3 << 12) | (5 << 16), 2 << 12, 0)
    set0 = 24 | (7 << 5) | (int(relative) << 11) | (2 << 26)  # source r7.z
    set1 = int(predicated) << 31
    set2 = int(predicated) << 31
    struct.pack_into('>3I', data, 140, set0, set1, set2)
    fetch0 = 1 | (8 << 5) | (2 << 12) | (3 << 20) | (4 << 26)
    fetch1 = 0x688 | (3 << 12) | (3 << 14) | (3 << 16)
    fetch1 |= int(computed) << 28 | int(register) << 29
    fetch2 = ((bias & 127) << 2) | (1 << 14)
    struct.pack_into('>3I', data, 152, fetch0, fetch1, fetch2)
    # MAX r2, r2 exported as color0 or oPos; ordinary register operands.
    struct.pack_into('>3I', data, 164, (62 if vertex else 0) | (1 << 15) | (15 << 16), 0,
                     2 | (2 << 8) | (2 << 16) | (2 << 24) | (7 << 29))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('common', type=Path)
    parser.add_argument('scratch', type=Path)
    args = parser.parse_args()
    inputs = args.scratch / 'input'
    hlsl = args.scratch / 'hlsl'
    inputs.mkdir(parents=True, exist_ok=True)
    cases = {
        'explicit_register': {},
        'explicit_zero': {'register': False},
        'predicated_set': {'predicated': True},
        'negative_instruction_bias': {'bias': -8},
        'positive_instruction_bias': {'bias': 24},
        'computed_pixel_lod': {'computed': True, 'register': False},
        'vertex_computed_is_explicit': {'computed': True, 'register': False, 'vertex': True},
    }
    for name, opts in cases.items():
        (inputs / f'{name}.bin').write_bytes(fixture(**opts))
    env = os.environ.copy()
    env['XENOS_RECOMP_DXIL_ONLY'] = '1'
    env['XENOS_RECOMP_LEGO_NATIVE_SCALE'] = '1'
    env['XENOS_RECOMP_VERBOSE_DXC'] = '1'
    env.pop('XENOS_RECOMP_ONLY_HASH', None)
    result = subprocess.run([str(args.compiler.resolve()), str(inputs.resolve()),
                             str((args.scratch / 'fixture-cache.cpp').resolve()),
                             str(args.common.resolve()), str(hlsl.resolve())],
                            env=env, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    for name, opts in cases.items():
        source = (hlsl / f'{name}.hlsl').read_text()
        assert 'float textureLod = 0.0;' in source, name
        assert 'textureLod = r7.z;' in source, name
        assert 'g_FetchLodBiasArr[8] : packoffset(c39)' in source, name
        computed = opts.get('computed', False) and not opts.get('vertex', False)
        call = re.search(r'r2\.xyzw = (tfetch\w+)\(([^\n]+)', source)
        assert call, name
        if computed:
            assert call.group(1) == 'tfetch2D', name
            assert 'g_FetchLodBias(3)' not in call.group(2), name
        else:
            assert call.group(1) == 'tfetchLevel2D', name
            lod = 'textureLod' if opts.get('register', True) else '0.0'
            assert f'{lod} + g_FetchLodBias(3)' in call.group(2), name
            assert str(opts.get('bias', 0) / 16).rstrip('0').rstrip('.') in call.group(2), name
        if opts.get('predicated'):
            assert re.search(r'if \(p0\)\s*\{\s*textureLod = r7.z;', source), name
    # Reject a source addressing mode that this candidate cannot translate.
    rejected = args.scratch / 'rejected-input'
    rejected.mkdir(parents=True, exist_ok=True)
    (rejected / 'relative.bin').write_bytes(fixture(relative=True))
    failure = subprocess.run([str(args.compiler.resolve()), str(rejected.resolve()),
                              str((args.scratch / 'rejected-cache.cpp').resolve()),
                              str(args.common.resolve())], env=env, text=True, capture_output=True)
    assert failure.returncode != 0 and 'relative setTexLOD unsupported' in failure.stderr
    print(f'Explicit LOD regression passed: {len(cases)} HLSL/DXIL fixtures, relative-source refusal')


if __name__ == '__main__':
    main()
