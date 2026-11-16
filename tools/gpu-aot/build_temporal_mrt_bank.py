"""Append experimental four-color+motion pixel shaders in reserved stage7.

Stages2..6 belong to material reactive coverage. This bank is not selected by
the production replay path and does not establish in-game MRT integration.
"""
import argparse,concurrent.futures,hashlib,json,re,runpy,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
p.add_argument('--dxc',type=Path,required=True);p.add_argument('--llvm-mc',type=Path,required=True);a=p.parse_args()
build=runpy.run_path(str(Path(__file__).with_name('prepare_temporal_motion.py')))['build']
source=a.source.resolve();out=a.output.resolve()
assert not out.exists(),'Use a fresh output directory; never replace an existing bank'
out.mkdir(parents=True)
rows=[tuple(int(v,16)if i==0 else int(v)for i,v in enumerate(m)) for m in re.findall(r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',(source/'temporal-bank.cpp').read_text())]
assert rows and len(rows)==len(set(r[:3]for r in rows))
assert not any(r[1]==7 for r in rows),'Combined stage7 already exists'
original=(source/'temporal-bank.bin').read_bytes();blob=bytearray(original)
def compile(row):
 h,stage,spec,offset,size,camera=row
 path=source/f'{h:016X}-ps-{spec}-expanded.hlsl'
 text=build(path.read_text(),'ps_mrt');shader=out/f'{h:016X}-ps-mrt-{spec}.hlsl';shader.write_text(text)
 binary=shader.with_suffix('.dxil')
 result=subprocess.run([str(a.dxc),str(shader),'-T','ps_6_0','-E','main','-HV','2021','-Wno-ignored-attributes','-Fo',str(binary)],capture_output=True,text=True,timeout=120)
 if result.returncode:raise RuntimeError(str(shader)+': '+result.stderr)
 return h,spec,binary.read_bytes()
jobs=[r for r in rows if r[1]==1 and r[0]]
with concurrent.futures.ThreadPoolExecutor(max_workers=6)as pool:results=list(pool.map(compile,jobs))
entries=[]
for h,spec,data in results:
 while len(blob)%16:blob.append(0)
 rows.append((h,7,spec,len(blob),len(data),0));entries.append(dict(hash=f'{h:016X}',stage=7,spec=spec,sha256=hashlib.sha256(data).hexdigest()));blob.extend(data)
assert len(rows)==len(set(r[:3]for r in rows))
bank=out/'temporal-bank.bin';bank.write_bytes(blob)
assert blob[:len(original)]==original
(out/'temporal-bank.cpp').write_text('const TemporalShaderEntry g_temporal_entries[] = {\n'+',\n'.join('{0x%016Xull,%d,%d,%d,%d,%d}'%r for r in sorted(rows))+'\n};\n')
asm=out/'temporal-bank.s';asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_temporalShaderBlob\ng_temporalShaderBlob:\n.incbin "'+bank.as_posix()+'"\n')
subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(out/'temporal-bank.obj')],check=True,timeout=60)
(out/'verification.json').write_text(json.dumps(dict(original_variants=len(rows)-len(entries),original_bytes=len(original),original_sha256=hashlib.sha256(original).hexdigest(),original_bytes_unchanged=True,combined_variants=len(entries),bytes=len(blob),entries=entries),indent=2))
print('Preserved',len(original),'original bytes; appended',len(entries),'combined pixel shaders.')
