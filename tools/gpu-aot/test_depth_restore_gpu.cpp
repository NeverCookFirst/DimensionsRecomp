// Execute the production EDRAM color-to-depth shader. Exact rational fetch oracle.
#define NOMINMAX
#include <plume_d3d12.h>
#include "gpu_native/depth_alias.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
using namespace legodimensions::gpu_native;
std::vector<char> Read(const char* path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
  std::vector<char>b(size_t(f.tellg()));f.seekg(0);f.read(b.data(),b.size());return b;
}
uint32_t DepthCode(uint32_t x,uint32_t y){return 0x123400u+((x*719u+y*1777u)%0xBC0000u);}
int main(int argc,char**argv) {
  assert(argc==3);
  auto render=plume::CreateD3D12Interface();auto device=render->createDevice();assert(device);
  auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();
  plume::RenderDescriptorSetBuilder tables;tables.begin();tables.addTexture(0,1);tables.end(true,1);
  auto table=tables.create(device.get());
  plume::RenderPipelineLayoutBuilder builder;builder.begin(false,true);builder.addDescriptorSet(tables);
  builder.addPushConstant(3,4,16,plume::RenderShaderStageFlag::PIXEL);builder.end();
  auto layout=builder.create(device.get());auto vb=Read(argv[1]),pb=Read(argv[2]);
  auto vs=device->createShader(vb.data(),vb.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(pb.data(),pb.size(),"main",plume::RenderShaderFormat::DXIL);
  plume::RenderGraphicsPipelineDesc p;p.pipelineLayout=layout.get();p.vertexShader=vs.get();p.pixelShader=ps.get();
  p.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  p.depthTargetFormat=plume::RenderFormat::D32_FLOAT_S8_UINT;p.depthEnabled=p.depthWriteEnabled=true;
  p.depthFunction=plume::RenderComparisonFunction::ALWAYS;p.cullMode=plume::RenderCullMode::NONE;
  p.fillMode=plume::RenderFillMode::SOLID;p.renderTargetCount=0;
  auto pipeline=device->createGraphicsPipeline(p);assert(pipeline&&static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  unsigned cases=0;
  for(uint32_t w:{640u,960u,1129u,1280u,1919u,1920u}) {
    constexpr uint32_t h=32;
    plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
    d.width=w;d.height=h;d.depth=d.arraySize=d.mipLevels=1;d.committed=true;
    d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
    d.format=plume::RenderFormat::R32G32B32A32_FLOAT;
    auto source=device->createTexture(d);
    d.format=plume::RenderFormat::D32_FLOAT_S8_UINT;d.flags=plume::RenderTextureFlag::DEPTH_TARGET;
    auto target=device->createTexture(d);assert(source&&target);
    auto view=source->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
    table->setTexture(0,source.get(),plume::RenderTextureLayout::SHADER_READ,view.get());
    plume::RenderFramebufferDesc fd;fd.depthAttachment=target.get();auto fb=device->createFramebuffer(fd);assert(fb);
    auto* sr=static_cast<plume::D3D12Texture*>(source.get())->d3d;
    auto* tr=static_cast<plume::D3D12Texture*>(target.get())->d3d;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT in_fp{},out_fp{};UINT64 in_bytes,out_bytes;
    auto in_desc=sr->GetDesc(),out_desc=tr->GetDesc();
    raw->GetCopyableFootprints(&in_desc,0,1,0,&in_fp,nullptr,nullptr,&in_bytes);
    raw->GetCopyableFootprints(&out_desc,0,1,0,&out_fp,nullptr,nullptr,&out_bytes);
    auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(in_bytes));
    auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(out_bytes));
    auto* bytes=static_cast<uint8_t*>(upload->map());assert(bytes);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
      const uint32_t packed=DepthCode(x,y)<<8;
      const float color[]{float(packed&255)/255,float((packed>>8)&255)/255,
        float((packed>>16)&255)/255,float(packed>>24)/255};
      std::memcpy(bytes+y*in_fp.Footprint.RowPitch+x*16,color,16);
    }upload->unmap();
    commands->begin();commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(source.get(),plume::RenderTextureLayout::COPY_DEST));
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
    from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=in_fp;
    to.pResource=sr;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto* list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    const plume::RenderTextureBarrier ready[]{{source.get(),plume::RenderTextureLayout::SHADER_READ},
      {target.get(),plume::RenderTextureLayout::DEPTH_WRITE}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,ready,2);commands->setFramebuffer(fb.get());
    commands->clearDepthStencil(true,true,0,0);
    commands->setViewports(plume::RenderViewport(0,0,float(w),float(h)));
    commands->setScissors(plume::RenderRect(0,0,w,h));commands->setGraphicsPipelineLayout(layout.get());
    commands->setGraphicsDescriptorSet(table.get(),0);commands->setPipeline(pipeline.get());
    struct {uint32_t index,index2;float half_tile,unused;}push{0,32,DepthColorAliasHalfTile(1280,w),0};
    commands->setGraphicsPushConstants(0,&push);commands->drawInstanced(3,1,0,0);commands->setFramebuffer(nullptr);
    commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COPY_SOURCE));
    from={};to={};from.pResource=tr;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;
    to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=out_fp;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);commands->end();
    queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    bytes=static_cast<uint8_t*>(readback->map());assert(bytes);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
      const uint64_t center=uint64_t(2*x+1)*16;
      const uint32_t half=uint32_t(center/w),sx=uint32_t((half%2?center-w:center+w)/32);
      assert(sx<w);const float expected=UnpackDepth24(DepthCode(sx,y));
      float actual;std::memcpy(&actual,bytes+y*out_fp.Footprint.RowPitch+x*4,4);
      if(!std::isfinite(actual)||std::abs(actual-expected)>0.00000012f){
        std::printf("FAIL width=%u pixel=%u,%u source=%u expected=%.9f got=%.9f\n",w,x,y,sx,expected,actual);return 1;
      }
    }readback->unmap();++cases;
  }
  std::printf("PASS %u production depth-restore GPU cases, including fractional tiles\n",cases);
}
