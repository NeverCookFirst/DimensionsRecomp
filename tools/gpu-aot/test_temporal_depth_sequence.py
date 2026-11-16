"""Replay copy/clear/resolve ordering through the production snapshot helper."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--verify-regression', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
draw = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
body = draw[draw.index('void SnapshotTemporalSceneDepth('):draw.index('bool TemporalInputsCandidate(')]
mark = draw[draw.index('  if (TemporalInputsCandidate() && HasVerifiedTemporalCamera(key.vertex_shader_hash) &&', draw.index('mesh_result.submitted = true;')):draw.index('  if (cpu_trace) cpu_trace->submitted = true;', draw.index('mesh_result.submitted = true;'))]
prefix = r'''
#include <atomic>
#include <cassert>
#include <memory>
#include "gpu_native/temporal_scene_depth.h"
#include "gpu_native/depth_state.h"
using namespace legodimensions::gpu_native;
using u32 = uint32_t;
namespace plume {struct RenderTexture {int value=0;};}
TemporalSceneDepth g_temporal_scene_depth;
u32 g_temporal_scene_depth_surface=12;
bool g_temporal_scene_depth_dirty=true;
std::atomic<uint64_t> g_probe_frame{7};
struct Extent {u32 logical_width=1280,logical_height=720,width=1280,height=720;};
Extent NativeTextureExtent(u32){return {};}
struct HostDevice {static void RetireResource(std::shared_ptr<plume::RenderTexture>) {}};
int surface=42,copies=0;
bool fail=false;
bool TemporalInputsCandidate(){return true;}
bool TemporalSceneComposed(uint64_t){return false;}
bool HasVerifiedTemporalCamera(uint64_t){return true;}
struct Key {uint64_t vertex_shader_hash=1;uint32_t depth_control=0x32;};
struct Attachment {bool texture=true;};
struct Bindings {u32 depth_stencil=12;};
struct Device {struct {float x=0,y=0;} viewport;};
namespace legodimensions::gpu_native {
TemporalSceneDepth CaptureTemporalSceneDepth(u32,u32 w,u32 h,uint64_t f,const TemporalSceneDepth& reuse) {
 ++copies;if(fail)return {};
 auto texture=reuse.texture?reuse.texture:std::make_shared<plume::RenderTexture>();
 texture->value=surface;return {texture,w,h,f};
}}
'''
suffix = r'''
int main(){
 SnapshotTemporalSceneDepth(13);assert(copies==0);
 SnapshotTemporalSceneDepth(12);assert(copies==1 && g_temporal_scene_depth.texture->value==42);
 // A depth-tested draw without depth writes after Clear cannot dirty the snapshot.
 surface=0;MarkDraw(0x32);SnapshotTemporalSceneDepth(12);
 if(copies!=1 || g_temporal_scene_depth.texture->value!=42)return 23;
 // New scene geometry is allowed to replace depth in the same allocation.
 auto* allocation=g_temporal_scene_depth.texture.get();
 surface=84;MarkDraw(0x36);SnapshotTemporalSceneDepth(12);
 assert(copies==2 && g_temporal_scene_depth.texture.get()==allocation && allocation->value==84);
 g_temporal_scene_depth_dirty=true;fail=true;SnapshotTemporalSceneDepth(12);
 assert(g_temporal_scene_depth.frame==~uint64_t{0});
 surface=0;SnapshotTemporalSceneDepth(12);assert(copies==3);
}
'''
a.output.mkdir(parents=True, exist_ok=True)
for name in (['sequence','mutant','read-only-mutant'] if a.verify_regression else ['sequence']):
    source = body.replace(' ||\n      !g_temporal_scene_depth_dirty', '') if name=='mutant' else body
    marker = mark.replace(' &&\n      DecodeDepthState(key.depth_control,true).write','') if name=='read-only-mutant' else mark
    marker = 'void MarkDraw(uint32_t control){Key key;key.depth_control=control;Attachment color,depth;Extent raster_extent;Device storage;auto* device=&storage;Bindings bindings;u32 viewport_width=1280,viewport_height=720;bool screen_space=false;\n'+marker+'}\n'
    cpp, exe = a.output.resolve()/(name+'.cpp'), a.output.resolve()/(name+'.exe')
    cpp.write_text(prefix+source+marker+suffix)
    subprocess.run([a.compiler,'-std=c++20','-UNDEBUG','-I',str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
    result = subprocess.run([str(exe)])
    assert result.returncode == (0 if name=='sequence' else 23), (name,result.returncode)
print('PASS: read-only depth draws, clear/resolve preservation, reuse and failed-copy invalidation; no game launched')
