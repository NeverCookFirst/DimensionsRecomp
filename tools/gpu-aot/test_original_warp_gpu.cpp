// Windowless reproduction of one saved draw using original executed shaders,
// captured IA/CBVs/sampler/source and an independent captured target reference.
#define NOMINMAX
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT result,int line){
  if(FAILED(result)){std::fprintf(stderr,"HRESULT %08X line %d\n",unsigned(result),line);std::exit(2);}
}
#define HR(x) Check((x),__LINE__)
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 23;}}while(0)
static std::vector<char> Read(const char* path){
  std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)std::exit(3);
  std::vector<char> result(size_t(file.tellg()));file.seekg(0);file.read(result.data(),result.size());return result;
}
int main(int argc,char** argv){
  REQUIRE(argc==2 || argc==3);
  const std::filesystem::path folder=argv[1];
  const bool mutate=argc==3;
  const auto read=[&](const char* name){return Read((folder/name).string().c_str());};
  constexpr UINT width=1129,height=635;
  ComPtr<ID3D12Debug> debug;
  if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;HR(factory->EnumAdapters1(0,&adapter));
  ComPtr<ID3D12Device> device;HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
  ComPtr<ID3D12InfoQueue> messages;device.As(&messages);
  D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> commands;HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));HR(commands->Close());
  ComPtr<ID3D12Fence> fence;HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);REQUIRE(event);UINT64 serial=0;
  auto begin=[&]{HR(allocator->Reset());HR(commands->Reset(allocator.Get(),nullptr));};
  auto submit=[&]{HR(commands->Close());ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
    HR(queue->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));
    if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)std::exit(4);};
  auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
      heap==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result)));return result;};
  auto texture=[&](DXGI_FORMAT format,UINT w,UINT h,UINT slices,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=UINT16(slices);
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=flags;
    ComPtr<ID3D12Resource> result;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)));return result;};
  auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};commands->ResourceBarrier(1,&barrier);};
  auto upload=[&](ID3D12Resource* resource,const void* bytes,size_t count){
    void* mapped=nullptr;D3D12_RANGE empty{};HR(resource->Map(0,&empty,&mapped));std::memcpy(mapped,bytes,count);resource->Unmap(0,nullptr);};
  auto source=texture(DXGI_FORMAT_R8G8B8A8_UNORM,width,height,1,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
  auto target=texture(DXGI_FORMAT_R8G8B8A8_UNORM,width,height,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
  auto desc=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 imageBytes;
  device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&imageBytes);
  auto sourceBytes=read("source.bin"),before=read("target-before.bin"),expected=read("target-after.bin");
  REQUIRE(sourceBytes.size()==width*height*4 && before.size()==sourceBytes.size() && expected.size()==before.size());
  auto sourceUpload=buffer(imageBytes,D3D12_HEAP_TYPE_UPLOAD),targetUpload=buffer(imageBytes,D3D12_HEAP_TYPE_UPLOAD);
  auto uploadImage=[&](ID3D12Resource* uploadBuffer,const std::vector<char>& image){
    void* mapped;D3D12_RANGE empty{};HR(uploadBuffer->Map(0,&empty,&mapped));
    for(UINT y=0;y<height;++y)std::memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,image.data()+y*width*4,width*4);
    uploadBuffer->Unmap(0,nullptr);
  };
  uploadImage(sourceUpload.Get(),sourceBytes);uploadImage(targetUpload.Get(),before);
  begin();
  for(auto pair:{std::pair{source.Get(),sourceUpload.Get()},std::pair{target.Get(),targetUpload.Get()}}){
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=pair.first;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=pair.second;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  }
  transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);submit();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=64;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> resources;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&resources)));
  auto cpu=resources->GetCPUDescriptorHandleForHeapStart();const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  for(UINT i=0;i<64;++i){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(source.Get(),&srv,{cpu.ptr+i*stride});}
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;hd.NumDescriptors=16;
  ComPtr<ID3D12DescriptorHeap> samplers;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samplers)));
  D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU=sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sampler.MaxLOD=.25f;sampler.MaxAnisotropy=1;
  for(UINT i=0;i<16;++i)device->CreateSampler(&sampler,{samplers->GetCPUDescriptorHandleForHeapStart().ptr+i*device->GetDescriptorHandleIncrementSize(hd.Type)});
  D3D12_DESCRIPTOR_RANGE ranges[4]{};D3D12_ROOT_PARAMETER params[5]{};
  for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor={i,4};}
  for(UINT i=0;i<4;++i)ranges[i]={i==3?D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:D3D12_DESCRIPTOR_RANGE_TYPE_SRV,i==3?16u:64u,0,i,0};
  params[3].ParameterType=params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable={3,ranges};params[4].DescriptorTable={1,ranges+3};
  D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=5;root.pParameters=params;root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob,error;HRESULT status=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
  if(FAILED(status)&&error)std::fprintf(stderr,"%s",static_cast<const char*>(error->GetBufferPointer()));HR(status);
  ComPtr<ID3D12RootSignature> signature;HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&signature)));
  auto vs=read("vs.dxil"),ps=read("ps.dxil");D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature=signature.Get();pipelineDesc.VS={vs.data(),vs.size()};pipelineDesc.PS={ps.data(),ps.size()};
  D3D12_INPUT_ELEMENT_DESC position{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
  pipelineDesc.InputLayout={&position,1};pipelineDesc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
  pipelineDesc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipelineDesc.RasterizerState.DepthClipEnable=TRUE;
  pipelineDesc.SampleMask=UINT_MAX;pipelineDesc.SampleDesc.Count=1;pipelineDesc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineDesc.NumRenderTargets=1;pipelineDesc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
  pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
  ComPtr<ID3D12PipelineState> pipeline;HR(device->CreateGraphicsPipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)));
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> rtvs;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)));auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,rtv);
  std::array<ComPtr<ID3D12Resource>,3> constants;
  for(UINT i=0;i<3;++i){auto bytes=read(("cb"+std::to_string(i)+".bin").c_str());
    if(mutate && i==1){const float gain=.75f;std::memcpy(bytes.data()+45*16+4,&gain,4);}
    constants[i]=buffer((bytes.size()+255)&~size_t(255),D3D12_HEAP_TYPE_UPLOAD);upload(constants[i].Get(),bytes.data(),bytes.size());}
  auto vertexBytes=read("vb-0.bin");REQUIRE(vertexBytes.size()==1536*36);
  auto vertices=buffer(vertexBytes.size(),D3D12_HEAP_TYPE_UPLOAD);upload(vertices.Get(),vertexBytes.data(),vertexBytes.size());
  auto readback=buffer(imageBytes,D3D12_HEAP_TYPE_READBACK);
  begin();commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);commands->SetGraphicsRootSignature(signature.Get());commands->SetPipelineState(pipeline.Get());
  for(UINT i=0;i<3;++i)commands->SetGraphicsRootConstantBufferView(i,constants[i]->GetGPUVirtualAddress());
  ID3D12DescriptorHeap* heaps[]{resources.Get(),samplers.Get()};commands->SetDescriptorHeaps(2,heaps);
  commands->SetGraphicsRootDescriptorTable(3,resources->GetGPUDescriptorHandleForHeapStart());commands->SetGraphicsRootDescriptorTable(4,samplers->GetGPUDescriptorHandleForHeapStart());
  D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
  commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(),UINT(vertexBytes.size()),36};commands->IASetVertexBuffers(0,1,&vb);commands->DrawInstanced(1536,1,0,0);
  transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
  void* mapped;HR(readback->Map(0,nullptr,&mapped));unsigned maxError=0;size_t differing=0,overOne=0;std::vector<char> actual(width*height*4);
  for(UINT y=0;y<height;++y)std::memcpy(actual.data()+y*width*4,static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,width*4);
  readback->Unmap(0,nullptr);for(size_t i=0;i<actual.size();++i){const unsigned delta=unsigned(std::abs(int(uint8_t(actual[i]))-int(uint8_t(expected[i]))));
    maxError=std::max(maxError,delta);differing+=delta!=0;overOne+=delta>1;}
  std::ofstream output(folder/(mutate?"mutant-rerender.bin":"original-rerender.bin"),std::ios::binary);output.write(actual.data(),actual.size());output.close();
  if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T size=0;HR(messages->GetMessage(i,nullptr,&size));std::vector<char> storage(size);
    auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());HR(messages->GetMessage(i,m,&size));
    if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12 validation: %s\n",m->pDescription);return 23;}}
  CloseHandle(event);std::printf("original captured warp: pixels=%u max_code_error=%u differing_channels=%zu channels_over_one=%zu mutated_gain=%u debug_layer=%u\n",width*height,maxError,differing,overOne,mutate,bool(debug));
  return overOne==0?0:24;
}
