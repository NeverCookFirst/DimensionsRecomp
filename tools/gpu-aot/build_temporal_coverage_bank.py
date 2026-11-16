"""Compile original-signature material reactive PS variants; no game launch."""
import argparse,concurrent.futures,hashlib,json,subprocess,runpy,re,zlib
from pathlib import Path
generator=runpy.run_path(str(Path(__file__).with_name('prepare_temporal_coverage.py')))
MODES,build=generator['MODES'],generator['build']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--expanded',type=Path,required=True);p.add_argument('--hash',action='append',required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--dxc',type=Path,required=True)
p.add_argument('--alias',action='append',default=[],help='SourceHash=ExactLinkedAlias')
p.add_argument('--material-bank',type=Path)
p.add_argument('--llvm-mc',type=Path,required=True);a=p.parse_args()
assert not a.output.exists(),'Use a fresh output directory'
a.output.mkdir(parents=True);out=a.output.resolve()
def compile_(job):
 h,spec,mode=job;source=a.expanded/f'{h}-ps-{spec}-expanded.hlsl'
 code=build(source.read_text(),mode);stem=f'{h}-coverage-{MODES[mode]}-{spec}'
 path=out/(stem+'.hlsl');binary=out/(stem+'.dxil');path.write_text(code)
 result=subprocess.run([str(a.dxc),str(path),'-T','ps_6_0','-E','main','-HV','2021','-Wno-ignored-attributes','-Fo',str(binary)],capture_output=True,text=True,timeout=120)
 if result.returncode:raise RuntimeError(stem+': '+result.stderr)
 return int(h,16),MODES[mode],spec,binary.read_bytes(),hashlib.sha256(source.read_bytes()).hexdigest()
jobs=[(h.upper().zfill(16),spec,mode) for h in a.hash for spec in range(4) for mode in MODES]
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:results=sorted(pool.map(compile_,jobs))
aliases=[]
if a.alias:
 assert a.material_bank,'Exact aliases require the linked material bank'
 payload=zlib.decompress((a.material_bank/'linked-bank.bin').read_bytes());identities={}
 for h,spec,offset,size in re.findall(r'\{\s*0x([\dA-Fa-f]+),\s*0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+)\s*\}',(a.material_bank/'linked-bank-metadata.cpp').read_text()):
  offset,size=int(offset),int(size);identities.setdefault(int(h,16),{})[int(spec,16)]=hashlib.sha256(payload[offset:offset+size]).hexdigest()
 for pair in a.alias:
  source,alias=(int(v,16) for v in pair.split('='))
  assert source!=alias and identities[source]==identities[alias] and set(identities[source])=={0,2}
  additions=[(alias,stage,spec,code,sha) for h,stage,spec,code,sha in results if h==source]
  assert len(additions)==20
  results.extend(additions);aliases.append(dict(source=f'{source:016X}',alias=f'{alias:016X}',linked_variants=identities[source]))
 results.sort()
blob=bytearray();rows=[];proof=[];stored={}
for h,stage,spec,code,source_sha in results:
 sha=hashlib.sha256(code).hexdigest()
 if sha not in stored:
  while len(blob)%16:blob.append(0)
  stored[sha]=len(blob);blob.extend(code)
 offset=stored[sha];rows.append((h,stage,spec,offset,len(code),0))
 proof.append(dict(hash=f'{h:016X}',stage=stage,spec=spec,source_sha256=source_sha,sha256=sha))
binary=out/'temporal-bank.bin';binary.write_bytes(blob)
(out/'temporal-bank.cpp').write_text('const TemporalShaderEntry g_temporal_entries[] = {\n'+',\n'.join('{0x%016Xull,%d,%d,%d,%d,%d}'%r for r in rows)+'\n};\n')
asm=out/'temporal-bank.s';asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_temporalShaderBlob\ng_temporalShaderBlob:\n.incbin "'+binary.as_posix()+'"\n')
subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(out/'temporal-bank.obj')],check=True)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,variants=len(rows),unique_bytecode=len(stored),exact_aliases=aliases,entries=proof),indent=2)+'\n')
print('PASS',len(rows),'material contribution variants;',len(stored),'distinct bytecode payloads')
