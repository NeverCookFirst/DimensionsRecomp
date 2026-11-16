"""Append verified position variants without changing existing bank payloads.

Inputs are ABI624 native HLSL. This offline tool does not start the game.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--position-source', type=Path, action='append', default=[])
    parser.add_argument('--position-source-list', type=Path, action='append', default=[],
                        help='JSON source-path array; relative paths resolve beside the list file')
    parser.add_argument('--canonical-static-source', type=Path, action='append', default=[],
                        help='Unmodified audited static fog source; normalize its clip sum without forcing precise')
    parser.add_argument('--dxc', type=Path, required=True)
    parser.add_argument('--llvm-mc', type=Path, required=True)
    args = parser.parse_args()
    for source_list in args.position_source_list:
        paths = json.loads(source_list.read_text())
        if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
            raise ValueError('Expected a JSON source-path array: '+str(source_list))
        args.position_source.extend(source_list.resolve().parent / p for p in paths)
    if not args.position_source and not args.canonical_static_source:
        parser.error('At least one position source is required')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if out == args.base.resolve():
        raise ValueError('The base bank must remain unchanged')
    metadata = (args.base/'float-blend-indices-bank.cpp').read_text()
    base_blob = (args.base/'float-blend-indices-bank.bin').read_bytes()
    entries = {}
    for match in re.finditer(r'\{0x([0-9A-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+)\}', metadata):
        hash_, spec, offset, size, kind = (int(v, 16 if i == 0 else 10) for i, v in enumerate(match.groups()))
        key = hash_, spec, kind
        if key in entries or offset+size > len(base_blob) or size < 32:
            raise ValueError('Invalid base bank entry')
        entries[key] = offset, size
    if not entries:
        raise ValueError('Empty base bank')
    original_entries = dict(entries)
    jobs = []
    for path, canonical in ([(p, False) for p in args.position_source] +
                            [(p, True) for p in args.canonical_static_source]):
        text = path.read_text()
        match = re.search(r'// vertex shader\s+hash=0x([0-9A-Fa-f]+)', text[:300])
        if not match or 'packoffset(c38)' not in text or text.count('out float4 oPos : SV_Position') != 1:
            raise ValueError('Expected ABI624 native vertex source: '+str(path))
        if text.count('uint g_SpecConstants();') == 1:
            text = text.replace('uint g_SpecConstants();', 'uint g_SpecConstants() { return 0; }')
        if text.count('uint g_SpecConstants() { return 0; }') != 1:
            raise ValueError('Expected resolved spec0 or unresolved specialization')
        hash_ = int(match[1], 16)
        if canonical:
            import runpy
            apply = runpy.run_path(str(Path(__file__).with_name('prepare_static_position_variant.py')))['apply']
            text = apply(text)
        for spec in range(2):
            if (hash_, spec, 1) in entries:
                raise ValueError('Refusing to replace an existing position variant')
            jobs.append((hash_, spec, path, text, canonical))
    if len({(j[0], j[1]) for j in jobs}) != len(jobs):
        raise ValueError('Duplicate input shader')

    def compile_one(job):
        hash_, spec, path, text, canonical = job
        text = text.replace('uint g_SpecConstants() { return 0; }', f'uint g_SpecConstants() {{ return {spec}; }}')
        if not canonical:
            text = text.replace('out float4 oPos : SV_Position', 'out precise float4 oPos : SV_Position')
        stem = f'{hash_:016X}-vs-{spec}-position'
        source, binary = out/(stem+'.hlsl'), out/(stem+'.dxil')
        source.write_text(text)
        result = subprocess.run([str(args.dxc), str(source), '-T', 'vs_6_0', '-E', 'main',
            '-DREBLUE_RECOMP', '-DLEGO_NATIVE_GPU', '-HV', '2021', '-Wno-ignored-attributes',
            '-Fo', str(binary)], capture_output=True, text=True, timeout=120)
        (out/(stem+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        data = binary.read_bytes()
        if data[:4] != b'DXBC':
            raise ValueError('Missing DXIL container')
        return hash_, spec, data, dict(hash=f'{hash_:016X}', spec=spec, kind=1,
            source=str(path.resolve()), source_sha256=digest(path.read_bytes()), sha256=digest(data),
            policy='canonical-static-clip' if canonical else 'precise-position')

    blob = bytearray(base_blob)
    records = []
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = sorted(pool.map(compile_one, jobs))
    for hash_, spec, data, record in results:
        while len(blob) % 16:
            blob.append(0)
        entries[hash_, spec, 1] = len(blob), len(data)
        blob.extend(data)
        records.append(record)
    assert bytes(blob[:len(base_blob)]) == base_blob
    assert all(entries[key] == value for key, value in original_entries.items())
    bank = out/'float-blend-indices-bank.bin'
    bank.write_bytes(blob)
    rows = [f'{{0x{h:016X}ull,{s},{o},{n},{k}}}' for (h,s,k),(o,n) in sorted(entries.items())]
    (out/'float-blend-indices-bank.cpp').write_text(
        'const FloatBlendIndicesEntry g_float_blend_indices_entries[] = {\n'+',\n'.join(rows)+'\n};\n')
    asm = out/'float-blend-indices-bank.s'
    asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_floatBlendIndicesBlob\n'
        'g_floatBlendIndicesBlob:\n.incbin "'+bank.as_posix()+'"\n')
    subprocess.run([str(args.llvm_mc), '-triple=x86_64-pc-windows-msvc', '-filetype=obj', str(asm),
        '-o', str(out/'float-blend-indices-bank.obj')], check=True, timeout=120)
    report = dict(abi=624, base_entries=len(original_entries), added_entries=len(results),
        total_entries=len(entries), base_bytes_preserved=len(base_blob), base_sha256=digest(base_blob),
        bank_sha256=digest(blob), entries=records)
    (out/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Preserved {len(original_entries)} entries; appended {len(results)} position variants.')


if __name__ == '__main__':
    main()
