"""Build declaration and verified position variants for native TT shaders.

The original uint signatures remain in the main banks for integer declarations.
Private HLSL and bytecode stay in the selected output directory. No GPU or game
is started by this tool. The shader math and shared constants ABI are unchanged.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess


def float_input(source):
    pattern = r'\bin\s+uint4\s+iBlendIndices0\s*:\s*BLENDINDICES0\b'
    if len(re.findall(pattern, source)) != 1:
        raise ValueError('Expected one uint BLENDINDICES0 input')
    # Keep every fetch expression and integer conversion in the translated body.
    return re.sub(pattern, 'in float4 iBlendIndices0 : BLENDINDICES0', source)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--position-source', type=Path, action='append', default=[],
        help='Explicit verified shader source; preserve position arithmetic only')
    p.add_argument('--dxc', type=Path, required=True)
    p.add_argument('--llvm-mc', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    jobs = []
    for path in sorted(a.source.glob('*-vs-0-input.hlsl')):
        text = path.read_text()
        if not re.search(r'\bin\s+uint4\s+iBlendIndices0\s*:\s*BLENDINDICES0\b', text):
            continue
        match = re.search(r'// vertex shader\s+hash=0x([0-9A-Fa-f]+)', text[:300])
        if not match: raise ValueError('Missing shader identity: ' + str(path))
        if 'packoffset(c38)' not in text: raise ValueError('Expected native ABI624 source')
        for spec in range(2): jobs.append((int(match[1],16), spec, 0, path, text))
    for path in a.position_source:
        text = path.read_text()
        match = re.search(r'// vertex shader\s+hash=0x([0-9A-Fa-f]+)', text[:300])
        if not match or 'packoffset(c38)' not in text: raise ValueError('Expected ABI624 vertex source')
        if text.count('out float4 oPos : SV_Position') != 1: raise ValueError('Expected one position output')
        for spec in range(2): jobs.append((int(match[1],16), spec, 1, path, text))
    if not jobs: raise ValueError('No matching vertex shaders')

    def compile_one(job):
        hash_, spec, kind, path, text = job
        # Each input is the native source with an already resolved spec0 function.
        old = 'uint g_SpecConstants() { return 0; }'
        if text.count(old) != 1: raise ValueError('Expected one resolved specialization')
        text = text.replace(old, f'uint g_SpecConstants() {{ return {spec}; }}')
        stem = f'{hash_:016X}-vs-{spec}' + ('-position' if kind else '')
        source = out/(stem+'.hlsl')
        source.write_text(text.replace('out float4 oPos : SV_Position','out precise float4 oPos : SV_Position') if kind else float_input(text))
        binary = out/(stem+'.dxil')
        subprocess.run([str(a.dxc), str(source), '-T','vs_6_0','-E','main',
            '-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU','-HV','2021',
            '-Wno-ignored-attributes','-Fo',str(binary)], check=True, capture_output=True, timeout=120)
        data = binary.read_bytes()
        # DXC reflection must agree with the declaration-driven production route.
        disassembly = subprocess.run([str(a.dxc),'-dumpbin',str(binary)],
            check=True, capture_output=True, text=True, timeout=30).stdout
        if not kind and not re.search(r'^; BLENDINDICES\s+0\s+\w+\s+\d+\s+NONE\s+float',disassembly,re.M):
            raise ValueError('Missing float BLENDINDICES signature: '+stem)
        return hash_, spec, kind, data, str(path), hashlib.sha256(path.read_bytes()).hexdigest()

    with ThreadPoolExecutor(max_workers=6) as pool: results = sorted(pool.map(compile_one,jobs))
    blob=bytearray(); rows=[]; records=[]
    for hash_,spec,kind,data,path,source_hash in results:
        while len(blob)%16: blob.append(0)
        rows.append(f'{{0x{hash_:016X}ull,{spec},{len(blob)},{len(data)},{kind}}}')
        records.append(dict(hash=f'{hash_:016X}',spec=spec,kind=kind,source=path,
            source_sha256=source_hash,sha256=hashlib.sha256(data).hexdigest()))
        blob.extend(data)
    bank=out/'float-blend-indices-bank.bin'; bank.write_bytes(blob)
    (out/'float-blend-indices-bank.cpp').write_text(
        'const FloatBlendIndicesEntry g_float_blend_indices_entries[] = {\n'+',\n'.join(rows)+'\n};\n')
    asm=out/'float-blend-indices-bank.s'
    asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_floatBlendIndicesBlob\n'
        'g_floatBlendIndicesBlob:\n.incbin "'+bank.as_posix()+'"\n')
    subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),
        '-o',str(out/'float-blend-indices-bank.obj')],check=True,timeout=120)
    (out/'verification.json').write_text(json.dumps(dict(families=len(results)//2,
        variants=len(results),bytes=len(blob),abi=624,entries=records),indent=2)+'\n')
    print(f'Compiled {len(results)} native vertex variants; {len(blob)} bytes.')


if __name__ == '__main__': main()
