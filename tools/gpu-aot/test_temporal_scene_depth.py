"""Exercise production depth snapshot allocation/publication with fence mocks."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
text = (root/'rexlego/src/gpu_native/temporal_scene_depth.cpp').read_text()
body = text[text.index('TemporalSceneDepth CaptureTemporalSceneDepth('):text.rindex('}  // namespace')]
code = r'''
#include <cassert>
#include <memory>
#include <vector>
#include "gpu_native/temporal_scene_depth.h"
using namespace legodimensions::gpu_native;
namespace plume {
enum class RenderTextureDimension {TEXTURE_2D};
enum class RenderSampleCount {COUNT_1};
enum class RenderFormat {R32_FLOAT};
enum class RenderTextureFlag {RENDER_TARGET=1};
struct Flags {int value=0;void operator|=(RenderTextureFlag flag){value|=int(flag);}};
struct RenderTextureDesc {
 RenderTextureDimension dimension;uint32_t width=0,height=0,depth=0,mipLevels=0,arraySize=0;
 struct {RenderSampleCount sampleCount;}multisampling;
 RenderFormat format;Flags flags;bool committed=false;
};
struct RenderTexture {RenderTextureDesc desc;void setName(const char*){}};
}
bool fail_allocation=false,fail_copy=false;
uint32_t allocations=0;
struct Device {
 std::unique_ptr<plume::RenderTexture>createTexture(plume::RenderTextureDesc desc){
  if(fail_allocation)return {};
  ++allocations;
  auto texture=std::make_unique<plume::RenderTexture>();texture->desc=desc;return texture;
 }
}device;
bool device_ready=true;
struct View {plume::RenderTexture*texture=nullptr;bool depth=false;uint32_t width=0,height=0,descriptor_index=~0u;}source;
auto ResolveTextureResource(uint32_t){return source;}
enum class ColorResolveDestination{kDepthFloat32};
std::vector<std::shared_ptr<plume::RenderTexture>>retained;
uint32_t copies=0;
struct HostDevice {
 static int LockRecording(){return 0;}
 static auto Device(){return device_ready?&device:nullptr;}
 static bool ResolveHdrColor(plume::RenderTexture*from,uint32_t descriptor,
     plume::RenderTexture*to,uint32_t width,uint32_t height,float scale,
     ColorResolveDestination format,bool is_target){
  ++copies;assert(from==source.texture && to!=from && descriptor==99);
  assert(width==1920 && height==1080 && scale==1 && is_target);
  assert(format==ColorResolveDestination::kDepthFloat32);
  return !fail_copy;
 }
 static void RetireResource(std::shared_ptr<plume::RenderTexture>texture){retained.push_back(std::move(texture));}
};
namespace legodimensions::gpu_native {
''' + body + r'''
}
int main(){
 assert(!CaptureTemporalSceneDepth(1,1920,1080,7).texture && copies==0);
 plume::RenderTexture depth;source={&depth,true,1920,1080,99};
 auto snapshot=CaptureTemporalSceneDepth(1,1920,1080,7);
 assert(snapshot.texture && snapshot.texture.get()!=&depth && snapshot.frame==7);
 const auto&desc=snapshot.texture->desc;
 assert(desc.format==plume::RenderFormat::R32_FLOAT && desc.width==1920 && desc.height==1080);
 assert(desc.depth==1 && desc.mipLevels==1 && desc.arraySize==1 && desc.committed && desc.flags.value==1);
 assert(snapshot.width==1920 && snapshot.height==1080 && copies==1);
 source.depth=false;assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture && copies==1);
 source.depth=true;source.width=960;assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture);
 source.width=1920;source.descriptor_index=~0u;assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture);
 source.descriptor_index=99;device_ready=false;assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture);
 device_ready=true;fail_allocation=true;assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture && copies==1);
 fail_allocation=false;fail_copy=true;
 assert(!CaptureTemporalSceneDepth(1,1920,1080,8).texture && retained.size()==1 && copies==2);
 fail_copy=false;auto next=CaptureTemporalSceneDepth(1,1920,1080,8);
 assert(next.texture && next.frame==8 && next.texture!=snapshot.texture);
 const auto allocated=allocations;
 auto reused=CaptureTemporalSceneDepth(1,1920,1080,9,next);
 assert(reused.texture==next.texture && reused.frame==9 && allocations==allocated);
 fail_copy=true;
 assert(!CaptureTemporalSceneDepth(1,1920,1080,10,reused).texture);
 assert(reused.frame==9); // Failed copy cannot publish a new generation.
 fail_copy=false;
 retained.clear();
 HostDevice::RetireResource(snapshot.texture);snapshot={};
 assert(retained.size()==1); // Original snapshot survives until simulated completion.
 retained.clear();
}
'''
a.output.mkdir(parents=True, exist_ok=True)
cpp, exe = a.output.resolve()/'snapshot.cpp', a.output.resolve()/'snapshot.exe'
cpp.write_text(code)
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', '-I', str(root/'rexlego/src'),
    str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
print('Production R32 depth snapshot publication and fence-retirement boundary passed.')
