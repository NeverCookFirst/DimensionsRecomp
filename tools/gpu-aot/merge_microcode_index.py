"""Extend an existing placement index while preserving every old signature.

Inputs are the generated ShaderMicrocodeEntry C++ format. Conflicting records
are refused rather than silently replacing metadata for a compiled shader.
"""
import argparse
import json
from pathlib import Path
import re


ROW = re.compile(r'\{\s*0x([\dA-Fa-f]+),\s*0x([\dA-Fa-f]+),\s*'
                 r'0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+),\s*0x([\dA-Fa-f]+)\s*\}')


def read(path):
    source = path.read_text()
    rows = [tuple(int(v, 10 if i in (3, 4) else 16) for i, v in enumerate(match))
            for match in ROW.findall(source)]
    declared = re.search(r'g_shaderMicrocodeEntryCount\s*=\s*(\d+)', source)
    if not declared or len(rows) != int(declared[1]):
        raise ValueError(f'Index record count mismatch: {path}')
    if any(r[3] < 8 or r[4] not in (0, 1) for r in rows):
        raise ValueError(f'Invalid size or stage: {path}')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('old', type=Path)
    parser.add_argument('addition', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    before, addition = read(args.old), read(args.addition)
    combined = {}
    for row in before + addition:
        key = (row[2], row[1])  # container and entire physical/instruction hash
        if key in combined and combined[key] != row:
            raise ValueError(f'Conflicting metadata: {key}')
        combined[key] = row
    rows = sorted(combined.values(), key=lambda r: (r[0], r[4], r[1], r[2]))
    assert set(before) <= set(rows) and set(addition) <= set(rows)
    result = '#include "gpu_native/shader_archive.h"\nShaderMicrocodeEntry g_shaderMicrocodeEntries[] = {\n'
    result += ''.join(f'  {{ 0x{r[0]:016X}, 0x{r[1]:016X}, 0x{r[2]:016X}, '
                      f'{r[3]}, {r[4]}, 0x{r[5]:08X} }},\n' for r in rows)
    result += f'}};\nconst size_t g_shaderMicrocodeEntryCount = {len(rows)};\n'
    args.output.write_text(result, encoding='utf-8')
    args.report.write_text(json.dumps({
        'before': len(before), 'addition': len(addition), 'after': len(rows),
        'added': len(rows) - len(before), 'preserved_all_old_records': True,
        'included_all_addition_records': True,
    }, indent=2), encoding='utf-8')
    print(f'Placement index: {len(before)} -> {len(rows)}, all prior records preserved')


if __name__ == '__main__':
    main()
