// Windowless readback using production cache keys and actual Plume IA calls.
#define NOMINMAX
#include <plume_d3d12.h>
#include "gpu_native/vertex_binding_cache.h"
#include "gpu_native/shader_input_usage.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 23;}}while(0)
std::vector<char> Read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)std::exit(3);std::vector<char>b(size_t(f.tellg()));f.seekg(0);f.read(b.data(),b.size());return b;}
int main(int argc,char**argv){
  CHECK(argc==3 || argc==4);
  const bool unused=argc==4 && std::strcmp(argv[3],"unused-stream")==0;
  const bool mutant=argc==4 && !unused;
  auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);
  auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();
  auto vbbytes=Read(argv[1]),psbytes=Read(argv[2]);
  if(unused)CHECK(ShaderReadsVertexInput(
      {reinterpret_cast<const uint8_t*>(vbbytes.data()),vbbytes.size()},"TEXCOORD",1)==false);
  auto vs=device->createShader(vbbytes.data(),vbbytes.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(psbytes.data(),psbytes.size(),"main",plume::RenderShaderFormat::DXIL);
  plume::RenderPipelineLayoutBuilder builder;builder.begin(false,true);builder.end();auto layout=builder.create(device.get());
  std::array<plume::RenderInputSlot,16> slots;
  for(unsigned i=0;i<16;++i)slots[i]=plume::RenderInputSlot(i,4);
  std::array<plume::RenderInputElement,2> elements;
  for(unsigned i=0;i<2;++i){elements[i].semanticName="TEXCOORD";elements[i].semanticIndex=i;elements[i].slotIndex=i?15:0;elements[i].format=plume::RenderFormat::R32_FLOAT;}
  plume::RenderGraphicsPipelineDesc desc;desc.pipelineLayout=layout.get();desc.vertexShader=vs.get();desc.pixelShader=ps.get();
  desc.inputSlots=slots.data();desc.inputSlotsCount=16;desc.inputElements=elements.data();desc.inputElementsCount=2;
  desc.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;desc.renderTargetCount=1;
  desc.renderTargetFormat[0]=plume::RenderFormat::R32G32B32A32_FLOAT;desc.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();
  desc.cullMode=plume::RenderCullMode::NONE;desc.fillMode=plume::RenderFillMode::SOLID;desc.depthClipEnabled=true;
  auto pipeline=device->createGraphicsPipeline(desc);CHECK(pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  plume::RenderTextureDesc td;td.dimension=plume::RenderTextureDimension::TEXTURE_2D;td.width=td.height=8;
  td.depth=td.arraySize=td.mipLevels=1;td.format=desc.renderTargetFormat[0];td.committed=true;td.flags=plume::RenderTextureFlag::RENDER_TARGET;
  td.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
  auto target=device->createTexture(td);const plume::RenderTexture* attachments[]{target.get()};
  auto fb=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,1));
  auto* targetRaw=static_cast<plume::D3D12Texture*>(target.get())->d3d;auto targetDesc=targetRaw->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 readBytes;
  raw->GetCopyableFootprints(&targetDesc,0,1,0,&footprint,nullptr,nullptr,&readBytes);
  readBytes=(readBytes+511)&~UINT64(511);
  auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(readBytes*7));
  std::array<std::array<float,64>,5> values{};
  for(unsigned i=0;i<64;++i){values[0][i]=.1f+.01f*i;values[1][i]=.7f+.02f*i;values[2][i]=9;values[3][i]=11;}
  std::array<std::unique_ptr<plume::RenderBuffer>,5> buffers;
  for(unsigned i=0;i<5;++i){buffers[i]=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));auto* m=buffers[i]->map();CHECK(m);std::memcpy(m,values[i].data(),256);buffers[i]->unmap();}
  unsigned totalSlots=0,totalCalls=0,cases=0;
  for(bool cacheOn:{false,true}){
    VertexBindingCache cache;
    for(uint64_t serial:{1ull,2ull}){
      commands->begin();commands->setGraphicsPipelineLayout(layout.get());commands->setPipeline(pipeline.get());
      commands->setViewports(plume::RenderViewport(0,0,8,8));commands->setScissors(plume::RenderRect(0,0,8,8));
      std::array<plume::RenderVertexBufferView,16> views;
      for(auto& view:views)view=plume::RenderVertexBufferView(buffers[2]->at(0),256);
      // Leave four bytes of headroom so step 2 changes only the offset.
      // Changing the size there too would hide a missing-offset cache key.
      views[0]=plume::RenderVertexBufferView(buffers[0]->at(0),252);
      slots[0].stride=4;std::array<std::array<float,2>,7> expected{};
      for(unsigned step=0;step<7;++step){
        if(step==2){views[0].buffer.offset=4;views[0].size=252;}
        if(step==3)slots[0].stride=8;
        if(step==4){views[0]=plume::RenderVertexBufferView(buffers[1]->at(0),256);slots[0].stride=4;}
        if(step==5)views[0].size=252;
        if(step==6)views[15]=plume::RenderVertexBufferView(buffers[3]->at(0),256);
        // Keep the unused input declaration while alternating real data and
        // the renderer's 256-byte zero-stream representation.
        if(unused)views[15]=plume::RenderVertexBufferView(buffers[step%2?4:2]->at(0),256);
        auto emit=[&](unsigned first,unsigned count){commands->setVertexBuffers(first,views.data()+first,count,slots.data()+first);if(cacheOn){totalSlots+=count;++totalCalls;}};
        if(cacheOn){auto identities=views;if(mutant)identities[0].buffer.offset=0;
          cache.Bind(commands.get(),serial,identities,slots,emit);
        }else emit(0,16);
        const unsigned source=step>=4?1:0,offset=unsigned(views[0].buffer.offset)/4,stride=slots[0].stride/4;
        expected[step]={values[source][offset]*.5f+values[source][offset+stride]*.28125f+
          values[source][offset+2*stride]*.21875f,unused?9.f:step==6?11.f:9.f};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(fb.get());commands->clearColor(0,plume::RenderColor(-1,-1,-1,-1));commands->drawInstanced(3,1,0,0);
        commands->setFramebuffer(nullptr);commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COPY_SOURCE));
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=targetRaw;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;dst.PlacedFootprint.Offset=step*readBytes;
        static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      }
      commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
      auto* bytes=static_cast<const uint8_t*>(readback->map());CHECK(bytes);
      for(unsigned step=0;step<7;++step){auto* actual=reinterpret_cast<const float*>(bytes+step*readBytes+4*footprint.Footprint.RowPitch+4*16);
        for(unsigned c=0;c<2;++c)if(!std::isfinite(actual[c])||std::abs(actual[c]-expected[step][c])>.00001f){
          std::fprintf(stderr,"mismatch cache=%u serial=%llu step=%u channel=%u got=%f expected=%f\n",cacheOn,serial,step,c,actual[c],expected[step][c]);return 23;}
        ++cases;
      }readback->unmap();
    }
  }
  if(unused){std::printf("PASS %u GPU cases: unread declared stream replaced with zero buffer, cached/uncached, offsets/strides/new recordings\n",cases);return 0;}
  CHECK(totalSlots==42 && totalCalls==12);
  std::printf("PASS %u GPU cases: exact cached/uncached values, offsets, strides, buffer replacement, high slot15, new recordings; 42 versus224 bound slots\n",cases);
}
