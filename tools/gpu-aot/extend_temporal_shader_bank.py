"""Append verified offline additions, preserving the original temporal bank."""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('base',type=Path);p.add_argument('additions',type=Path);p.add_argument('output',type=Path)
p.add_argument('--llvm-mc',type=Path,required=True);a=p.parse_args()
assert not a.output.exists(),'Use a new output directory'
def read(folder):
    data=(folder/'temporal-bank.bin').read_bytes()
    rows=[tuple(int(v,16) if i==0 else int(v) for i,v in enumerate(row)) for row in re.findall(
        r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',
        (folder/'temporal-bank.cpp').read_text())]
    assert rows and len(rows)==len({row[:3] for row in rows})
    for h,stage,spec,offset,size,camera in rows:
        assert size>0 and offset+size<=len(data) and data[offset:offset+4]==b'DXBC'
    return rows,data
rows,original=read(a.base);extra,payload=read(a.additions)
blob=bytearray(original);known={row[:3] for row in rows};added=[]
for h,stage,spec,offset,size,camera in extra:
    key=h,stage,spec
    if key==(0,1,0):continue # Keep the existing conservative fallback unchanged.
    assert key not in known,f'Refusing to replace {key}'
    while len(blob)%16:blob.append(0)
    code=payload[offset:offset+size]
    rows.append((h,stage,spec,len(blob),size,camera));blob.extend(code);known.add(key)
    added.append(dict(hash=f'{h:016X}',stage=stage,spec=spec,bytes=size,sha256=hashlib.sha256(code).hexdigest()))
assert added and bytes(blob[:len(original)])==original
a.output.mkdir(parents=True);binary=a.output/'temporal-bank.bin';binary.write_bytes(blob)
(a.output/'temporal-bank.cpp').write_text('const TemporalShaderEntry g_temporal_entries[] = {\n'+
    ',\n'.join('{0x%016Xull,%d,%d,%d,%d,%d}'%row for row in sorted(rows))+'\n};\n')
asm=a.output/'temporal-bank.s';asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_temporalShaderBlob\ng_temporalShaderBlob:\n.incbin "'+binary.resolve().as_posix()+'"\n')
subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(a.output/'temporal-bank.obj')],check=True)
for folder in (a.base,a.additions):
    for path in folder.iterdir():
        if path.suffix not in ('.hlsl','.dxil'):continue
        target=a.output/path.name
        if not target.exists():shutil.copy2(path,target)
check_rows,check_data=read(a.output)
assert sorted(rows)==check_rows and bytes(blob)==check_data
report=dict(original_bank=str(a.base),additions_bank=str(a.additions),original_variants=len(rows)-len(added),
    original_bytes=len(original),original_sha256=hashlib.sha256(original).hexdigest(),
    original_bytes_unchanged=True,added=added,variants=len(rows),bytes=len(blob),
    sha256=hashlib.sha256(blob).hexdigest(),game_launched=False)
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Preserved {report["original_variants"]} original variants; appended {len(added)} variants')
