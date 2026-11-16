"""Windowless material contribution, discard, blend and motion-retention oracle."""
import argparse,hashlib,json,os,re,runpy,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--bank',type=Path,required=True);p.add_argument('--expanded',type=Path,required=True)
p.add_argument('--hash',required=True);p.add_argument('--dxc',type=Path,required=True);p.add_argument('--compiler',default='clang-cl');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True);scripts=Path(__file__).resolve().parent
split=runpy.run_path(str(scripts/'prepare_temporal_motion.py'))['split_entry']
build=runpy.run_path(str(scripts/'prepare_temporal_coverage.py'))['build']
payload=(a.bank/'temporal-bank.bin').read_bytes();entries={ (int(h,16),int(st),int(sp)):(int(o),int(n)) for h,st,sp,o,n,_ in re.findall(r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',(a.bank/'temporal-bank.cpp').read_text())}
def run(cmd,**kw):
 r=subprocess.run([str(v) for v in cmd],capture_output=True,text=True,timeout=120,**kw)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return r
def compile_(text,name,stage):
 source=a.output/(name+'.hlsl');source.write_text(text);binary=a.output/(name+'.dxil')
 run([a.dxc,source,'-T',stage+'_6_0','-E','main','-HV','2021','-Wno-ignored-attributes','-Fo',binary]);return binary
exe=a.output/'coverage-gpu.exe'
run([a.compiler,scripts/'test_temporal_material_gpu.cpp','/std:c++20','/O2','/MD','/EHsc','/Fo'+str(a.output/'fixture.obj'),'/Fe'+str(exe),'/link','d3d12.lib','dxgi.lib','dxguid.lib'])
results=[];negatives=[];h=int(a.hash,16)
for spec in range(4):
 source=(a.expanded/f'{h:016X}-ps-{spec}-expanded.hlsl').read_text();clean=re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$','',source,flags=re.M)
 _,params,_=split(clean);inputs=[v for v in params if v[0]=='in' and not v[3].upper().startswith('SV_')]
 signature=['out float4 position:SV_Position']+[f'out {v[1]} v{i}:{v[3]}' for i,v in enumerate(inputs)]
 vs='void main(uint id:SV_VertexID,'+','.join(signature)+'){float2 xy=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);position=float4(xy,.5,1);'
 for i,v in enumerate(inputs):
  assert v[1]=='float4';vs+=f'v{i}=float4(xy*.25+.5,{.2+i*.01},1);'
 vertex=compile_(vs+'}',f'{spec}-original-signature-vs','vs');original=compile_(clean,f'{spec}-original-ps','ps')
 for mode in range(2,7):
  offset,size=entries[h,mode,spec];blob=payload[offset:offset+size];binary=a.output/f'{spec}-{mode}-bank.dxil';binary.write_bytes(blob)
  cmd=[exe,vertex,original,binary,int(bool(spec&2)),mode];r=run(cmd)
  results.append(dict(spec=spec,mode=mode,sha256=hashlib.sha256(blob).hexdigest(),stdout=r.stdout));print(spec,mode,r.stdout.strip(),flush=True)
  if spec==2 and mode==4:
   for name,key in [('motion-write','LEGO_TEST_WRITE_MOTION'),('reactive-copy','LEGO_TEST_COPY_REACTIVE')]:
    env=dict(os.environ);env[key]='1';r=subprocess.run([str(v) for v in cmd],env=env,capture_output=True,text=True,timeout=120)
    assert r.returncode==23,(name,r.returncode,r.stdout,r.stderr);negatives.append(dict(name=name,exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
   modified=build(clean,'add_alpha').replace('if (!LegoAlphaPass(oC0.w, g_AlphaThreshold, g_AlphaFunction)) discard;','')
   mutant=compile_(modified,'removed-discard','ps');r=subprocess.run([str(v) for v in [exe,vertex,original,mutant,1,mode]],capture_output=True,text=True,timeout=120)
   assert r.returncode==23,(r.returncode,r.stdout,r.stderr);negatives.append(dict(name='removed-discard',exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
report=dict(passed=True,game_launched=False,window_opened=False,exact_bank_bytes=True,
 scope='controlled gradient textures/constants with actual UI material PS; all alpha functions, black additive case, MAX rejection and preserved scene motion',
 variants=len(results),cases_per_variant=192,total_cases=len(results)*192,results=results,negatives=negatives)
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS material contribution bank, exact discard/depth, original VS signature, retained motion and previous rejection; three mutations rejected')
