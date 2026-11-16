// Windowless coverage/depth oracle for real translated material pixel shaders.
// The caller supplies an original PS, exact bank companion, and a matching VS.
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
  REQUIRE(argc==5 || argc==6); // optional coverage bank stage2..6
  const bool alphaTest=std::atoi(argv[4])!=0;
  const unsigned coverageMode=argc==6?unsigned(std::atoi(argv[5])):0;
  constexpr UINT width=32,height=24;
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
  // Each descriptor is initialized, with distinct 2D/3D/cube spaces. A small
  // gradient texture supplies finite nonconstant material inputs, not null SRVs.
  auto sampled=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,8,8,6,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
  auto sampled2D=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,8,8,1,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
  auto sampledDesc=sampled->GetDesc();std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,6> layouts{};UINT64 sampledBytes;
  device->GetCopyableFootprints(&sampledDesc,0,6,0,layouts.data(),nullptr,nullptr,&sampledBytes);
  auto sampledUpload=buffer(sampledBytes,D3D12_HEAP_TYPE_UPLOAD);void* mapped=nullptr;D3D12_RANGE empty{};
  HR(sampledUpload->Map(0,&empty,&mapped));
  for(UINT face=0;face<6;++face)for(UINT y=0;y<8;++y){
    auto* row=reinterpret_cast<float*>(static_cast<char*>(mapped)+layouts[face].Offset+y*layouts[face].Footprint.RowPitch);
    for(UINT x=0;x<8;++x){row[x*4]=.15f+.08f*x;row[x*4+1]=.2f+.07f*y;row[x*4+2]=.3f+.04f*face;row[x*4+3]=.1f+.1f*((x+y)%8);}}
  sampledUpload->Unmap(0,nullptr);begin();
  for(UINT face=0;face<6;++face){D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=sampledUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=layouts[face];
    dst.pResource=sampled.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=face;commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if(face==0){dst.pResource=sampled2D.Get();commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}}
  transition(sampled.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);submit();
  begin();transition(sampled2D.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);submit();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=96;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> resources;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&resources)));
  auto resourceCpu=resources->GetCPUDescriptorHandleForHeapStart();UINT resourceStride=device->GetDescriptorHandleIncrementSize(hd.Type);
  for(UINT index=0;index<96;++index){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if(index<32){srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;}
    else if(index<64){srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;srv.Texture3D.MipLevels=1;}
    else{srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;srv.TextureCube.MipLevels=1;}
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{resourceCpu.ptr+index*resourceStride};device->CreateShaderResourceView(index<32?sampled2D.Get():index<64?nullptr:sampled.Get(),&srv,cpu);}
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;hd.NumDescriptors=32;
  ComPtr<ID3D12DescriptorHeap> samplers;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samplers)));
  D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
  sampler.MaxAnisotropy=1;
  for(UINT i=0;i<32;++i)device->CreateSampler(&sampler,{samplers->GetCPUDescriptorHandleForHeapStart().ptr+i*device->GetDescriptorHandleIncrementSize(hd.Type)});
  D3D12_DESCRIPTOR_RANGE ranges[4]{};D3D12_ROOT_PARAMETER params[5]{};
  for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor={i,4};}
  for(UINT i=0;i<4;++i)ranges[i]={i==3?D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:D3D12_DESCRIPTOR_RANGE_TYPE_SRV,32,0,i,i==3?0:i*32};
  params[3].ParameterType=params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable={3,ranges};params[4].DescriptorTable={1,ranges+3};
  D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=5;root.pParameters=params;
  ComPtr<ID3DBlob> blob,error;HRESULT rootResult=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
  if(FAILED(rootResult)&&error)std::fprintf(stderr,"%s",static_cast<const char*>(error->GetBufferPointer()));HR(rootResult);
  ComPtr<ID3D12RootSignature> signature;HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&signature)));
  auto vertex=Read(argv[1]);
  auto pipeline=[&](const char* path,bool coverage=false){auto pixel=Read(path);D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature=signature.Get();desc.VS={vertex.data(),vertex.size()};desc.PS={pixel.data(),pixel.size()};
    desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
    desc.DepthStencilState.DepthEnable=TRUE;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    desc.SampleMask=UINT_MAX;desc.SampleDesc.Count=1;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets=2;desc.RTVFormats[0]=desc.RTVFormats[1]=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    for(auto& rt:desc.BlendState.RenderTarget)rt.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    if(coverage){
      desc.BlendState.IndependentBlendEnable=TRUE;
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask=std::getenv("LEGO_TEST_WRITE_MOTION")?D3D12_COLOR_WRITE_ENABLE_ALL:0;
      auto& reactive=desc.BlendState.RenderTarget[1];reactive.BlendEnable=TRUE;
      reactive.SrcBlend=reactive.DestBlend=reactive.SrcBlendAlpha=reactive.DestBlendAlpha=D3D12_BLEND_ONE;
      reactive.BlendOp=reactive.BlendOpAlpha=std::getenv("LEGO_TEST_COPY_REACTIVE")?D3D12_BLEND_OP_ADD:D3D12_BLEND_OP_MAX;
      if(std::getenv("LEGO_TEST_COPY_REACTIVE"))reactive.BlendEnable=FALSE;
    }
    ComPtr<ID3D12PipelineState> result;HR(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&result)));return result;};
  auto original=pipeline(argv[2]),companion=pipeline(argv[3],coverageMode!=0);
  std::array<ComPtr<ID3D12Resource>,3> targets{
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,width,height,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET),
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,width,height,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET),
    texture(DXGI_FORMAT_D32_FLOAT,width,height,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE)};
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> rtvs;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)));
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> rtv{rtvs->GetCPUDescriptorHandleForHeapStart(),rtvs->GetCPUDescriptorHandleForHeapStart()};rtv[1].ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);
  for(UINT i=0;i<2;++i)device->CreateRenderTargetView(targets[i].Get(),nullptr,rtv[i]);
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
  ComPtr<ID3D12DescriptorHeap> dsvs;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsvs)));auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();device->CreateDepthStencilView(targets[2].Get(),nullptr,dsv);
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,3> footprints{};std::array<ComPtr<ID3D12Resource>,3> readbacks;
  for(UINT i=0;i<3;++i){auto desc=targets[i]->GetDesc();UINT64 bytes;device->GetCopyableFootprints(&desc,0,1,0,&footprints[i],nullptr,nullptr,&bytes);readbacks[i]=buffer(bytes,D3D12_HEAP_TYPE_READBACK);}
  auto current=buffer(256,D3D12_HEAP_TYPE_UPLOAD),pixel=buffer(4096,D3D12_HEAP_TYPE_UPLOAD),shared=buffer(768,D3D12_HEAP_TYPE_UPLOAD);
  auto draw=[&](ID3D12PipelineState* state){begin();const float clear[]{-99,-99,-99,-99};
    for(auto cpu:rtv)commands->ClearRenderTargetView(cpu,clear,0,nullptr);
    if(coverageMode){const float velocity[]{.125f,-.25f,0,0},reactive[]{.15f,.15f,.15f,.15f};
      commands->ClearRenderTargetView(rtv[0],velocity,0,nullptr);commands->ClearRenderTargetView(rtv[1],reactive,0,nullptr);}
    commands->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    commands->OMSetRenderTargets(2,rtv.data(),FALSE,&dsv);commands->SetGraphicsRootSignature(signature.Get());commands->SetPipelineState(state);
    commands->SetGraphicsRootConstantBufferView(0,current->GetGPUVirtualAddress());commands->SetGraphicsRootConstantBufferView(1,pixel->GetGPUVirtualAddress());commands->SetGraphicsRootConstantBufferView(2,shared->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[]{resources.Get(),samplers.Get()};commands->SetDescriptorHeaps(2,heaps);commands->SetGraphicsRootDescriptorTable(3,resources->GetGPUDescriptorHandleForHeapStart());commands->SetGraphicsRootDescriptorTable(4,samplers->GetGPUDescriptorHandleForHeapStart());
    D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    for(UINT i=0;i<3;++i){const auto state=i==2?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
      transition(targets[i].Get(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
      src.pResource=targets[i].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readbacks[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[i];
      commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(targets[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,state);}
    submit();std::array<std::vector<float>,3> data;
    for(UINT i=0;i<3;++i){HR(readbacks[i]->Map(0,nullptr,&mapped));const UINT channels=i==2?1:4;data[i].resize(width*height*channels);
      for(UINT y=0;y<height;++y)std::memcpy(data[i].data()+y*width*channels,static_cast<char*>(mapped)+footprints[i].Offset+y*footprints[i].Footprint.RowPitch,width*channels*4);readbacks[i]->Unmap(0,nullptr);}return data;};
  unsigned cases=0,coveredTotal=0,discardedTotal=0;
  // Alpha NEVER and ALWAYS provide deterministic rejection/acceptance. The
  // remaining comparisons and thresholds exercise actual material alpha.
  for(UINT seed=0;seed<(coverageMode?4u:3u);++seed)for(UINT function=0;function<8;++function)for(float threshold:{0.f,.5f,1.f})for(bool valid:{false,true}){
    std::array<float,1024> pixelConstants{};for(UINT i=0;i<224*4;++i)pixelConstants[i]=.15f+float((i*17+seed*31)%37)/50.f;
    if(coverageMode && seed==3)pixelConstants[45*4]=0; // Actual UI shader RGB scale: black additive no-op.
    std::array<unsigned char,768> sharedConstants{};auto setFloat=[&](UINT byte,float value){std::memcpy(sharedConstants.data()+byte,&value,4);};
    setFloat(556,threshold);setFloat(584,1);setFloat(588,1);setFloat(592,1);setFloat(596,1);
    std::memcpy(sharedConstants.data()+600,&function,4);for(UINT i=0;i<4;++i)setFloat(608+i*4,1);
    const float validity=valid?1.f:0.f;upload(current.Get(),&validity,4);upload(pixel.Get(),pixelConstants.data(),4096);upload(shared.Get(),sharedConstants.data(),768);
    const auto a=draw(original.Get()),b=draw(companion.Get());unsigned covered=0;
    for(UINT i=0;i<width*height;++i){const bool ca=a[2][i]!=1,cb=b[2][i]!=1;
      if(ca!=cb || std::memcmp(&a[2][i],&b[2][i],4)){std::fprintf(stderr,"coverage/depth seed=%u func=%u threshold=%g valid=%u pixel=%u original=%g companion=%g\n",seed,function,threshold,valid,i,a[2][i],b[2][i]);return 23;}
      if(!ca){++discardedTotal;continue;}++covered;++coveredTotal;
      if(coverageMode){
        const float* color=&a[0][i*4];
        const float energy=std::fmax(std::abs(color[0]),std::fmax(std::abs(color[1]),std::abs(color[2])));
        float contribution=1;
        if(coverageMode==3)contribution=color[3];
        if(coverageMode==4)contribution=energy*std::abs(color[3]);
        if(coverageMode==5)contribution=energy;
        if(coverageMode==6)contribution=std::fmax(energy,std::abs(color[3]));
        contribution=std::fmax(.15f,std::fmin(1.f,std::fmax(0.f,contribution)));
        REQUIRE(b[0][i*4]==.125f && b[0][i*4+1]==-.25f);
        if(std::abs(b[1][i*4]-contribution)>1e-5f){std::fprintf(stderr,"contribution mode=%u seed=%u pixel=%u got=%g expected=%g\n",coverageMode,seed,i,b[1][i*4],contribution);return 23;}
        continue;
      }
      const float expected[]{valid?.125f:0.f,valid?.0625f:0.f,valid?0.f:1.f};
      const float actual[]{b[0][i*4],b[0][i*4+1],b[1][i*4]};
      for(UINT c=0;c<3;++c)if(!std::isfinite(actual[c])||std::abs(actual[c]-expected[c])>1e-5f){std::fprintf(stderr,"motion/mask case=%u pixel=%u channel=%u actual=%g expected=%g\n",cases,i,c,actual[c],expected[c]);return 23;}
    }
    if(alphaTest&&function==0)REQUIRE(covered==0);
    if(!alphaTest||function==7)REQUIRE(covered==width*height);
    ++cases;
  }
  REQUIRE(coveredTotal>0);if(alphaTest)REQUIRE(discardedTotal>0);
  if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
    SIZE_T size=0;HR(messages->GetMessage(i,nullptr,&size));std::vector<char> storage(size);
    auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());HR(messages->GetMessage(i,message,&size));
    if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12 validation: %s\n",message->pDescription);return 23;}}
  CloseHandle(event);std::printf("PASS cases=%u covered_pixels=%u discarded_pixels=%u exact_depth_and_coverage=1 motion_and_mask=1 debug_layer=%u\n",cases,coveredTotal,discardedTotal,bool(debug));
}
