"""Inventory Xenos fetch semantics in validated containers without changing shaders.

This flags features for comparison with Xenia, not proven rendering defects.
Decoding follows shader_code.h / shader_texture_usage.h in this workspace.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct


def field(value, shift, bits=1):
    return (value >> shift) & ((1 << bits) - 1)


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def decode(path):
    data = path.read_bytes()
    flags, virtual, physical = struct.unpack_from('>III', data)
    if flags & 0xFFFFFF00 != 0x102A1100 or virtual < 36 or virtual + 4 > len(data):
        raise ValueError('invalid container header')
    physical_base = virtual + (4 if struct.unpack_from('>I', data, virtual)[0] == physical else 0)
    shader_offset = struct.unpack_from('>I', data, 24)[0]
    if shader_offset + 8 > virtual or physical_base + physical > len(data):
        raise ValueError('invalid section bounds')
    offset, size = struct.unpack_from('>II', data, shader_offset)
    if not size or size % 12 or offset + size > physical:
        raise ValueError('invalid instruction bounds')
    code = data[physical_base + offset:physical_base + offset + size]
    slots = [struct.unpack_from('>III', code, at) for at in range(0, size, 12)]
    cf_end = len(slots)
    visited = set()
    fetches, control = [], []
    pair = 0
    while pair < cf_end:
        a, b, c = slots[pair]
        for lo, hi in ((a, b & 0xFFFF), ((b >> 16) | ((c << 16) & 0xFFFFFFFF), c >> 16)):
            op = hi >> 12
            control.append(op)
            if op not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            address, count, sequence = lo & 0xFFF, field(lo, 12, 3), field(lo, 16, 12)
            if count > 6 or address <= pair or address + count > len(slots):
                raise ValueError('invalid EXEC clause')
            cf_end = min(cf_end, address)
            for i in range(count):
                instruction = address + i
                if not field(sequence, i * 2) or instruction in visited:
                    continue
                visited.add(instruction)
                w0, w1, w2 = slots[instruction]
                opcode = w0 & 31
                item = {'instruction': instruction, 'opcode': opcode,
                        'words': [f'{w:08X}' for w in (w0, w1, w2)]}
                if opcode == 0:
                    item.update(kind='vertex', source_register=field(w0, 5, 6),
                        source_relative=field(w0, 11), constant=field(w0, 20, 5),
                        constant_select=field(w0, 25, 2), source_component=field(w0, 30, 2),
                        format=field(w1, 16, 6), signed=field(w1, 12), integer=field(w1, 13),
                        exponent=signed(field(w1, 24, 6), 6), mini=field(w1, 30),
                        stride=field(w2, 0, 8), offset=signed(field(w2, 8, 23), 23))
                else:
                    item.update(kind='texture', constant=field(w0, 20, 5),
                        source_register=field(w0, 5, 6), source_swizzle=field(w0, 26, 6),
                        computed_lod=field(w1, 28), register_lod=field(w1, 29),
                        register_gradients=field(w2, 0), dimension=field(w2, 14, 2),
                        mag_filter=field(w1, 12, 2), min_filter=field(w1, 14, 2),
                        mip_filter=field(w1, 16, 2))
                fetches.append(item)
        pair += 1
    return {'file': str(path.resolve()), 'stage': 'vs' if flags & 1 else 'ps',
            'container_sha256': hashlib.sha256(data).hexdigest(), 'code_bytes': size,
            'control_opcodes': sorted(set(control)), 'fetches': fetches}


def audit(roots):
    entries, errors, seen = [], [], set()
    for root in roots:
        for path in sorted(root.rglob('*.bin')):
            try:
                entry = decode(path)
            except (ValueError, struct.error) as exc:
                errors.append({'file': str(path), 'error': str(exc)})
                continue
            if entry['container_sha256'] in seen:
                continue
            seen.add(entry['container_sha256'])
            entries.append(entry)
    findings = collections.defaultdict(list)
    for entry in entries:
        for fetch in entry['fetches']:
            name = Path(entry['file']).name
            if fetch['kind'] == 'vertex':
                for key in ('mini', 'exponent', 'source_relative'):
                    if fetch[key]:
                        findings[f'vertex_{key}'].append({'shader': name, **fetch})
                if fetch['source_register'] or fetch['source_component']:
                    findings['vertex_nondefault_index'].append({'shader': name, **fetch})
            else:
                if fetch['opcode'] not in (1, 19):
                    findings['texture_state_opcode'].append({'shader': name, **fetch})
                if fetch['opcode'] == 1 and fetch['register_lod']:
                    findings['texture_register_lod'].append({'shader': name, **fetch})
                if fetch['opcode'] == 1 and fetch['register_gradients']:
                    findings['texture_register_gradients'].append({'shader': name, **fetch})
    return {'note': 'Static feature inventory; findings are not proven visual defects.',
            'containers': len(entries), 'stages': dict(collections.Counter(e['stage'] for e in entries)),
            'findings': dict(findings), 'errors': errors, 'shaders': entries}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roots', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = audit(args.roots)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'containers': report['containers'], 'stages': report['stages'],
        'finding_counts': {k: len(v) for k, v in report['findings'].items()},
        'errors': len(report['errors'])}))
