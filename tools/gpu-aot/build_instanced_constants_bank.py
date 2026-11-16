"""Build per-instance VS variants whose LOAD_C reads come from a t0/space5 StructuredBuffer at SV_InstanceID * 256 + INDEX, skipping shaders the transform cannot apply to exactly."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess

DEFINE = '#define LOAD_C(INDEX) g_FloatConstants[INDEX]'
BANK = ('StructuredBuffer<float4> g_InstanceFloatConstants : register(t0, space5);\n'
        'static uint g_LegoBank;\n'
        '#define LOAD_C(INDEX) g_InstanceFloatConstants[g_LegoBank + (INDEX)]')
INSTANCE = 'iLegoInstanceId'
POSITION = '\tout float4 oPos : SV_Position'
SPECS = ('uint g_SpecConstants();', 'uint g_SpecConstants() { return 0; }')
RESERVED = ('g_InstanceFloatConstants', 'g_LegoBank', INSTANCE, 'SV_InstanceID')


class Skip(Exception):
    pass


def instanced(text):
    """Return (variant source, LOAD_C read count) or raise Skip."""
    if text.count(DEFINE) != 1: raise Skip('expected one cbuffer LOAD_C definition')
    if text.count('\tfloat4 g_FloatConstants[256];') != 1: raise Skip('expected the 256-entry VS cbuffer')
    if len(re.findall(r'\bg_FloatConstants\b', text)) != 2:
        raise Skip('g_FloatConstants is read outside LOAD_C')
    for name in RESERVED:
        if re.search(r'\b' + name + r'\b', text): raise Skip('source already uses ' + name)
    if re.search(r'register\(\s*t0\s*,\s*space5\s*\)', text): raise Skip('t0 space5 is already bound')
    mains = [m.start() for m in re.finditer(r'^void main\(', text, re.M)]
    if len(mains) != 1: raise Skip('expected one main entry point')
    if text.count(POSITION) != 1: raise Skip('expected one position output')
    position = text.find(POSITION, mains[0])
    if position < 0: raise Skip('position output is not a main parameter')
    close = re.compile(r'\)\s*\{\n').search(text, position)
    if not close or ')' in text[position:close.start()]: raise Skip('unrecognized main signature')
    body = close.end()
    # g_LegoBank is assigned first in main; any read before that point would see 0.
    reads = [m.start() for m in re.finditer(r'\bLOAD_C\b', text)
             if not text.startswith('#define LOAD_C(', m.start() - 8)]
    if any(read < body for read in reads): raise Skip('LOAD_C is read before main sets g_LegoBank')
    source = (text[:position] + f'\tin uint {INSTANCE} : SV_InstanceID,\n' + text[position:body] +
              f'\tg_LegoBank = {INSTANCE} * 256;\n' + text[body:])
    return source.replace(DEFINE, BANK), len(reads)


def write_variant(path, variant, spec):
    # LF keeps each variant byte-comparable with its XenosRecomp input.
    with open(path, 'w', newline='\n') as f:
        f.write(variant.replace(SPECS[1], f'uint g_SpecConstants() {{ return {spec}; }}'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--source', type=Path, action='append', required=True,
        help='Folder of ABI624 VS HLSL (XenosRecomp dumps or prepared *-vs-0-input.hlsl); repeat for several dumps')
    p.add_argument('--dxc', type=Path)
    p.add_argument('--llvm-mc', type=Path)
    p.add_argument('--hashes', type=Path,
        help='Optional file restricting the bank to these 16-digit VS hashes')
    p.add_argument('--jobs', type=int, default=6)
    p.add_argument('--transform-only', action='store_true',
        help='Write variant HLSL and report coverage without compiling')
    a = p.parse_args()
    if not a.transform_only and (not a.dxc or not a.llvm_mc):
        p.error('--dxc and --llvm-mc are required unless --transform-only')
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    sources = {}; rejected = {}
    for folder in a.source:
        for path in sorted(folder.glob('*.hlsl')):
            text = path.read_text()
            match = re.search(r'// vertex shader\s+hash=0x([0-9A-Fa-f]+)(?:\s+specConstants=0x([0-9A-Fa-f]+))?', text[:300])
            # Pixel shaders and copies already specialized to a nonzero value are not inputs.
            if not match or re.search(r'uint g_SpecConstants\(\) \{ return [1-9]', text): continue
            if 'packoffset(c38)' not in text: raise ValueError('Expected native ABI624 source: ' + str(path))
            hash_ = int(match[1], 16)
            if sum(text.count(spec) for spec in SPECS) != 1:
                rejected[hash_] = dict(hash=f'{hash_:016X}', source=str(path),
                    reason='expected one unspecialized g_SpecConstants'); continue
            text = text.replace(SPECS[0], SPECS[1])
            if hash_ in sources and sources[hash_][1] != text:
                raise ValueError(f'Conflicting sources for {hash_:016X}: {sources[hash_][0]} and {path}')
            sources.setdefault(hash_, (path, text, None if match[2] is None else int(match[2], 16)))
    if a.hashes:
        wanted = {int(h, 16) for h in re.findall(r'\b(?:0x)?([0-9A-Fa-f]{16})\b', a.hashes.read_text())}
        sources = {h: v for h, v in sources.items() if h in wanted}
        rejected = {h: v for h, v in rejected.items() if h in wanted}
    if not sources and not rejected: raise ValueError('No vertex shader sources')

    jobs = []; skipped = [entry for hash_, entry in sorted(rejected.items()) if hash_ not in sources]
    inputs = len(sources) + len(skipped)
    for hash_, (path, text, mask) in sorted(sources.items()):
        try:
            # Runtime lookups key on spec & 1, which matches the base lookup only for R11G11B10 normals.
            if mask is not None and mask & ~1: raise Skip(f'specialization mask 0x{mask:X} exceeds bit 0')
            variant, reads = instanced(text)
        except Skip as reason:
            skipped.append(dict(hash=f'{hash_:016X}', source=str(path), reason=str(reason))); continue
        # A shader without specialization compiles once; both spec rows share its bytes.
        for spec in ((0,) if mask == 0 else (0, 1)): jobs.append((hash_, spec, reads, path, variant))
    if a.transform_only:
        for entry in skipped: print(f"Skipped {entry['hash']}: {entry['reason']} ({entry['source']})")
        for hash_, spec, reads, path, variant in jobs:
            write_variant(out/f'{hash_:016X}-vs-{spec}-instanced.hlsl', variant, spec)
        print(f'Transformed {len({job[0] for job in jobs})} of {inputs} vertex shaders; {len(skipped)} skipped.')
        return
    if not jobs: raise ValueError('No vertex shaders accept the instanced transform')

    def compile_one(job):
        hash_, spec, reads, path, variant = job
        stem = f'{hash_:016X}-vs-{spec}-instanced'
        source = out/(stem+'.hlsl')
        write_variant(source, variant, spec)
        binary = out/(stem+'.dxil')
        compiled = subprocess.run([str(a.dxc), str(source), '-T','vs_6_0','-E','main',
            '-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU','-HV','2021','-all-resources-bound',
            '-Wno-ignored-attributes','-Fo',str(binary)], capture_output=True, text=True, timeout=120)
        if compiled.returncode: return hash_, spec, 'DXC failed: ' + compiled.stderr.strip()[:300], str(path)
        data = binary.read_bytes()
        # DXC reflection must show the instance input and, when constants are read, the bank SRV.
        disassembly = subprocess.run([str(a.dxc),'-dumpbin',str(binary)],
            check=True, capture_output=True, text=True, timeout=30).stdout
        if not re.search(r'^; SV_InstanceID\s+0\s+x\s+\d+\s+INSTID\s+uint', disassembly, re.M):
            return hash_, spec, 'missing SV_InstanceID signature', str(path)
        if reads and not re.search(r'^; g_InstanceFloatConstants\s+texture\s+struct\s+r/o\s+T\d+\s+t0,space5\s+1$',
                                   disassembly, re.M):
            return hash_, spec, 'missing t0 space5 constant bank', str(path)
        return hash_, spec, data, str(path)

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool: compiled = list(pool.map(compile_one, jobs))
    results = []
    for hash_, spec, data, path in compiled:
        if isinstance(data, str): skipped.append(dict(hash=f'{hash_:016X}', spec=spec, source=path, reason=data))
        else: results.append((hash_, spec, data, path, hashlib.sha256(Path(path).read_bytes()).hexdigest()))
    for entry in skipped: print(f"Skipped {entry['hash']}: {entry['reason']} ({entry['source']})")
    if not results: raise ValueError('No instanced vertex variants compiled')
    families = len({result[0] for result in results})
    rows_by_key = {}
    for hash_, spec, data, path, source_hash in results:
        rows_by_key[(hash_, spec)] = (data, path, source_hash)
        if sources[hash_][2] == 0: rows_by_key[(hash_, 1)] = (data, path, source_hash)
    blob = bytearray(); rows = []; records = []; offsets = {}
    for (hash_, spec), (data, path, source_hash) in sorted(rows_by_key.items()):
        digest = hashlib.sha256(data).hexdigest()
        if digest not in offsets:
            while len(blob)%16: blob.append(0)
            offsets[digest] = len(blob); blob.extend(data)
        rows.append(f'{{0x{hash_:016X}ull,{spec},{offsets[digest]},{len(data)}}}')
        records.append(dict(hash=f'{hash_:016X}', spec=spec, source=path,
            source_sha256=source_hash, sha256=digest))
    bank = out/'instanced-constants-bank.bin'; bank.write_bytes(blob)
    (out/'instanced-constants-bank.cpp').write_text(
        'const InstancedConstantsEntry g_instanced_constants_entries[] = {\n'+',\n'.join(rows)+'\n};\n')
    asm = out/'instanced-constants-bank.s'
    asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_instancedConstantsBlob\n'
        'g_instancedConstantsBlob:\n.incbin "'+bank.as_posix()+'"\n')
    # Same assembler contract as embed_linked_bank.py: llvm-mc, or clang when that is the host tool.
    flags = (['--target=x86_64-pc-windows-msvc','-c','-x','assembler']
             if a.llvm_mc.stem.lower() in ('clang','clang++','clang-cl')
             else ['-triple=x86_64-pc-windows-msvc','-filetype=obj'])
    subprocess.run([str(a.llvm_mc),*flags,str(asm),'-o',str(out/'instanced-constants-bank.obj')],
        check=True, timeout=120)
    (out/'verification.json').write_text(json.dumps(dict(families=families, sources=inputs,
        variants=len(rows), compiled=len(results), bytes=len(blob), abi=624, bank_float4s=256,
        binding='t0,space5', entries=records, skipped=skipped), indent=2)+'\n')
    print(f'Compiled {len(results)} instanced vertex variants for {families} of {inputs} shaders; '
          f'{len(rows)} entries, {len(blob)} bytes; {len(skipped)} skipped.')


if __name__ == '__main__': main()
