"""Replay captured LOTR positions/constants through the actual VS pair, offline.

Requires the existing temporal GPU fixture's captured-4/5 shaders. This checks
position/depth only: it does not replay material textures or claim visual parity.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess


def run(args, **kwargs):
    result = subprocess.run([str(x) for x in args], capture_output=True,
                            text=True, timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dxc', type=Path, required=True)
    parser.add_argument('--host-compiler', default='clang-cl')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    exe = args.output / 'replay.exe'
    run([args.host_compiler, Path(__file__).with_name('test_temporal_motion_gpu.cpp'),
         '/std:c++20', '/O2', '/MD', '/EHsc', '/Fo'+str(args.output/'replay.obj'),
         '/Fe'+str(exe), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib'])
    shaders = [args.bank / ('captured-'+str(i)+'-0-'+stage+'.dxil')
               for i in (4, 5) for stage in ('normal-vs', 'probe-ps')]
    results = []
    inputs = []
    for record in re.split(r'(?m)(?=^draw=)', args.capture.read_text()):
        if not record.strip():
            continue
        fields = dict(re.findall(r'(\w+)=(\S+)', record.splitlines()[0]))
        if fields['VS'] != 'b881e7dfa89cf8f4':
            raise ValueError('Capture contains a different vertex shader')
        if fields['primitive'] != '4' or fields['indexed'] != '1':
            raise ValueError('Expected indexed triangle lists')
        if not re.search(r'host_attr=POSITION0 slot=0 offset=0 format=6 stride=40 stream_offset=0 ', record):
            raise ValueError('Unsupported position declaration')
        if not re.search(r'guest_format=1 ', record):
            raise ValueError('Expected uint16 indices')
        index = Path(json.loads(re.search(r'(?m)^index_blob=(.*)$', record)[1])).read_bytes()
        vertex = Path(json.loads(re.search(r'(?m)^vertex_blob_slot=0 .* path=(.*)$', record)[1])).read_bytes()
        registers = dict(re.findall(r'(?m)^VS_C(\d+)=(.*)$', record))
        constants = b''.join(struct.pack('<4I', *[int(v, 16) for v in
                          registers[str(i)].split(',')[:4]]) for i in range(256))
        start, count, base = (int(fields[k]) for k in ('start', 'count', 'base'))
        indices = struct.unpack_from('>'+str(count)+'H', index, start*2)
        positions = bytearray()
        for i in indices:
            offset = (i+base)*40
            if offset < 0 or offset+12 > len(vertex):
                raise ValueError('Out-of-bounds vertex')
            positions.extend(struct.pack('<4f', *struct.unpack_from('>3f', vertex, offset), 1))
        path = args.output / ('draw-'+fields['draw']+'.bin')
        path.write_bytes(constants+positions)
        output = run([exe, *shaders, 'capture', path])
        result = dict(draw=int(fields['draw']), **{k: int(v) for k, v in
                      re.findall(r'(vertices|covered|depth_mismatches|coverage_differences)=(\d+)', output)})
        results.append(result)
        inputs.append(path)
    if not results or sum(r['covered'] for r in results) < 1000:
        raise ValueError('Replay had insufficient visible coverage')
    # A compiled position mutation must fail; matching empty images cannot pass.
    source = (args.bank/'captured-5-0-normal-vs.hlsl').read_text()
    marker = 'oPos.xy += g_HalfPixelOffset * oPos.w;'
    if source.count(marker) != 1:
        raise ValueError('Missing negative-control insertion point')
    mutant = args.output/'mutated-vs.hlsl'
    mutant.write_text(source.replace(marker, marker+'\n oPos.x += 0.02 * oPos.w;'))
    dxil = args.output/'mutated-vs.dxil'
    run([args.dxc, '-T', 'vs_6_0', '-E', 'main', '-HV', '2021', '-Fo', dxil, mutant])
    visible = next(i for i, r in enumerate(results) if r['covered'] > 1000)
    control = subprocess.run([str(x) for x in [exe, *shaders[:2], dxil,
                             shaders[3], 'capture', inputs[visible]]],
                             capture_output=True, text=True, timeout=120)
    if control.returncode != 9:
        raise RuntimeError('Position mutation was not rejected: '+control.stdout+control.stderr)
    report = dict(draws=results, negative_control=control.stdout.strip(),
                  scope='Position/depth comparison only; no material texture replay')
    (args.output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS', len(results), 'draws; depth/coverage identical; mutation rejected')


if __name__ == '__main__':
    main()
