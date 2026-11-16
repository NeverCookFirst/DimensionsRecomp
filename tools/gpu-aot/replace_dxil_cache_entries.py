"""Replace existing DXIL-only AOT entries, preserving all other shader bytes.

Writes a new file; never modifies the input bank. Python 3.14+ is required.
The replacement may contain entries for more than one bank. Unknown hashes
are ignored, but at least one must match and specialization masks must agree.
"""
import argparse
from compression import zstd
import hashlib
import json
from pathlib import Path
import re

ENTRY = re.compile(r'\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+) \}')


def read(path):
    text = path.read_text()
    if not re.search(r'g_spirvCacheDecompressedSize = 0;', text):
        raise ValueError('Only DXIL-only banks are supported')
    match = re.search(r'const uint8_t g_compressedDxilCache\[\] = \{([\d,\s]+)\};', text)
    if not match:
        raise ValueError('Expected numeric DXIL bank')
    compressed = bytes(int(x) for x in match[1].split(',') if x.strip())
    payload = zstd.decompress(compressed)
    for name, size in (('Compressed', len(compressed)), ('Decompressed', len(payload))):
        declared = re.search(r'g_dxilCache' + name + r'Size = (\d+);', text)
        if not declared or int(declared[1]) != size:
            raise ValueError('DXIL payload size mismatch')
    entries = {}
    for row in ENTRY.findall(text):
        key = int(row[0], 16)
        offset, size, spv_offset, spv_size, mask = map(int, row[1:])
        if key in entries or spv_offset or spv_size or offset + size > len(payload):
            raise ValueError('Invalid or duplicate shader entry')
        blob = payload[offset:offset + size]
        if not blob.startswith(b'DXBC'):
            raise ValueError('Expected DXIL container')
        entries[key] = (mask, blob)
    declared = int(re.search(r'g_shaderCacheEntryCount = (\d+);', text)[1])
    if len(entries) != declared:
        raise ValueError('Entry count mismatch')
    return entries


def replace(base, updates):
    result = dict(base)
    matched = sorted(base.keys() & updates.keys())
    if not matched:
        raise ValueError('No matching replacements')
    for key in matched:
        if base[key][0] != updates[key][0]:
            raise ValueError('Specialization mask changed')
        result[key] = updates[key]
    return result, matched


def write(path, entries):
    payload = bytearray()
    rows = []
    for key, (mask, blob) in sorted(entries.items()):
        rows.append(f'\t{{ 0x{key:016X}, {len(payload)}, {len(blob)}, 0, 0, {mask} }},\n')
        payload.extend(blob)
    compressed = zstd.compress(payload)
    empty = zstd.compress(b'')
    with path.open('w', newline='\n') as f:
        f.write('#include "gpu/shaders/shader_cache.h"\nShaderCacheEntry g_shaderCacheEntries[] = {\n')
        f.writelines(rows)
        f.write('};\nconst uint8_t g_compressedDxilCache[] = {\n')
        for start in range(0, len(compressed), 4096):
            f.write(','.join(map(str, compressed[start:start+4096])) + ',\n')
        f.write('};\n')
        f.write(f'const size_t g_dxilCacheCompressedSize = {len(compressed)};\n')
        f.write(f'const size_t g_dxilCacheDecompressedSize = {len(payload)};\n')
        f.write('const uint8_t g_compressedSpirvCache[] = {' + ','.join(map(str, empty)) + ',};\n')
        f.write(f'const size_t g_spirvCacheCompressedSize = {len(empty)};\n')
        f.write('const size_t g_spirvCacheDecompressedSize = 0;\n')
        f.write(f'const size_t g_shaderCacheEntryCount = {len(entries)};\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('base', type=Path)
    p.add_argument('updates', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    if a.output.exists() or a.output.resolve() in (a.base.resolve(), a.updates.resolve()):
        p.error('Output must be a new file')
    base, updates = read(a.base), read(a.updates)
    result, matched = replace(base, updates)
    write(a.output, result)
    if read(a.output) != result:
        raise RuntimeError('Output round-trip failed')
    report = dict(entries=len(result), replaced=[f'{k:016X}' for k in matched],
                  unchanged_verified=len(result)-len(matched),
                  output_sha256=hashlib.sha256(a.output.read_bytes()).hexdigest())
    a.output.with_suffix('.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report))
