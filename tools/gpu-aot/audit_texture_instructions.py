"""Inventory actual fetch instructions in local Xenos containers, without a game.

Uses EXEC instruction sequences, not arbitrary byte pattern matches. Game input
and generated reports remain local. This is coverage evidence, not a pixel test.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

EXEC = {1, 2, 3, 4, 5, 6, 13, 14}

def fetches(data):
    flags, virtual, physical = struct.unpack_from('>III', data)
    if flags not in (0x102A1100, 0x102A1101):
        raise ValueError('Not a vertex/pixel Xenos container')
    if len(data) != virtual + 4 + physical:
        raise ValueError('Container length mismatch')
    header = struct.unpack_from('>I', data, 24)[0]
    offset, size = struct.unpack_from('>II', data, header)
    if size % 12 or offset + size > physical:
        raise ValueError('Invalid microcode extent')
    words = struct.unpack_from('>' + 'I' * (size // 4), data, virtual + 4 + offset)
    boundary = len(words) // 3
    pos = 0
    seen = set()
    result = []
    while pos < boundary:
        w0, w1, w2 = words[pos * 3:pos * 3 + 3]
        for cf in (w0 | ((w1 & 65535) << 32), (w1 >> 16) | (w2 << 16)):
            opcode = (cf >> 44) & 15
            if opcode not in EXEC:
                continue
            address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
            boundary = min(boundary, address)
            if address + count > len(words) // 3:
                raise ValueError('EXEC outside microcode')
            for i in range(count):
                at = address + i
                if (sequence >> (i * 2)) & 1 and at not in seen:
                    seen.add(at)
                    a, b, c = words[at * 3:at * 3 + 3]
                    op = a & 31
                    if op == 0:
                        continue  # Vertex fetch, not a texture operation.
                    result.append({'instruction':at, 'opcode':op,
                        'slot':(a >> 20) & 31, 'denormalized':bool((a >> 25) & 1),
                        'dimension':(c >> 14) & 3,
                        'mag_filter':(b >> 12) & 3, 'min_filter':(b >> 14) & 3,
                        'mip_filter':(b >> 16) & 3,
                        'computed_lod':bool((b >> 28) & 1),
                        'register_lod':bool((b >> 29) & 1),
                        'register_gradients':bool(c & 1),
                        'instruction_lod_bias':(((c >> 2) & 127) - (128 if c & 256 else 0)) / 16})
        pos += 1
    return flags & 1, result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    counts = Counter()
    shaders = []
    containers = []
    paths = sorted(a.input.glob('*.bin'))
    if paths:
        containers = [(path.name, path.read_bytes()) for path in paths]
    else:
        seen = set()
        for path in sorted(a.input.rglob('*.360_shaders')):
            asset = path.read_bytes()
            for signature in (b'\x10\x2a\x11\x00', b'\x10\x2a\x11\x01'):
                at = asset.find(signature)
                while at != -1:
                    if at + 36 <= len(asset):
                        _, virtual, physical = struct.unpack_from('>III', asset, at)
                        end = at + virtual + 4 + physical
                        if 36 <= virtual <= 16*1024*1024 and physical >= 12 and end <= len(asset):
                            data = asset[at:end]
                            key = hashlib.sha256(data).digest()
                            if key not in seen:
                                try:
                                    fetches(data)
                                except (ValueError, struct.error):
                                    pass
                                else:
                                    seen.add(key)
                                    containers.append((str(path.relative_to(a.input))+f'@{at:X}', data))
                    at = asset.find(signature, at+1)
    if not containers:
        raise ValueError('No shader containers found')
    for name, data in containers:
        stage, records = fetches(data)
        counts['containers'] += 1
        interesting = []
        for record in records:
            op = record['opcode']
            counts[f'opcode_{op}'] += 1
            if op != 1:
                continue
            for field in ['denormalized', 'register_gradients']:
                if record[field]:
                    counts[field] += 1
            for field in ['mag_filter', 'min_filter', 'mip_filter']:
                counts[f'{field}_{record[field]}'] += 1
            if record['denormalized'] or record['register_gradients'] or record['mip_filter'] != 3:
                interesting.append(record)
        if interesting:
            shaders.append({'container':name, 'stage':'VS' if stage else 'PS',
                            'fetches':interesting})
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps({'counts':dict(counts), 'shaders':shaders}, indent=2)+'\n')
    print(json.dumps(dict(counts), sort_keys=True))

if __name__ == '__main__':
    main()
