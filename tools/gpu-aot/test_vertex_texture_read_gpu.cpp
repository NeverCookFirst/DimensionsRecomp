#define NOMINMAX
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include "gpu_native/native_recording.h"
#include "gpu_native/vertex_texture_read.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateD3D12Interface(); }
using namespace legodimensions::gpu_native;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 23; } } while (0)
std::vector<char> Read(const char* path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);
  if (!f) std::exit(3);
  std::vector<char> b(size_t(f.tellg()));f.seekg(0);f.read(b.data(),b.size());return b;
}
int main(int argc,char** argv) {
  CHECK(argc==3 || argc==4);
  const bool missing_barrier=argc==4;
  ID3D12Debug* debug=nullptr;
  CHECK(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))));
  debug->EnableDebugLayer();debug->Release();
  auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);
  auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto commands=queue->createCommandList();auto fence=device->createCommandFence();
  auto texture=[&](unsigned size) {
    plume::RenderTextureDesc d;d.dimension=plume::RenderTextureDimension::TEXTURE_2D;
    d.width=d.height=size;d.depth=d.arraySize=d.mipLevels=1;
    d.format=plume::RenderFormat::R32G32B32A32_FLOAT;d.committed=true;
    d.flags=plume::RenderTextureFlag::RENDER_TARGET;
    d.multisampling.sampleCount=plume::RenderSampleCount::COUNT_1;
    return device->createTexture(d);
  };
  auto source=texture(1),target=texture(8);
  auto* source_native=static_cast<plume::D3D12Texture*>(source.get());
  auto* target_native=static_cast<plume::D3D12Texture*>(target.get());
  CHECK(source_native->d3d && target_native->d3d);
  auto view=source->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
  plume::RenderDescriptorSetBuilder sets;sets.begin();sets.addTexture(0);sets.end();
  auto table=sets.create(device.get());table->setTexture(0,source.get(),plume::RenderTextureLayout::SHADER_READ,view.get());
  plume::RenderPipelineLayoutBuilder layout_builder;layout_builder.begin(false,true);
  layout_builder.addDescriptorSet(sets);layout_builder.end();auto layout=layout_builder.create(device.get());
  auto vb=Read(argv[1]),pb=Read(argv[2]);
  auto vs=device->createShader(vb.data(),vb.size(),"main",plume::RenderShaderFormat::DXIL);
  auto ps=device->createShader(pb.data(),pb.size(),"main",plume::RenderShaderFormat::DXIL);
  plume::RenderGraphicsPipelineDesc pd;pd.pipelineLayout=layout.get();pd.vertexShader=vs.get();pd.pixelShader=ps.get();
  pd.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;pd.renderTargetCount=1;
  pd.renderTargetFormat[0]=plume::RenderFormat::R32G32B32A32_FLOAT;pd.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();
  pd.cullMode=plume::RenderCullMode::NONE;pd.fillMode=plume::RenderFillMode::SOLID;
  auto pipeline=device->createGraphicsPipeline(pd);CHECK(pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d);
  const plume::RenderTexture* attachments[]{target.get()};
  auto fb=device->createFramebuffer(plume::RenderFramebufferDesc(attachments,1));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
  auto td=target_native->d3d->GetDesc();raw->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
  auto readback=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes));
  auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(512));
  float expected[4]{};unsigned checks=0;
  for (bool deferred:{false,true}) for(unsigned step=0;step<32;++step) {
    if(step%3==0) {
      for(unsigned c=0;c<4;++c)expected[c]=float(step+c+1)/64.f;
      auto* mapped=upload->map();CHECK(mapped);std::memcpy(mapped,expected,sizeof(expected));upload->unmap();
    }
    commands->begin();
    auto tape=CreateNativeDrawCommands();
    plume::RenderCommandList* record=deferred ? tape.get() : commands.get();
    if(step%3==0) {
      record->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(source.get(),plume::RenderTextureLayout::COPY_DEST));
      record->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(source.get(),0),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(),plume::RenderFormat::R32G32B32A32_FLOAT,1,1,1,16,0));
      record->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(source.get(),plume::RenderTextureLayout::SHADER_READ));
    }
    if(!missing_barrier)PrepareVertexTextureShaderRead(record,source.get());
    record->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COLOR_WRITE));
    record->setFramebuffer(fb.get());record->setGraphicsPipelineLayout(layout.get());
    record->setGraphicsDescriptorSet(table.get(),0);record->setPipeline(pipeline.get());
    record->setViewports(plume::RenderViewport(0,0,8,8));record->setScissors(plume::RenderRect(0,0,8,8));
    record->drawInstanced(3,1,0,0);record->setFramebuffer(nullptr);
    record->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(target.get(),plume::RenderTextureLayout::COPY_SOURCE));
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target_native->d3d;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=static_cast<plume::D3D12Buffer*>(readback.get())->d3d;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    RecordNativeCommand(record,[src,dst](auto* list){static_cast<plume::D3D12CommandList*>(list)->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);});
    if(deferred)tape->Replay(commands.get());
    commands->end();
    // Reject the deliberately omitted transition before submitting invalid GPU work.
    CHECK((source_native->resourceStates & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)==D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    auto* result=static_cast<const unsigned char*>(readback->map());CHECK(result);
    const auto* pixel=reinterpret_cast<const float*>(result+4*footprint.Footprint.RowPitch+4*16);
    for(unsigned c=0;c<4;++c)CHECK(std::isfinite(pixel[c]) && std::abs(pixel[c]-expected[c])<.00001f);
    readback->unmap();++checks;
  }
  ID3D12InfoQueue* messages=nullptr;CHECK(SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&messages))));
  unsigned errors=0;
  for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
    SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<unsigned char> storage(size);
    auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());messages->GetMessage(i,msg,&size);
    if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::fprintf(stderr,"D3D12 [%u] %s\n",msg->ID,msg->pDescription);}
  }
  messages->Release();CHECK(errors==0);CHECK(raw->GetDeviceRemovedReason()==S_OK);
  std::printf("PASS %u GPU readbacks: VS+PS texture fetch, fresh uploads/cached content, immediate/detached replay; zero D3D12 errors\n",checks);
}
