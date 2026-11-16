"""Execute actual HostDevice bridge with real Plume before/after rendering."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--production-shader-dir',type=Path,required=True)
p.add_argument('--plume-lib',type=Path,required=True);p.add_argument('--dxc',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
text=(root/'rexlego/src/gpu_native/device.cpp').read_text();start=text.index('bool HostDevice::ResampleDepthSurface(')
end=text.index('{',start)+1;level=1
while level:level+=(text[end]=='{')-(text[end]=='}');end+=1
production=text[start:end]
def compile_shader(text,name,stage):
    source=out/(name+'.hlsl');source.write_text(text);target=out/(name+'.dxil')
    subprocess.run([str(a.dxc),str(source),'-T',stage+'_6_0','-E','main','-Fo',str(target)],check=True,capture_output=True)
    return target
vs=compile_shader('void main(uint id:SV_VertexID,out float4 p:SV_Position){p=float4(id?.8:-.8,0,.5,1);}','ordinary-vs','vs')
ps=compile_shader('float4 main():SV_Target{return 1;}','ordinary-ps','ps')
include=out/'production-depth-bridge.inc'
def build(name,body):
    include.write_text(body);exe=out/(name+'.exe')
    args=['clang-cl',str(Path(__file__).with_suffix('.cpp')),str(root/'rexlego/src/gpu_native/depth_resample.cpp'),
      '/std:c++20','/O2','/MD','/EHsc','/DNOMINMAX','/UNDEBUG','/I'+str(out),
      '/I'+str(root/'rexlego/src'),'/I'+str(root/'thirdparty/plume'),
      '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
      '/I'+str(a.production_shader_dir.resolve().parents[1]),'/Fo'+str(out)+'/', '/Fe'+str(exe),
      '/link',str(a.plume_lib.resolve()),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
    result=subprocess.run(args,capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode();return exe
def run(exe,cache,name):
    result=subprocess.run([str(exe),str(vs),str(ps)],env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=60)
    (out/(name+'.log')).write_text(result.stdout+result.stderr);return result
exe=build('bridge',production);positive=[]
for cache in ('0','1'):
    result=run(exe,cache,'cache-'+cache);print(result.stdout);result.check_returncode()
    assert 'PASS Plume graphics resume' in result.stdout
    positive.append(dict(state_cache=cache,exit=result.returncode,stdout=result.stdout))
negative=[]
for name,line in [('omit-topology','native->activeTopology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;'),
                  ('omit-stencil','native->activeStencilRef=~u32{0};')]:
    assert line in production
    bad=build(name,production.replace(line,''));result=run(bad,'1',name)
    if result.returncode!=23:raise RuntimeError(name+' escaped state-restoration oracle: '+result.stdout+result.stderr)
    negative.append(dict(name=name,exit=result.returncode,stdout=result.stdout))
include.write_text(production)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_production_bridge=True,real_plume=True,positive=positive,negative=negative,
    scope='normal line draw with stencil equality after resize, state cache off/on; not full frame or game integration'),indent=2))
print('PASS production resize bridge, two real Plume modes, broken IA/stencil restoration rejected')
