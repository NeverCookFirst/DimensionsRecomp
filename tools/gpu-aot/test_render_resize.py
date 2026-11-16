"""Exercise production host resize publication and fence-retained old storage."""
import argparse
from pathlib import Path
import subprocess

def body(text, signature):
    start=text.index(signature); end=text.index('{',start)+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
text=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
source=r'''
#include <cassert>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <utility>
#include <cstdint>
#include "gpu_native/render_extent.h"
#include "gpu_native/texture_writeback_coverage.h"
using u32=uint32_t;
using u64=uint64_t;
using namespace legodimensions::gpu_native;
int failure=0,registrations=0,invalidations=0,copies=0;u32 next_descriptor=100;
namespace plume {
enum class RenderFormat {D32_FLOAT_S8_UINT=7,R8G8B8A8_UNORM=6};
struct RenderTextureViewDesc {u32 mapping=0,mips=1;};
struct RenderTextureView {virtual ~RenderTextureView()=default;};
struct D3D12TextureView:RenderTextureView {RenderTextureViewDesc desc;D3D12TextureView(RenderTextureViewDesc d):desc(d){}};
struct RenderTexture {
 u32 width=0,height=0,levels=0;
 std::unique_ptr<RenderTextureView> createTextureView(RenderTextureViewDesc d){
  if(failure==2 || (failure==3 && d.mapping==42))return{};
  return std::make_unique<D3D12TextureView>(d);
 }
};
}
#define REXLOG_INFO(...) ((void)0)
''' + body(text,'struct SurfaceStorage {') + r''';
struct TextureResource {
 TextureWritebackCoverage writeback_coverage;
 TextureResource(){writeback_coverage.Record(1280,720,0,0,640,360);}
 std::atomic<u64> promoted_viewport{0};
 u32 guest_address=12,width=1280,height=720,host_width=0,host_height=0,logical_width=0,logical_height=0;
 u32 levels=1,d3d_type=3,descriptor_index=1,resolved_descriptor_index=2,sampled_descriptor_index=3;
 plume::RenderFormat format=plume::RenderFormat::D32_FLOAT_S8_UINT;
 bool surface=false,sampled_valid=true,guest_uploaded=true,resolved_on_host=true,guest_backing_current=true;
 u32 depth_alias_generation=4,depth_resolve_generation=5;
 std::unique_ptr<plume::RenderTexture> texture=std::make_unique<plume::RenderTexture>();
 std::unique_ptr<plume::RenderTextureView> view=std::make_unique<plume::D3D12TextureView>(plume::RenderTextureViewDesc{17,1});
 std::unique_ptr<plume::RenderTextureView> resolved_view=std::make_unique<plume::D3D12TextureView>(plume::RenderTextureViewDesc{42,1});
 std::unique_ptr<plume::RenderTexture> sampled_texture=std::make_unique<plume::RenderTexture>();
 std::unique_ptr<plume::RenderTextureView> sampled_view=std::make_unique<plume::D3D12TextureView>(plume::RenderTextureViewDesc{9,1});
 std::vector<u32> previous_surface_descriptors;
 u32 HostWidth()const{return host_width?host_width:width;}u32 HostHeight()const{return host_height?host_height:height;}
};
std::vector<std::shared_ptr<SurfaceStorage>> retained;
std::vector<u32> unregistered;
struct HostDevice {
 static u32 RegisterTexture(plume::RenderTexture*,plume::RenderTextureView*) {
  ++registrations;if(failure==4 || (failure==5 && registrations==2))return ~0u;
  return next_descriptor++;
 }
 static void UnregisterTexture(u32 d){unregistered.push_back(d);}
 static void RetireResource(std::shared_ptr<SurfaceStorage> s){retained.push_back(s);}
 static bool ResampleDepthSurface(plume::RenderTexture* s,plume::RenderTexture* d){
  ++copies;assert(s && d && s!=d);return failure!=6;
 }
};
void InvalidateFramebufferReferences(u32){++invalidations;}
std::unique_ptr<plume::RenderTexture> CreateHostTexture(const TextureResource& r,u32,u32){
 if(failure==1)return{};
 auto t=std::make_unique<plume::RenderTexture>();t->width=r.HostWidth();t->height=r.HostHeight();t->levels=r.levels;return t;
}
''' + body(text,'void PublishPromotedViewport(') + body(text,'struct PreparedNativeStorage {') + ';\n' + \
    body(text,'bool PrepareNativeStorage(') + body(text,'void CommitNativeStorage(') + body(text,'bool ResizeNativeStorage(') + r'''
int main(){
 for(failure=1;failure<=5;++failure){
  TextureResource r;auto* before=r.texture.get();registrations=invalidations=0;
  assert(!ResizeNativeStorage(r,NativeSceneExtent(1280,720,true)));
  assert(r.texture.get()==before && r.HostWidth()==1280 && r.sampled_valid && r.resolved_on_host);
  assert(r.guest_backing_current);
  assert(r.writeback_coverage.Matches(1280,720) && r.writeback_coverage.Contains(639,359));
  assert(retained.empty() && invalidations==0 && r.previous_surface_descriptors.empty());
 }
 assert(unregistered.size()==1);failure=0;registrations=0;
 TextureResource r;auto* old=r.texture.get();auto* sampled=r.sampled_texture.get();
 assert(ResizeNativeStorage(r,NativeSceneExtent(1280,720,true)));
 assert(r.texture->width==1920 && r.texture->height==1080 && r.logical_width==1280 && r.logical_height==720);
 assert(static_cast<plume::D3D12TextureView*>(r.view.get())->desc.mapping==17);
 assert(static_cast<plume::D3D12TextureView*>(r.resolved_view.get())->desc.mapping==42);
 assert(!r.sampled_texture && !r.sampled_valid && !r.resolved_on_host && !r.guest_uploaded);
 assert(!r.guest_backing_current);
 assert(!r.writeback_coverage.Matches(1280,720) && !r.writeback_coverage.Contains(0,0));
 assert(r.depth_resolve_generation==0 && r.depth_alias_generation==0);
 assert((r.previous_surface_descriptors==std::vector<u32>{1,2,3}));
 assert(retained.size()==1 && retained[0]->texture.get()==old && retained[0]->sampled_texture.get()==sampled);
 const auto calls=registrations;assert(ResizeNativeStorage(r,NativeSceneExtent(1280,720,true)));
 assert(registrations==calls && retained.size()==1);
 // A borrowed raw depth alias can return to CPU authority with guest-sized storage.
 assert(ResizeNativeStorage(r,NativeSceneExtent(1280,720,false)));
 assert(r.HostWidth()==1280 && r.HostHeight()==720 && retained.size()==2);
 retained.clear();
 // A failed depth copy keeps all previously published storage/authority intact.
 TextureResource depth;depth.surface=true;auto* before=depth.texture.get();
 registrations=invalidations=copies=0;unregistered.clear();failure=6;
 assert(!ResizeNativeStorage(depth,NativeSceneExtent(1280,720,true),true));
 assert(copies==1 && depth.texture.get()==before && depth.HostWidth()==1280);
 assert(depth.descriptor_index==1 && depth.resolved_descriptor_index==2 && depth.sampled_valid);
 assert(depth.resolved_on_host && depth.guest_backing_current && depth.previous_surface_descriptors.empty());
 assert(invalidations==0 && unregistered.size()==2);
 assert(retained.size()==1 && retained[0]->texture.get()!=before && retained[0]->texture->width==1920);
 retained.clear();failure=0;registrations=0;
 assert(ResizeNativeStorage(depth,NativeSceneExtent(1280,720,true),true));
 assert(copies==2 && invalidations==1 && depth.texture.get()!=before);
 assert(depth.HostWidth()==1920 && retained.size()==1 && retained[0]->texture.get()==before);
 const auto copies_before=copies;
 assert(ResizeNativeStorage(depth,NativeSceneExtent(1280,720,true),true));
 assert(copies==copies_before); // No duplicate resize/copy for an unchanged extent.
 TextureResource color;color.surface=true;color.format=plume::RenderFormat::R8G8B8A8_UNORM;
 assert(!ResizeNativeStorage(color,NativeSceneExtent(1280,720,true),true));
 assert(copies==copies_before); // Unsupported formats reject without a copy.
 TextureResource promoted;promoted.surface=true;promoted.height=512;
 assert(ResizeNativeStorage(promoted,{1280,720,1920,1080}));
 const u64 first=(u64(1280)<<32)|720,second=(u64(640)<<32)|360;
 assert(promoted.promoted_viewport.load()==first);
 failure=1;
 assert(!ResizeNativeStorage(promoted,{640,360,640,360}));
 assert(promoted.promoted_viewport.load()==first);
 failure=0;
 assert(ResizeNativeStorage(promoted,{640,360,1920,1080}));
 assert(promoted.promoted_viewport.load()==second);
 std::atomic<bool> done=false;
 std::thread publisher([&]{
  for(unsigned i=0;i<100000;++i){
   promoted.logical_width=i%2?1280:640;promoted.logical_height=i%2?720:360;
   PublishPromotedViewport(promoted);
  }
  done.store(true);
 });
 do {const auto pair=promoted.promoted_viewport.load(std::memory_order_acquire);
  assert(pair==first || pair==second);
 } while(!done.load());
 publisher.join();
 assert(ResizeNativeStorage(promoted,{1280,512,1920,1080}));
 assert(promoted.promoted_viewport.load()==0);
}
'''
# This fixture selects explicit physical dimensions. Production now obtains
# scene scale from HostDevice rather than the removed NativeSceneExtent helper.
source=source.replace('NativeSceneExtent(1280,720,true)', 'NativeRenderExtent{1280,720,1920,1080}')
source=source.replace('NativeSceneExtent(1280,720,false)', 'NativeRenderExtent{1280,720,1280,720}')
fixture=a.output/'test.cpp';binary=a.output/'test.exe';fixture.write_text(source)
subprocess.run([a.compiler,'-std=c++20','-UNDEBUG','-I',str(root/'rexlego/src'),str(fixture),'-o',str(binary)],check=True,timeout=45)
subprocess.run([str(binary)],check=True,timeout=10)
