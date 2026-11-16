"""Execute both actual queued draw bodies against delayed tasks and stale global shaders."""
from pathlib import Path
import argparse
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
text = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
def body(signature):
    start = text.index(signature); end = text.index('{', start) + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]
production = '\n'.join(body(name) for name in ('u32 DrawVerticesHookQueued(', 'u32 DrawIndexedVerticesHookQueued('))
prefix = r'''
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <functional>
#include <vector>
using u32=uint32_t;using i32=int32_t;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct D3DDevice{u32 vertex_shader,pixel_shader;};
struct DrawSnapshot{std::shared_ptr<D3DDevice> device;};
bool temporal=false;u32 full=0,draws=0,flushes=0;
std::vector<std::function<void()>> pending;
bool ConsumeCpuPoolCopyDraw(u32,u32,u32){return false;}
bool NativeWorkerShouldQueue(){return true;}
bool NativeFullDrawPreparationEnabled(){return true;}
bool NativeFsr3Enabled(){return temporal;}
bool NativeParallelDrawsEnabled(){return false;}
void NativeWorkerSubmitCapture(std::function<void()> run){pending.push_back(std::move(run));}
DrawSnapshot SnapshotDrawDevice(D3DDevice* d){return {std::make_shared<D3DDevice>(*d)};}
std::shared_ptr<D3DDevice> SnapshotFullDevice(DrawSnapshot& s){return s.device;}
void CaptureFullDraw(DrawSnapshot,u32,bool,u32,u32,i32){++full;}
bool CaptureParallelDraw(std::shared_ptr<D3DDevice>,u32,bool,u32,u32,i32){std::exit(24);}
void FlushNativeDrawBatch(){++flushes;}
struct ShaderPair{u32 vs,ps;};
ShaderPair bound{0xdead,0xbeef};
ShaderPair CaptureShaderBindings(u32 vs,u32 ps){return {vs,ps};}
struct ShaderBindingsScope{ShaderPair old;ShaderBindingsScope(ShaderPair p):old(bound){bound=p;}~ShaderBindingsScope(){bound=old;}};
u32 DrawVerticesHook(D3DDevice* d,u32 primitive,u32 start,u32 count){
 CHECK(temporal && d->vertex_shader==101 && d->pixel_shader==202);
 CHECK(bound.vs==101 && bound.ps==202 && primitive==4 && start==7 && count==12);++draws;return 0;
}
u32 DrawIndexedVerticesHook(D3DDevice* d,u32 primitive,u32 base,u32 start,u32 count){
 CHECK(temporal && d->vertex_shader==303 && d->pixel_shader==404);
 CHECK(bound.vs==303 && bound.ps==404 && primitive==5 && base==u32(-3) && start==8 && count==18);++draws;return 0;
}
'''
suffix = r'''
int main(){
 for(bool enabled:{false,true}){
  D3DDevice a{101,202},b{303,404};
  DrawVerticesHookQueued(&a,4,7,12);DrawIndexedVerticesHookQueued(&b,5,u32(-3),8,18);
  a={999,999};b={888,888};temporal=enabled;
  for(auto& run:pending)run();pending.clear();
 }
 CHECK(full==2 && draws==2 && flushes==0 && bound.vs==0xdead && bound.ps==0xbeef);
 puts("PASS actual queued draw routes: parallel capture off, ordered temporal draws on, captured VS/PS and signed base retained");
}
'''
for name, selected in [('current', production), ('stale-global-shaders', production.replace('ShaderBindingsScope shaders(programs);', ''))]:
    cpp = out / (name + '.cpp'); exe = cpp.with_suffix('.exe')
    cpp.write_text(prefix + selected + suffix)
    result = subprocess.run(['clang++', '-std=c++20', '-O2', str(cpp), '-o', str(exe)], capture_output=True, text=True)
    (out / (name + '-compile.log')).write_text(result.stdout + result.stderr); result.check_returncode()
    result = subprocess.run([exe], capture_output=True, text=True, timeout=10)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == (0 if name == 'current' else 23), (name, result.returncode, result.stdout, result.stderr)
    print(result.stdout if name == 'current' else 'PASS stale global shader negative control', end='\n')
