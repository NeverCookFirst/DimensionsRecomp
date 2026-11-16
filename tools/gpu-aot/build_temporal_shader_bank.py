"""Compile captured native material motion shaders into an offline COFF bank."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import runpy
build=runpy.run_path(str(Path(__file__).with_name('prepare_temporal_motion.py')))['build']

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--source',type=Path,action='append',required=True)
p.add_argument('--override',type=Path,action='append',default=[])
p.add_argument('--dxc',type=Path,required=True)
p.add_argument('--llvm-mc',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources={}
for folder in a.source:
 for path in folder.glob('*.hlsl'):
  text=path.read_text();match=re.search(r'// (vertex|pixel) shader\s+hash=0x([0-9A-Fa-f]+)',text[:300])
  if match:sources[int(match[2],16)]=(path,text,'vs' if match[1]=='vertex' else 'ps')
for path in a.override:
 text=path.read_text();match=re.search(r'// (vertex|pixel) shader\s+hash=0x([0-9A-Fa-f]+)',text[:300])
 if not match:raise ValueError('Missing source identity: '+str(path))
 sources[int(match[2],16)]=(path,text,'vs' if match[1]=='vertex' else 'ps')

def compile_shader(job):
 hash_,spec,path,text,stage=job
 stem=f'{hash_:016X}-{stage}-{spec}'
 prepared=out/(stem+'-input.hlsl');expanded=out/(stem+'-expanded.hlsl')
 prepared.write_text(text.replace('uint g_SpecConstants();',f'uint g_SpecConstants() {{ return {spec}; }}'))
 subprocess.run([str(a.dxc),str(prepared),'-T',stage+'_6_0','-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU',
  '-HV','2021','-P','-Fi',str(expanded)],check=True,capture_output=True,timeout=120)
 motion=out/(stem+'-motion.hlsl');motion.write_text(build(expanded.read_text(),stage))
 binary=out/(stem+'.dxil')
 result=subprocess.run([str(a.dxc),str(motion),'-T',stage+'_6_0','-E','main','-HV','2021',
  '-Wno-ignored-attributes','-Fo',str(binary)],capture_output=True,text=True,timeout=120)
 if result.returncode:raise RuntimeError(stem+': '+result.stderr)
 return hash_,0 if stage=='vs' else 1,spec,binary.read_bytes(),str(path)

jobs=[(hash_,spec,path,text,stage)for hash_,(path,text,stage)in sorted(sources.items())for spec in range(2 if stage=='vs' else 4)]
with concurrent.futures.ThreadPoolExecutor(max_workers=6)as pool:
 results=list(pool.map(compile_shader,jobs))
# Unknown geometry writes a full reactive mask, never inventing previous motion.
fallback=out/'reactive-fallback.hlsl';fallback.write_text('void main(out float2 velocity : SV_Target0, out float reactive : SV_Target1) { velocity=0; reactive=1; }')
fallback_dxil=out/'reactive-fallback.dxil'
subprocess.run([str(a.dxc),str(fallback),'-T','ps_6_0','-E','main','-Fo',str(fallback_dxil)],check=True,capture_output=True,timeout=120)
results.append((0,1,0,fallback_dxil.read_bytes(),'reactive fallback'))
results.sort(key=lambda entry:entry[:3])
blob=bytearray();rows=[];records=[]
for hash_,stage,spec,code,path in results:
 while len(blob)%16:blob.append(0)
 camera = int(stage == 0 and all(re.search(r'\bLOAD_C\(\s*'+str(i)+r'\s*\)', (out/f'{hash_:016X}-vs-{spec}-input.hlsl').read_text()) for i in (0,1,2,3,48,49,50,51)))
 rows.append(f'{{0x{hash_:016X}ull,{stage},{spec},{len(blob)},{len(code)},{camera}}}')
 records.append({'hash':f'{hash_:016X}','stage':stage,'spec':spec,'source':path,'sha256':hashlib.sha256(code).hexdigest()})
 blob.extend(code)
bank=out/'temporal-bank.bin';bank.write_bytes(blob)
(out/'temporal-bank.cpp').write_text('const TemporalShaderEntry g_temporal_entries[] = {\n'+',\n'.join(rows)+'\n};\n')
asm=out/'temporal-bank.s';asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_temporalShaderBlob\ng_temporalShaderBlob:\n.incbin "'+bank.as_posix()+'"\n')
subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(out/'temporal-bank.obj')],check=True,timeout=120)
(out/'verification.json').write_text(json.dumps({'sources':len(sources),'variants':len(results),'bytes':len(blob),
 'game_launched':False,'frame_generation':False,'entries':records},indent=2)+'\n')
print(f'Compiled {len(results)} motion variants from {len(sources)} captured programs; {len(blob)} bytes.')
