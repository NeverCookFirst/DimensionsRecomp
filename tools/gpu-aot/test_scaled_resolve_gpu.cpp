// Real production resampling shader + mapping. Windowless DX12 readback oracle.
#define NOMINMAX
#include <plume_d3d12.h>
#include "gpu_native/scaled_resolve.h"
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
int main(int argc,char**argv) {
  assert(argc==3);
  auto render_interface=plume::CreateD3D12Interface();assert(render_interface);
  auto device=render_interface->createDevice();assert(device);
  auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();
  auto texture=[&](uint32_t w,uint32_t h){
    plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
    d.width=w;d.height=h;d.depth=d.arraySize=d.mipLevels=1;
    d.format=plume::RenderFormat::R32G32B32A32_FLOAT;d.committed=true;
    d.flags=plume::RenderTextureFlag::RENDER_TARGET;
    d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
    return device->createTexture(d);
  };
  plume::RenderDescriptorSetBuilder tables;tables.begin();tables.addTexture(0,1);tables.end(true,1);
  auto table=tables.create(device.get());
  plume::RenderPipelineLayoutBuilder builder;builder.begin(false,true);
  builder.addDescriptorSet(tables);
  for(uint32_t i:{0u,1u,2u,4u})builder.addRootDescriptor(i,4,plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addPushConstant(3,4,16,plume::RenderShaderStageFlag::PIXEL);builder.end();
  auto layout=builder.create(device.get());
  auto vb=Read(argv[1]),pb=Read(argv[2]);
  auto vs=device->createShader(vb.data(),vb.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(pb.data(),pb.size(),"main",plume::RenderShaderFormat::DXIL);
  plume::RenderGraphicsPipelineDesc p;p.pipelineLayout=layout.get();p.vertexShader=vs.get();p.pixelShader=ps.get();
  p.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;p.renderTargetCount=1;
  p.renderTargetFormat[0]=plume::RenderFormat::R32G32B32A32_FLOAT;
  p.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();p.cullMode=plume::RenderCullMode::NONE;
  p.depthEnabled=p.depthWriteEnabled=false;p.fillMode=plume::RenderFillMode::SOLID;
  auto pipeline=device->createGraphicsPipeline(p);assert(pipeline&&static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  unsigned cases=0;
  for(const auto size:{std::pair{1129u,635u},std::pair{640u,360u},std::pair{1920u,1080u},std::pair{2560u,1440u},std::pair{3840u,2160u}})
  for(bool crop:{false,true}) {
    const uint32_t sw=size.first,sh=size.second,dw=337,dh=199;
    auto source=texture(sw+24,sh+32),target=texture(dw,dh);assert(source&&target);
    auto view=source->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
    table->setTexture(0,source.get(),plume::RenderTextureLayout::SHADER_READ,view.get());
    const plume::RenderTexture* attachments[]{target.get()};
    auto fb=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,1));assert(fb);
    const ResolveRegion input{crop?17u:0u,crop?23u:0u,sw,sh,0,0};
    const ResolveRegion output{0,0,320,180,crop?7u:0u,crop?11u:0u};
    const auto mapping=ScaledResolveMapping(input,output);assert(mapping);
    auto cb=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
    auto* constants=cb->map();assert(constants);std::memcpy(constants,&*mapping,sizeof(*mapping));cb->unmap();
    auto* source_raw=static_cast<plume::D3D12Texture*>(source.get())->d3d;
    auto* target_raw=static_cast<plume::D3D12Texture*>(target.get())->d3d;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT in_fp{},out_fp{};UINT64 in_bytes,out_bytes;
    auto in_desc=source_raw->GetDesc(),out_desc=target_raw->GetDesc();
    raw->GetCopyableFootprints(&in_desc,0,1,0,&in_fp,nullptr,nullptr,&in_bytes);
    raw->GetCopyableFootprints(&out_desc,0,1,0,&out_fp,nullptr,nullptr,&out_bytes);
    auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(in_bytes));
    auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(out_bytes));
    auto* bytes=static_cast<uint8_t*>(upload->map());assert(bytes);
    for(uint32_t y=0;y<sh+32;++y)for(uint32_t x=0;x<sw+24;++x){
      const float color[]{float(x)/(sw+24),float(y)/(sh+32),float((x+17*y)%61)/61,1};
      std::memcpy(bytes+y*in_fp.Footprint.RowPitch+x*16,color,16);
    }upload->unmap();
    commands->begin();
    commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(source.get(),plume::RenderTextureLayout::COPY_DEST));
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
    src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=in_fp;
    dst.pResource=source_raw;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto* list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    const plume::RenderTextureBarrier ready[]{{source.get(),plume::RenderTextureLayout::SHADER_READ},{target.get(),plume::RenderTextureLayout::COLOR_WRITE}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,ready,2);commands->setFramebuffer(fb.get());
    commands->clearColor(0,plume::RenderColor(-1,-1,-1,-1));
    commands->setViewports(plume::RenderViewport(float(output.x),float(output.y),320,180));
    commands->setScissors(plume::RenderRect(output.x,output.y,output.x+320,output.y+180));
    commands->setGraphicsPipelineLayout(layout.get());commands->setGraphicsDescriptorSet(table.get(),0);
    commands->setGraphicsRootDescriptor(cb->at(0),3);commands->setPipeline(pipeline.get());
    const uint32_t push[4]{};commands->setGraphicsPushConstants(0,push);commands->drawInstanced(3,1,0,0);
    commands->setFramebuffer(nullptr);
    commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COPY_SOURCE));
    src={};dst={};src.pResource=target_raw;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out_fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);commands->end();
    queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    bytes=static_cast<uint8_t*>(readback->map());assert(bytes);
    for(uint32_t y=0;y<180;++y)for(uint32_t x=0;x<320;++x){
      const auto sx=uint32_t(input.left+(double(x)+.5)*sw/320),sy=uint32_t(input.top+(double(y)+.5)*sh/180);
      const float expected[]{float(sx)/(sw+24),float(sy)/(sh+32),float((sx+17*sy)%61)/61,1};
      const auto* actual=reinterpret_cast<float*>(bytes+(y+output.y)*out_fp.Footprint.RowPitch+(x+output.x)*16);
      for(unsigned c=0;c<4;++c)if(std::abs(actual[c]-expected[c])>0.00001f){
        std::printf("FAIL %ux%u crop=%u pixel=%u,%u channel=%u expected=%f got=%f\n",sw,sh,crop,x,y,c,expected[c],actual[c]);return 1;
      }
    }readback->unmap();++cases;
  }
  std::printf("PASS %u production scaled-resolve GPU cases, independent axes and offset rectangles\n",cases);
}
