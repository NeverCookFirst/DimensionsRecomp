"""Rank exact-prefix offline candidates against a bounded raw placement dump.

Diagnostics only: similarity is NOT authorization to bind a shader.
"""
import argparse
from pathlib import Path
import struct

def compare(dump, root, stage):
    runtime = dump.read_bytes()
    candidates = []
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        data = path.read_bytes()
        offset = 0
        while True:
            offset = data.find(b'\x10\x2a\x11', offset)
            if offset < 0: break
            pos = offset
            offset += 1
            if pos + 36 > len(data): continue
            flags, virtual, physical = struct.unpack_from('>III', data, pos)
            if bool(flags & 1) != (stage == 'vs'): continue
            if virtual < 36 or max(virtual, physical) > 4 * 1024 * 1024: continue
            if pos + virtual + 4 > len(data): continue
            marker = struct.unpack_from('>I', data, pos + virtual)[0] == physical
            physical_base = pos + virtual + (4 if marker else 0)
            if physical_base + physical > len(data): continue
            shader_offset = struct.unpack_from('>I', data, pos + 24)[0]
            if shader_offset + 8 > virtual: continue
            code_offset, code_size = struct.unpack_from('>II', data, pos + shader_offset)
            for kind, start, size in [('physical', physical_base, physical),
                                      ('code', physical_base + code_offset, code_size)]:
                if size < 8 or size > len(runtime) or start + size > physical_base + physical: continue
                blob = data[start:start + size]
                if blob[:8] != runtime[:8]: continue
                differences = [i for i in range(size) if blob[i] != runtime[i]]
                candidates.append((len(differences), -size, str(path), pos, kind, start, differences))
    for count, negsize, path, pos, kind, start, differences in sorted(candidates)[:12]:
        print(f'{path}: container={pos} {kind}={start} size={-negsize} differing_bytes={count}')
        data = Path(path).read_bytes()
        for i in differences[:24]:
            print(f'  +{i:04X}: offline={data[start+i]:02X} runtime={runtime[i]:02X}')
    print(f'{len(candidates)} same-stage exact-prefix candidates (not binding decisions)')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('stage', choices=['vs', 'ps'])
    args = parser.parse_args()
    compare(args.dump, args.root, args.stage)
