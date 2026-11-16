"""Compile and run the windowless production declaration/type regression."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
p.add_argument('--dxc',required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
def run(args):return subprocess.run([str(v)for v in args],check=True,capture_output=True,text=True,timeout=90)
for kind in ['float','uint']:
 source=out/f'typed-vs-{kind}.hlsl'
 source.write_text(f'void main(in {kind}4 w : BLENDINDICES0, uint id : SV_VertexID, '
  'out float4 pos : SV_Position, out float4 weight : COLOR0) '
  '{ pos=float4(id==2?3:-1,id==1?3:-1,0,1); weight=w; }')
 run([a.dxc,source,'-T','vs_6_0','-E','main','-Fo',out/f'typed-vs-{kind}.dxil'])
source=out/'typed-ps.hlsl'
source.write_text('float4 main(float4 pos : SV_Position, float4 weight : COLOR0) '
 ': SV_Target0 { return weight; }')
run([a.dxc,source,'-T','ps_6_0','-E','main','-Fo',out/'typed-ps.dxil'])
exe=out/'typed-input-gpu.exe'
run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),'-I'+str(root/'thirdparty/plume'),
 root/'tools/gpu-aot/test_float_blend_indices_gpu.cpp','-o',exe,'-ld3d12','-ldxgi'])
result=run([exe,out/'typed-vs-float.dxil',out/'typed-vs-uint.dxil',out/'typed-ps.dxil'])
(out/'typed-input-verification.json').write_text(json.dumps(dict(passed=True,
 checks=['UNORM float selection','UINT uint selection','wrong UNORM signature rejected',
 'wrong UINT signature rejected'],output=result.stdout),indent=2)+'\n')
print(result.stdout,end='')
