// Execute the production HostDevice bridge followed by a normal Plume draw.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <plume_d3d12.h>
#include "gpu_native/depth_resample.h"
#include "gpu_native/shaders/copy_vs.h"
#include "gpu_native/shaders/depth_resample_ps.h"
#include "gpu_native/shaders/depth_resample_depth_ps.h"
#include "gpu_native/shaders/depth_resample_stencil_ps.h"
#include <mutex>
#include <vector>
#include <memory>
#include <fstream>
#include <cstdio>
#include <cstdlib>
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
namespace legodimensions::gpu_native {
using u32=uint32_t;
struct State{std::unique_ptr<plume::RenderDevice> device;std::unique_ptr<DepthResampler> depth_resampler;};
State* g_state=nullptr;std::mutex g_mutex;std::recursive_mutex recording_mutex;
plume::RenderCommandList* fixture_commands=nullptr;
std::vector<std::shared_ptr<void>> retained;
struct HostDevice{
  static auto LockRecording(){return std::unique_lock(recording_mutex);}
  static auto BeginFrameCommands(){return fixture_commands;}
  static void RetireResource(std::shared_ptr<void> lease){retained.push_back(std::move(lease));}
  static bool ResampleDepthSurface(plume::RenderTexture*,plume::RenderTexture*);
};
#include "production-depth-bridge.inc"
}
using namespace legodimensions::gpu_native;
static void require(bool value){if(!value)std::exit(2);}
static std::vector<char> read(const char* path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f));
  std::vector<char>b(size_t(f.tellg()));f.seekg(0);f.read(b.data(),b.size());return b;
}
int main(int argc,char**argv){
  if(argc!=3)return 1;setvbuf(stdout,nullptr,_IONBF,0);
  auto render_interface=plume::CreateD3D12Interface();State state;state.device=render_interface->createDevice();
  require(bool(state.device));g_state=&state;auto* device=state.device.get();
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();fixture_commands=commands.get();
  auto texture=[&](unsigned w,unsigned h,bool depth){
    plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
    d.width=w;d.height=h;d.depth=d.arraySize=d.mipLevels=1;d.committed=true;
    d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
    d.format=depth?plume::RenderFormat::D32_FLOAT_S8_UINT:plume::RenderFormat::R8G8B8A8_UNORM;
    d.flags=depth?plume::RenderTextureFlag::DEPTH_TARGET:plume::RenderTextureFlag::RENDER_TARGET;
    return device->createTexture(d);};
  auto source=texture(64,48,true),destination=texture(128,96,true),color=texture(128,96,false);
  require(bool(source)&&bool(destination)&&bool(color));
  plume::RenderFramebufferDesc src_desc;src_desc.depthAttachment=source.get();
  auto src_fb=device->createFramebuffer(src_desc);
  const plume::RenderTexture* attachments[]{color.get()};
  auto framebuffer=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,1,destination.get()));
  plume::RenderPipelineLayoutBuilder builder;builder.begin(false,true);builder.end();
  auto layout=builder.create(device);
  auto v=read(argv[1]),p=read(argv[2]);
  auto vs=device->createShader(v.data(),v.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(p.data(),p.size(),"main",plume::RenderShaderFormat::DXIL);
  plume::RenderGraphicsPipelineDesc desc;desc.pipelineLayout=layout.get();desc.vertexShader=vs.get();desc.pixelShader=ps.get();
  desc.primitiveTopology=plume::RenderPrimitiveTopology::LINE_LIST;desc.cullMode=plume::RenderCullMode::NONE;
  desc.fillMode=plume::RenderFillMode::SOLID;desc.renderTargetCount=1;
  desc.renderTargetFormat[0]=plume::RenderFormat::R8G8B8A8_UNORM;
  desc.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat=plume::RenderFormat::D32_FLOAT_S8_UINT;desc.depthEnabled=desc.depthWriteEnabled=false;
  desc.stencilEnabled=true;desc.stencilReadMask=0xFF;desc.stencilWriteMask=0;
  desc.stencilReference=3;desc.stencilFrontFace.compareFunction=plume::RenderComparisonFunction::EQUAL;
  desc.stencilBackFace=desc.stencilFrontFace;
  auto pipeline=device->createGraphicsPipeline(desc);require(bool(pipeline));
  static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->stencilRef=3;
  auto* raw_device=static_cast<plume::D3D12Device*>(device)->d3d;
  auto* raw_color=static_cast<plume::D3D12Texture*>(color.get())->d3d;
  auto resource=raw_color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;
  raw_device->GetCopyableFootprints(&resource,0,1,0,&footprint,nullptr,nullptr,&bytes);
  auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
  commands->begin();
  const plume::RenderTextureBarrier ready[]{{source.get(),plume::RenderTextureLayout::DEPTH_WRITE},
    {destination.get(),plume::RenderTextureLayout::DEPTH_WRITE},{color.get(),plume::RenderTextureLayout::COLOR_WRITE}};
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,ready,3);
  commands->setFramebuffer(src_fb.get());commands->clearDepthStencil(true,true,.75f,3);
  commands->setFramebuffer(framebuffer.get());commands->clearDepthStencil(true,true,.25f,3);
  commands->clearColor(0,plume::RenderColor(0,0,0,1));
  auto ordinary=[&]{
    commands->setFramebuffer(framebuffer.get());commands->setGraphicsPipelineLayout(layout.get());
    commands->setPipeline(pipeline.get());commands->setViewports(plume::RenderViewport(0,0,128,96));
    commands->setScissors(plume::RenderRect(0,0,128,96));commands->drawInstanced(2,1,0,0);};
  ordinary(); // Prime the same topology/stencil/root/PSO caches used after helper.
  commands->clearColor(0,plume::RenderColor(0,0,0,1));
  require(HostDevice::ResampleDepthSurface(source.get(),destination.get()));
  require(retained.size()==1);ordinary();
  commands->setFramebuffer(nullptr);
  commands->barriers(plume::RenderBarrierStage::COPY,
    plume::RenderTextureBarrier(color.get(),plume::RenderTextureLayout::COPY_SOURCE));
  D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=raw_color;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;
  to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
  static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
  auto* data=static_cast<unsigned char*>(readback->map());require(data!=nullptr);unsigned white=0;
  for(unsigned y=0;y<96;++y)for(unsigned x=0;x<128;++x){
    auto* pixel=data+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
    white+=pixel[0]==255 && pixel[1]==255 && pixel[2]==255;
  }readback->unmap();retained.clear();
  printf("ordinary line after production resize: white_pixels=%u\n",white);
  if(white<90 || white>110)return 23;
  printf("PASS Plume graphics resume: root/PSO/topology/stencil state restored\n");
}
