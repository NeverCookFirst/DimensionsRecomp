"""Verify vertex texture transitions with actual D3D12 vertex/pixel sampling."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--plume-lib',type=Path,required=True)
p.add_argument('--dxc',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
shaders={
 'vs':'Texture2D<float4> t:register(t0); void main(uint id:SV_VertexID,out float4 p:SV_Position,out float4 c:TEXCOORD0){p=float4(id==1?3:-1,id==2?3:-1,0,1);c=t.Load(int3(0,0,0));}',
 'ps':'Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position,float4 c:TEXCOORD0):SV_Target{return .5*c+.5*t.Load(int3(0,0,0));}'
}
for stage,code in shaders.items():
 src=out/(stage+'.hlsl');src.write_text(code)
 subprocess.run([str(a.dxc),str(src),'-T',stage+'_6_0','-E','main','-Fo',str(out/(stage+'.dxil'))],check=True,timeout=30)
exe=out/'vertex-texture-read.exe'
cmd=['clang-cl',str(Path(__file__).with_suffix('.cpp')),str(root/'rexlego/src/gpu_native/native_recording.cpp'),'/std:c++20','/O2','/MD','/EHsc','/UNDEBUG',
 '/I'+str(root/'rexlego/src'),'/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
 '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
compiled=subprocess.run(cmd,capture_output=True,text=True,timeout=90);(out/'compile.log').write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
records=[]
for cache,missing in [('0',False),('1',False),('1',True)]:
 r=subprocess.run([str(exe),str(out/'vs.dxil'),str(out/'ps.dxil'),*(['missing-barrier'] if missing else [])],env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=90)
 (out/('negative.log' if missing else f'gpu-cache-{cache}.log')).write_text(r.stdout+r.stderr)
 assert r.returncode==(23 if missing else 0),r.stdout+r.stderr
 if not missing:assert 'PASS 64 GPU readbacks' in r.stdout
 records.append(dict(cache=cache,missing_barrier=missing,exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
(out/'verification.json').write_text(json.dumps(dict(passed=True,real_d3d12=True,records=records,scope='Legal VS+PS read states and actual shader/readback correctness, not a gameplay effects result'),indent=2)+'\n')
print('PASS 128 GPU readbacks with state cache off/on and omitted-barrier negative control')
