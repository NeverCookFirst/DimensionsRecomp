// Production storage transaction and copy recorders, with real Plume resources.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <plume_d3d12.h>
#include "gpu_native/device.h"
#include "gpu_native/native_recording.h"
#include "gpu_native/depth_resample.h"
#include "gpu_native/scaled_resolve.h"
#include "gpu_native/textures.h"
#include "gpu_native/shaders/copy_vs.h"
#include "gpu_native/shaders/resolve_color_ps.h"
#include "gpu_native/shaders/resolve_scaled_ps.h"
#include "gpu_native/shaders/depth_resample_ps.h"
#include "gpu_native/shaders/depth_resample_depth_ps.h"
#include "gpu_native/shaders/depth_resample_stencil_ps.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <atomic>
#include <utility>
#include "gpu_native/texture_writeback_coverage.h"
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define CHECK(x) do{if(!(x)){printf("FAIL line=%d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
namespace legodimensions::gpu_native {
#include "composition-storage-struct.inc"
struct TextureResource {
  TextureWritebackCoverage writeback_coverage;
  std::atomic<u64> promoted_viewport{0};
  u32 guest_address=0,width=1280,height=720,logical_width=1280,logical_height=720;
  u32 host_width=0,host_height=0,levels=1,d3d_type=1;
  plume::RenderFormat format=plume::RenderFormat::UNKNOWN;
  bool surface=true,composition_active=false,sampled_valid=false,guest_uploaded=false;
  bool resolved_on_host=false,guest_backing_current=false,authority_probe_signature_valid=false;
  u64 depth_alias_generation=0,depth_resolve_generation=0;
  u32 descriptor_index=~0u,resolved_descriptor_index=~0u,sampled_descriptor_index=~0u;
  std::unique_ptr<plume::RenderTexture> texture,sampled_texture;
  std::unique_ptr<plume::RenderTextureView> view,resolved_view,sampled_view;
  std::shared_ptr<SurfaceStorage> composition_storage;
  std::vector<u32> previous_surface_descriptors;
  u32 LogicalWidth()const{return logical_width;}u32 LogicalHeight()const{return logical_height;}
  u32 HostWidth()const{return host_width?host_width:width;}u32 HostHeight()const{return host_height?host_height:height;}
};
struct State {
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptors,sampler_descriptors;
  std::unique_ptr<plume::RenderShader> copy_vertex_shader,resolve_pixel_shader,scaled_resolve_pixel_shader;
  std::array<std::unique_ptr<plume::RenderPipeline>,5> resolve_pipelines;
  std::unique_ptr<DepthResampler> depth_resampler;
  std::vector<bool> texture_slots=std::vector<bool>(128,false);
};
State* g_state=nullptr;std::mutex g_mutex,g_texture_slots_mutex;NativeRecordingGate recording_mutex;
plume::RenderCommandList* recording_commands=nullptr;
std::vector<std::shared_ptr<void>> retired;
std::map<u32,std::shared_ptr<TextureResource>> registry;
auto& g_textures=registry;
std::mutex g_textures_mutex;
std::vector<std::shared_ptr<TextureResource>> g_composition_surfaces;
bool g_composition_restore_pending=false;
u32 allocations=0,registrations=0,invalidations=0,next_descriptor=1;
auto FindTexture(u32 address){auto it=registry.find(address);return it==registry.end()?std::shared_ptr<TextureResource>{}:it->second;}
void InvalidateFramebufferReferences(u32){++invalidations;}
std::unique_ptr<plume::RenderTexture> CreateHostTexture(const TextureResource& resource,u32,u32){
  plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
  d.width=resource.HostWidth();d.height=resource.HostHeight();d.depth=d.arraySize=d.mipLevels=1;
  d.format=resource.format;d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
  d.flags=d.format==plume::RenderFormat::D32_FLOAT_S8_UINT?plume::RenderTextureFlag::DEPTH_TARGET:plume::RenderTextureFlag::RENDER_TARGET;
  d.committed=true;++allocations;return g_state->device->createTexture(d);
}
std::unique_lock<NativeRecordingGate> HostDevice::LockRecording(){return std::unique_lock(recording_mutex);}
plume::RenderCommandList* HostDevice::BeginFrameCommands(){return recording_commands;}
plume::RenderDevice* HostDevice::Device(){return g_state->device.get();}
void HostDevice::RetireResource(std::shared_ptr<void> p){if(p)retired.push_back(std::move(p));}
u32 HostDevice::RegisterTexture(plume::RenderTexture* t,plume::RenderTextureView* v){
  CHECK(next_descriptor<128);const u32 slot=next_descriptor++;++registrations;g_state->texture_slots[slot]=true;
  g_state->texture_descriptors->setTexture(slot,t,plume::RenderTextureLayout::SHADER_READ,v);return slot;
}
void HostDevice::UnregisterTexture(u32 index){if(index<128)g_state->texture_slots[index]=false;}
struct Upload {std::unique_ptr<plume::RenderBuffer> buffer;void* mapped=nullptr;~Upload(){if(mapped)buffer->unmap();}};
DrawUploadSlice HostDevice::AllocateDrawUpload(u32 size){
  auto upload=std::make_shared<Upload>();upload->buffer=g_state->device->createBuffer(plume::RenderBufferDesc::UploadBuffer((size+255)&~255u));
  CHECK(upload->buffer);upload->mapped=upload->buffer->map();CHECK(upload->mapped);
  DrawUploadSlice result{upload->buffer.get(),upload->mapped,0};retired.push_back(std::move(upload));return result;
}
#include "production-composition-recorders.inc"
}
using namespace legodimensions::gpu_native;
static std::vector<unsigned char> read(const char* path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);CHECK(f);std::vector<unsigned char>b(size_t(f.tellg()));
  f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),b.size());return b;
}
struct Pixels {u32 width=0,height=0;std::vector<unsigned char> color,stencil;std::vector<float> depth;};
struct Download {std::unique_ptr<plume::RenderBuffer> buffer;std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> fp{};bool depth=false;u32 width=0,height=0;};
int main(int argc,char**argv){
  CHECK(argc==3);setvbuf(stdout,nullptr,_IONBF,0);
  const auto captured_color=read(argv[1]),captured_depth=read(argv[2]);
  CHECK(captured_color.size()==1129u*635u*4 && captured_depth.size()==1129u*635u*8);
  auto render=plume::CreateD3D12Interface();State state;state.device=render->createDevice();CHECK(state.device);g_state=&state;
  auto* device=state.device.get();auto* raw_device=static_cast<plume::D3D12Device*>(device)->d3d;
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();recording_commands=commands.get();
  auto* list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
  plume::RenderDescriptorSetBuilder textures;textures.begin();textures.addTexture(0,128);textures.end(true,128);
  state.texture_descriptors=textures.create(device);
  plume::RenderDescriptorSetBuilder samplers;samplers.begin();samplers.addSampler(0,1);samplers.end();
  state.sampler_descriptors=samplers.create(device);
  plume::RenderPipelineLayoutBuilder layout;layout.begin(false,true);
  for(int i=0;i<3;++i)layout.addDescriptorSet(textures);layout.addDescriptorSet(samplers);
  for(u32 reg:{0u,1u,2u,4u})layout.addRootDescriptor(reg,4,plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout.addPushConstant(3,4,16,plume::RenderShaderStageFlag::PIXEL);layout.end();state.pipeline_layout=layout.create(device);
  state.copy_vertex_shader=device->createShader(g_copy_vs_dxil,sizeof(g_copy_vs_dxil),"main",plume::RenderShaderFormat::DXIL);
  const auto surface=[&](u32 id,plume::RenderFormat format,u32 w,u32 h){
    auto r=std::make_shared<TextureResource>();r->guest_address=id;r->format=format;r->host_width=w;r->host_height=h;
    r->texture=CreateHostTexture(*r,1,0);CHECK(r->texture);
    r->view=r->texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));CHECK(r->view);
    r->descriptor_index=HostDevice::RegisterTexture(r->texture.get(),r->view.get());registry[id]=r;return r;
  };
  auto color=surface(10,plume::RenderFormat::R8G8B8A8_UNORM,1129,635);
  auto depth=surface(20,plume::RenderFormat::D32_FLOAT_S8_UINT,1129,635);
  const auto upload=[&](TextureResource& resource,const Pixels& p,bool is_depth){
    auto* raw=static_cast<plume::D3D12Texture*>(resource.texture.get())->d3d;auto d=raw->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> fp{};UINT64 bytes;const u32 planes=is_depth?2:1;
    raw_device->GetCopyableFootprints(&d,0,planes,0,fp.data(),nullptr,nullptr,&bytes);
    std::shared_ptr<plume::RenderBuffer> buffer=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
    auto* mapped=static_cast<unsigned char*>(buffer->map());CHECK(mapped);std::memset(mapped,0,bytes);
    for(u32 y=0;y<p.height;++y){
      const void* row=is_depth?static_cast<const void*>(p.depth.data()+y*p.width):p.color.data()+y*p.width*4;
      std::memcpy(mapped+fp[0].Offset+y*fp[0].Footprint.RowPitch,row,p.width*4);
      if(is_depth)std::memcpy(mapped+fp[1].Offset+y*fp[1].Footprint.RowPitch,p.stencil.data()+y*p.width,p.width);
    }buffer->unmap();commands->setFramebuffer(nullptr);
    commands->barriers(plume::RenderBarrierStage::COPY,{resource.texture.get(),plume::RenderTextureLayout::COPY_DEST});
    for(u32 plane=0;plane<planes;++plane){
      D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=static_cast<plume::D3D12Buffer*>(buffer.get())->d3d;
      src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp[plane];
      dst.pResource=raw;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=plane;
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }retired.push_back(std::move(buffer));
  };
  const auto download=[&](TextureResource& resource,bool is_depth){
    Download d;d.depth=is_depth;d.width=resource.HostWidth();d.height=resource.HostHeight();
    auto* raw=static_cast<plume::D3D12Texture*>(resource.texture.get())->d3d;const auto desc=raw->GetDesc();UINT64 bytes;
    const u32 planes=is_depth?2:1;raw_device->GetCopyableFootprints(&desc,0,planes,0,d.fp.data(),nullptr,nullptr,&bytes);
    d.buffer=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));CHECK(d.buffer);
    commands->setFramebuffer(nullptr);commands->barriers(plume::RenderBarrierStage::COPY,{resource.texture.get(),plume::RenderTextureLayout::COPY_SOURCE});
    for(u32 plane=0;plane<planes;++plane){
      D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=raw;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=plane;
      dst.pResource=static_cast<plume::D3D12Buffer*>(d.buffer.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=d.fp[plane];
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }return d;
  };
  const auto decode=[&](Download& c,Download& d){
    CHECK(c.width==d.width && c.height==d.height);Pixels p;p.width=c.width;p.height=c.height;
    p.color.resize(p.width*p.height*4);p.depth.resize(p.width*p.height);p.stencil.resize(p.width*p.height);
    auto* cb=static_cast<unsigned char*>(c.buffer->map());auto* db=static_cast<unsigned char*>(d.buffer->map());CHECK(cb && db);
    for(u32 y=0;y<p.height;++y){
      std::memcpy(p.color.data()+y*p.width*4,cb+c.fp[0].Offset+y*c.fp[0].Footprint.RowPitch,p.width*4);
      std::memcpy(p.depth.data()+y*p.width,db+d.fp[0].Offset+y*d.fp[0].Footprint.RowPitch,p.width*4);
      std::memcpy(p.stencil.data()+y*p.width,db+d.fp[1].Offset+y*d.fp[1].Footprint.RowPitch,p.width);
    }c.buffer->unmap();d.buffer->unmap();return p;
  };
  const auto compare=[&](const Pixels& actual,const Pixels& source,bool ui){
    for(u32 y=0;y<actual.height;++y)for(u32 x=0;x<actual.width;++x){
      const u32 sx=u32(uint64_t(2*x+1)*source.width/(2*actual.width));
      const u32 sy=u32(uint64_t(2*y+1)*source.height/(2*actual.height));
      const u32 i=y*actual.width+x,j=sy*source.width+sx;
      const bool marker=ui && x>=900 && x<940 && y>=400 && y<430;
      const unsigned char ink[]{51,153,204,255};
      const float expected_depth=marker?.125f:source.depth[j];
      for(u32 channel=0;channel<4;++channel){const auto expected=marker?ink[channel]:source.color[j*4+channel];
        if(actual.color[i*4+channel]!=expected){printf("COLOR mismatch %ux%u<- %ux%u at%u,%u channel%u got%u expected%u\n",actual.width,actual.height,source.width,source.height,x,y,channel,actual.color[i*4+channel],expected);std::exit(24);}}
      if(std::memcmp(&actual.depth[i],&expected_depth,4) || actual.stencil[i]!=(marker?77:source.stencil[j])){
        printf("DEPTH/STENCIL mismatch %ux%u at%u,%u\n",actual.width,actual.height,x,y);std::exit(25);}
    }
  };
  u32 cases=0;
  for(const auto shape:{std::pair{1129u,635u},std::pair{640u,360u},std::pair{1280u,720u},std::pair{960u,540u},std::pair{1920u,1080u}}){
    const u32 w=shape.first,h=shape.second;u32 warm_allocations=0,warm_registrations=0;
    for(u32 frame=0;frame<3;++frame){
      commands->begin();list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
      if(color->HostWidth()!=w || color->HostHeight()!=h){
        CHECK(ReconfigureNativeSceneSurfaces({1280,720,w,h}));
      }
      Pixels input;input.width=w;input.height=h;input.color.resize(w*h*4);input.depth.resize(w*h);input.stencil.resize(w*h);
      for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x){const u32 i=y*w+x;
        for(u32 c=0;c<4;++c)input.color[4*i+c]=static_cast<unsigned char>(x*17+y*29+c*63+frame*7);
        input.depth[i]=float((x*7919+y*104729)%0xFFFFFFu)/float(0xFFFFFFu);input.stencil[i]=static_cast<unsigned char>(x+y*17+frame);
        if(w==1129){std::memcpy(input.color.data()+i*4,captured_color.data()+i*4,4);
          std::memcpy(&input.depth[i],captured_depth.data()+i*8,4);input.stencil[i]=captured_depth[i*8+4];}
      }
      upload(*color,input,false);upload(*depth,input,true);
      CHECK(BeginNativeCompositionTargets(10,20));CHECK(color->HostWidth()==1920 && depth->HostHeight()==1080);
      auto before_color=download(*color,false),before_depth=download(*depth,true);
      const plume::RenderTexture* attachments[]{color->texture.get()};
      std::shared_ptr<plume::RenderFramebuffer> framebuffer=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,1,depth->texture.get()));CHECK(framebuffer);
      const plume::RenderTextureBarrier writable[]{{color->texture.get(),plume::RenderTextureLayout::COLOR_WRITE},{depth->texture.get(),plume::RenderTextureLayout::DEPTH_WRITE}};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,writable,2);commands->setFramebuffer(framebuffer.get());
      const plume::RenderRect rect(900,400,940,430);
      commands->clearColor(0,plume::RenderColor(.2f,.6f,.8f,1),&rect,1);
      commands->clearDepthStencil(true,true,.125f,77,&rect,1);commands->setFramebuffer(nullptr);retired.push_back(std::move(framebuffer));
      auto native_color=download(*color,false),native_depth=download(*depth,true);
      CHECK(EndNativeCompositionTargets());CHECK(color->HostWidth()==w && depth->HostHeight()==h);
      auto low_color=download(*color,false),low_depth=download(*depth,true);
      commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
      auto before=decode(before_color,before_depth),native=decode(native_color,native_depth),low=decode(low_color,low_depth);
      compare(before,input,false);compare(native,input,true);compare(low,native,false);
      retired.clear();CHECK(!g_composition_restore_pending && g_composition_surfaces.empty());
      if(frame==0){warm_allocations=allocations;warm_registrations=registrations;}
      else CHECK(allocations==warm_allocations && registrations==warm_registrations);
      printf("PASS production GPU storage %ux%u native writes/copyback frame%u allocations=%u registrations=%u\n",w,h,frame,allocations,registrations);++cases;
    }
  }
  printf("PASS %u real GPU transaction cases, exact RGBA/depth/stencil, preset changes and warmed allocation reuse\n",cases);
}
