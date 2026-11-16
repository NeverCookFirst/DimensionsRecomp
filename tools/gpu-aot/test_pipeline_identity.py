"""Exercise production PSO identity across guest relocation and render-state changes."""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
source=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
types=source[source.index('struct PipelineKey {'):source.index('bool RetainedDrawPreparationEnabled()')]
fixture=a.output/'pipeline-identity.cpp'
fixture.write_text('''#include <array>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include "gpu_native/draw_preparation.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
constexpr size_t kNativeRenderTargets=4,kNativeVertexStreams=16;
'''+types+'''
int main(){
 PipelineKey base;base.vertex_shader=0x1000;base.pixel_shader=0x2000;base.declaration=0x3000;
 base.vertex_shader_hash=11;base.pixel_shader_hash=22;base.declaration_hash=33;
 std::unordered_map<PipelineKey,u32,PipelineKeyHash> cache;cache.emplace(base,1);
 // Scene instances and placement aliases can own thousands of distinct guest
 // resources while using precisely the same compiled shader and input layout.
 for(u32 n=1;n<4096;++n){auto moved=base;moved.vertex_shader+=n*64;
  moved.pixel_shader+=n*80;moved.declaration+=n*32;
  assert(moved==base);assert(PipelineKeyHash{}(moved)==PipelineKeyHash{}(base));
  assert(!cache.emplace(moved,2).second);assert(cache.at(moved)==1);
 }
 assert(cache.size()==1);
 PreparedValue<PipelineKey,int> retained;
 int lookups=0;
 auto prepare=[&]{return ++lookups;};
 assert(retained.Resolve(base,prepare)==1);
 auto alias=base;alias.vertex_shader+=64;alias.strides[0]=32;
 assert(retained.Resolve(alias,prepare)==1);
 auto changed=[&](PipelineKey value){assert(!(value==base));assert(cache.find(value)==cache.end());
  retained.Resolve(base,prepare);const int before=lookups;
  retained.Resolve(value,prepare);assert(lookups==before+1);};
'''+''.join(' {auto value=base;++value.'+field+';changed(value);}\n' for field in [
 'vertex_shader_hash','pixel_shader_hash','declaration_hash','depth_format','depth_control',
 'clip_disable','raster_control','stencil_masks','stencil_face','color_write_mask','topology',
 'spec_constants','depth_bias','slope_scaled_depth_bias'])+'''
 for(size_t i=0;i<kNativeRenderTargets;++i){auto value=base;++value.render_target_formats[i];changed(value);
  value=base;++value.blend_controls[i];changed(value);}
 // A D3D12 buffer-view stride is dynamic IA state, not PSO identity. Every
 // stream must reuse the pipeline while the explicit VB binding changes.
 for(size_t i=0;i<kNativeVertexStreams;++i){auto value=base;value.strides[i]=4+4*u32(i);
  assert(value==base);assert(PipelineKeyHash{}(value)==PipelineKeyHash{}(base));
  assert(!cache.emplace(value,2).second);}
 // Reusing an allocation with a new program must miss even at the same address.
 auto reused=base;reused.vertex_shader_hash=77;assert(cache.emplace(reused,3).second);
 auto noPixel=base;noPixel.pixel_shader=0;noPixel.pixel_shader_hash=0;
 assert(cache.emplace(noPixel,4).second);assert(cache.size()==3);
 auto missing=base;missing.pixel_shader=0;changed(missing);
 missing=base;missing.vertex_shader=0;changed(missing);
}
''')
exe=a.output/'pipeline-identity.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(fixture),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
print('PASS production pipeline identity: guest aliases and 16 dynamic strides reuse one PSO; actual PSO state mutations miss')
