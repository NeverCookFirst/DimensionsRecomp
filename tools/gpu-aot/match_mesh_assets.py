"""Match complete traced upload records against decompressed guest assets offline."""
import argparse
import json
import re
from pathlib import Path


def guest_record(observations):
    attrs = []
    for line in observations:
        m = re.fullmatch(r'attr=(\w+) slot=0 offset=(\d+) stride=(\d+) format=\d+ reversed=(\d+) length=\d+ sampled=true words=([0-9A-F]{8}),([0-9A-F]{8})', line)
        if m:
            _, offset, stride, reverse, a, b = m.groups()
            attrs.append((int(offset), int(stride), int(reverse),
                          int(a, 16).to_bytes(4, 'little') + int(b, 16).to_bytes(4, 'little')))
    if not attrs or len({a[1] for a in attrs}) != 1:
        return None
    stride = attrs[0][1]
    data = [None] * stride
    for offset, _, _, raw in attrs:
        for i, byte in enumerate(raw[:max(0, stride-offset)]):
            if data[offset+i] is not None and data[offset+i] != byte:
                raise ValueError('Overlapping trace samples disagree')
            data[offset+i] = byte
    if None in data:
        return None
    data = bytes(data)
    # Undo per-field WZYX, then the common guest DWORD endian conversion.
    for offset, _, reverse, _ in attrs:
        if reverse:
            data = data[:offset] + data[offset:offset+4][::-1] + data[offset+4:]
    return b''.join(data[i:i+4][::-1] for i in range(0, stride, 4))


def match(report, assets):
    rows = []
    for draw in report['draws']:
        record = guest_record(draw['observations'])
        if record is None:
            continue
        matches = []
        for name, data in assets.items():
            offset = data.find(record)
            while offset >= 0:
                matches.append({'asset': name, 'offset': offset})
                offset = data.find(record, offset+1)
        rows.append({'id': draw['id'], 'VS': draw['VS'], 'PS': draw['PS'],
                     'start': draw['start'], 'count': draw['count'], 'base': draw['base'],
                     'guest_record': record.hex(), 'bytes': len(record), 'matches': matches})
    return rows


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('summary', type=Path)
    p.add_argument('assets', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    assets = {f.name: f.read_bytes() for f in a.assets.glob('*.ghg')}
    rows = match(json.loads(a.summary.read_text()), assets)
    result = {'trace': str(a.summary.resolve()), 'assets': {k: len(v) for k,v in assets.items()},
              'note': 'Full byte-record matches identify stored vertex data, not scene instance or correctness of every vertex/bone.', 'rows': rows}
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(rows, indent=2))
