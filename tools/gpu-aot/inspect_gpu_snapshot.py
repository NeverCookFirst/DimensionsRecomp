"""Inspect uncompressed DX10 DDS GPU snapshots without third-party packages.

Directory mode prints sampled RGB ranges. --image renders one snapshot as PPM;
--scale is an explicit diagnostic exposure multiplier, not an in-game change.
"""
import argparse
import math
from pathlib import Path
import struct


def inspect(path, image, scale):
    data = path.read_bytes()
    assert data[:4] == b'DDS ' and data[84:88] == b'DX10'
    height, width = struct.unpack_from('<II', data, 12)
    fmt = struct.unpack_from('<I', data, 128)[0]
    code, divisor, channels = {
        2: ('4f', 1, (0, 1, 2)),
        28: ('4B', 255, (0, 1, 2)), 87: ('4B', 255, (2, 1, 0)),
        10: ('4e', 1, (0, 1, 2)), 11: ('4H', 65535, (0, 1, 2)),
        13: ('4h', 32767, (0, 1, 2)),
        34: ('2e', 1, (0, 1, 0)), 35: ('2H', 65535, (0, 1, 0)),
        41: ('f', 1, (0, 0, 0)),
    }[fmt]
    pixel = struct.Struct('<' + code)
    count = width * height
    assert len(data) == 148 + count * pixel.size
    def rgb(index):
        value = pixel.unpack_from(data, 148 + index * pixel.size)
        return tuple(value[c] / divisor for c in channels)
    samples = [rgb(i) for i in range(0, count, max(1, count // 4096))]
    finite = [v for sample in samples for v in sample if math.isfinite(v)]
    nonzero = sum(any(math.isfinite(c) and abs(c) > 1e-8 for c in p) for p in samples)
    print(f'{path.name} {width}x{height} dxgi={fmt} '
          f'range={min(finite, default=0):.5g}..{max(finite, default=0):.5g} '
          f'nonzero={nonzero}/{len(samples)}')
    if image:
        out = bytearray()
        for i in range(count):
            for channel in rgb(i):
                out.append(round(max(0, min(1, channel * scale)) * 255)
                           if math.isfinite(channel) else 255)
        path.with_suffix('.ppm').write_bytes(f'P6\n{width} {height}\n255\n'.encode() + out)


parser = argparse.ArgumentParser()
parser.add_argument('path', type=Path)
parser.add_argument('--image', action='store_true')
parser.add_argument('--scale', type=float, default=1)
args = parser.parse_args()
paths = sorted(args.path.glob('*.dds')) if args.path.is_dir() else [args.path]
for path in paths:
    inspect(path, args.image, args.scale)
