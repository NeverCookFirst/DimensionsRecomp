// Windowless test of the actual generated companion shaders. No game launch.
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <vector>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static void checkAt(HRESULT h,int line) { if (FAILED(h)) { printf("HRESULT=%08X line=%d\n",unsigned(h),line); std::exit(2); } }
#define check(value) checkAt((value),__LINE__)
static std::vector<char> read(const char* path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);
  if(!f)std::exit(3); auto size=f.tellg(); std::vector<char> b(size);
  f.seekg(0);f.read(b.data(),size);return b;
}
int main(int argc,char** argv) {
  if(argc!=5 && argc!=6 && argc!=7)return 1;
  const bool realVertex=argc>=6;
  const bool captured=argc==7;
  const bool pairDepth=realVertex && std::strcmp(argv[5],"pair")==0;
  const bool lodTest=realVertex && std::strncmp(argv[5],"lod",3)==0;
  const bool textureVertex=realVertex && std::strncmp(argv[5],"texture-real",12)==0;
  // The translated DEC3N specialization deliberately decodes tangent.w as0.
  // Only the ordinary float tangent preserves the supplied wind weight.
  const bool packedTangent=textureVertex && std::strcmp(argv[5],"texture-real-packed")==0;
  const bool textureResources=lodTest||textureVertex;
  const float instructionBias=lodTest?std::strtof(argv[5]+3,nullptr):0;
  const UINT width=captured?1280:128,height=captured?720:96;
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapters1(0,&adapter));
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> alloc;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
  ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
  ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 3;UINT64 serial=0;
  auto submit=[&]{check(list->Close());ID3D12CommandList* l[]={list.Get()};queue->ExecuteCommandLists(1,l);
    check(queue->Signal(fence.Get(),++serial));check(fence->SetEventOnCompletion(serial,event));
    if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)std::exit(4);};
  auto begin=[&]{check(alloc->Reset());check(list->Reset(alloc.Get(),nullptr));};
  auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;};
  auto texture=[&](DXGI_FORMAT format,bool depth){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;
    d.Flags=depth?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
      depth?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&r)));return r;};
  auto velocity=texture(DXGI_FORMAT_R32G32_FLOAT,false),reactive=texture(DXGI_FORMAT_R32_FLOAT,false),depth=texture(DXGI_FORMAT_D32_FLOAT,true);
  auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
  ComPtr<ID3D12DescriptorHeap> rtvs;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)));
  auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart(),rtv1=rtv;
  rtv1.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  device->CreateRenderTargetView(velocity.Get(),nullptr,rtv);device->CreateRenderTargetView(reactive.Get(),nullptr,rtv1);
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
  ComPtr<ID3D12DescriptorHeap> dsvs;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsvs)));
  auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();device->CreateDepthStencilView(depth.Get(),nullptr,dsv);
  D3D12_ROOT_PARAMETER params[6]{};
  for(UINT i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister=i==3?4:i;params[i].Descriptor.RegisterSpace=4;params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,32,0,0,0};
  ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,32,0,3,0};
  for(UINT i=0;i<2;++i){params[4+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4+i].DescriptorTable={1,&ranges[i]};}
  D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=textureResources?6:4;root.pParameters=params;
  root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
  ComPtr<ID3D12RootSignature> signature;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&signature)));
  auto makePipeline=[&](const char* vsPath,const char* psPath){
    auto vs=read(vsPath),ps=read(psPath);D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature=signature.Get();p.VS={vs.data(),vs.size()};p.PS={ps.data(),ps.size()};
    std::vector<D3D12_INPUT_ELEMENT_DESC> input={ {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0} };
    if(realVertex){
      for(UINT i=1;i<5;++i)input.push_back({"POSITION",i,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
      input.push_back({"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
      input.push_back({"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
      for(UINT i=0;i<2;++i)input.push_back({"COLOR",i,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
      for(UINT i=0;i<16;++i)input.push_back({"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0});
    }
    p.InputLayout={input.data(),UINT(input.size())};p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable=TRUE;p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=2;
    p.RTVFormats[0]=DXGI_FORMAT_R32G32_FLOAT;p.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    for(auto& rt:p.BlendState.RenderTarget)rt.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));return pipeline;};
  auto normal=makePipeline(argv[1],argv[2]),motion=makePipeline(argv[3],argv[4]);
  const float verts[]={-0.8f,-0.8f,0.4f,1, 0.8f,-0.8f,0.4f,1, -0.8f,0.8f,0.4f,1,
                        0.8f,-0.8f,0.4f,1, 0.8f,0.8f,0.4f,1, -0.8f,0.8f,0.4f,1};
  std::vector<char> capturedData; UINT vertexCount=6;
  if(captured){capturedData=read(argv[6]); if(capturedData.size()<4096 || (capturedData.size()-4096)%16)return 11; vertexCount=UINT((capturedData.size()-4096)/16);}
  const UINT vertexBytes=vertexCount*16;
  auto vb=buffer(vertexBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  void* mapped;D3D12_RANGE empty{0,0};check(vb->Map(0,&empty,&mapped));std::memcpy(mapped,captured?capturedData.data()+4096:static_cast<const void*>(verts),vertexBytes);vb->Unmap(0,nullptr);
  auto nullAttributes=buffer(16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  check(nullAttributes->Map(0,&empty,&mapped));std::memset(mapped,0,16);
  if(textureVertex)static_cast<float*>(mapped)[3]=1; // Nonzero wind weight.
  nullAttributes->Unmap(0,nullptr);
  auto current=buffer(4096,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto previous=buffer(4352,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto pixel=buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto shared=buffer(768,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  check(shared->Map(0,&empty,&mapped));std::memset(mapped,0,768);shared->Unmap(0,nullptr);
  ComPtr<ID3D12DescriptorHeap> samples,samplers;
  ComPtr<ID3D12Resource> lodTexture,lodUpload;
  D3D12_SAMPLER_DESC lodSampler{};
  if(textureResources){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=32;td.Height=32;td.DepthOrArraySize=1;td.MipLevels=6;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&lodTexture)));
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,6> layouts{};UINT64 bytes;
    device->GetCopyableFootprints(&td,0,6,0,layouts.data(),nullptr,nullptr,&bytes);
    lodUpload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    check(lodUpload->Map(0,&empty,&mapped));
    for(UINT mip=0;mip<6;++mip)for(UINT y=0;y<layouts[mip].Footprint.Height;++y){
      auto* row=reinterpret_cast<float*>(static_cast<char*>(mapped)+layouts[mip].Offset+y*layouts[mip].Footprint.RowPitch);
      for(UINT x=0;x<layouts[mip].Footprint.Width;++x)for(UINT c=0;c<4;++c)row[x*4+c]=0.1f*(mip+1);
    }
    lodUpload->Unmap(0,nullptr);begin();
    for(UINT mip=0;mip<6;++mip){D3D12_TEXTURE_COPY_LOCATION src{},dst{};
      src.pResource=lodUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=layouts[mip];
      dst.pResource=lodTexture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=mip;
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
    transition(lodTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);submit();
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=32;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samples)));device->CreateShaderResourceView(lodTexture.Get(),nullptr,samples->GetCPUDescriptorHandleForHeapStart());
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samplers)));
    lodSampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    lodSampler.AddressU=lodSampler.AddressV=lodSampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;lodSampler.MaxLOD=5;
    lodSampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&lodSampler,samplers->GetCPUDescriptorHandleForHeapStart());
  }
  const auto footprint=[&](ID3D12Resource* r,D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp){auto d=r->GetDesc();UINT64 bytes;device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);return bytes;};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,3> fps{};
  auto vr=buffer(footprint(velocity.Get(),fps[0]),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto rr=buffer(footprint(reactive.Get(),fps[1]),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto dr=buffer(footprint(depth.Get(),fps[2]),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto draw=[&](ID3D12PipelineState* pipeline){
    begin();float cleared[]={9,9,9,9};list->ClearRenderTargetView(rtv,cleared,0,nullptr);list->ClearRenderTargetView(rtv1,cleared,0,nullptr);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    list->OMSetRenderTargets(1,&rtv,TRUE,&dsv);D3D12_CPU_DESCRIPTOR_HANDLE targets[]={rtv,rtv1};list->OMSetRenderTargets(2,targets,FALSE,&dsv);
    list->SetGraphicsRootSignature(signature.Get());list->SetPipelineState(pipeline);
    list->SetGraphicsRootConstantBufferView(0,current->GetGPUVirtualAddress());list->SetGraphicsRootConstantBufferView(1,pixel->GetGPUVirtualAddress());
    list->SetGraphicsRootConstantBufferView(2,shared->GetGPUVirtualAddress());
    list->SetGraphicsRootConstantBufferView(3,previous->GetGPUVirtualAddress());
    if(textureResources){ID3D12DescriptorHeap* heaps[]={samples.Get(),samplers.Get()};list->SetDescriptorHeaps(2,heaps);
      list->SetGraphicsRootDescriptorTable(4,samples->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootDescriptorTable(5,samplers->GetGPUDescriptorHandleForHeapStart());}
    D3D12_VIEWPORT view{0,0,float(width),float(height),0,1};D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&view);list->RSSetScissorRects(1,&scissor);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW v[]={ {vb->GetGPUVirtualAddress(),vertexBytes,16},
      {nullAttributes->GetGPUVirtualAddress(),16,0} };
    list->IASetVertexBuffers(0,realVertex?2:1,v);list->DrawInstanced(vertexCount,1,0,0);
    ID3D12Resource* sources[]={velocity.Get(),reactive.Get(),depth.Get()},*destinations[]={vr.Get(),rr.Get(),dr.Get()};
    for(UINT i=0;i<3;++i){auto state=i==2?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
      transition(sources[i],state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
      src.pResource=sources[i];src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=destinations[i];dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fps[i];
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,state);}
    submit();
    std::array<std::vector<float>,3> data;
    for(UINT i=0;i<3;++i){check(destinations[i]->Map(0,nullptr,&mapped));const UINT channels=i?1:2;data[i].resize(width*height*channels);
      for(UINT y=0;y<height;++y)std::memcpy(data[i].data()+y*width*channels,static_cast<char*>(mapped)+fps[i].Offset+y*fps[i].Footprint.RowPitch,width*channels*4);
      destinations[i]->Unmap(0,nullptr);}
    return data;};
  auto upload=[&](ID3D12Resource* resource,const void* data,size_t size){check(resource->Map(0,&empty,&mapped));std::memcpy(mapped,data,size);resource->Unmap(0,nullptr);};
  if(lodTest){
    std::array<float,1024> curr{};for(UINT i=0;i<4;++i)curr[i*4+i]=1;upload(current.Get(),curr.data(),sizeof(curr));
    std::array<float,64> p{};std::array<float,192> s{};for(UINT i=152;i<156;++i)s[i]=1;
    for(float lod:{0.0f,2.0f,4.0f})for(float bias:{-1.0f,0.0f,1.0f}){
      // Previous draw's fence completed before editing this fixture descriptor.
      lodSampler.MipLODBias=bias;device->CreateSampler(&lodSampler,samplers->GetCPUDescriptorHandleForHeapStart());
      p[2]=lod;s[159]=bias;upload(pixel.Get(),p.data(),sizeof(p));upload(shared.Get(),s.data(),sizeof(s));
      auto generated=draw(motion.Get());UINT coverage=0;const float expected=0.1f*(std::clamp(lod+bias+instructionBias,0.0f,5.0f)+1);
      for(UINT i=0;i<width*height;++i){if(generated[2][i]==1)continue;++coverage;
        if(std::abs(generated[0][2*i]-expected)>0.000001f){printf("LOD mismatch register=%.1f fetch_bias=%.1f got=%.6f expected=%.6f\n",lod,bias,generated[0][2*i],expected);return 10;}}
      if(coverage<1000)return 8;printf("PASS LOD register=%.1f fetch_bias=%.1f pixels=%u\n",lod,bias,coverage);
    }
    CloseHandle(event);return 0;
  }
  if(captured){
    upload(current.Get(),capturedData.data(),4096);
    std::array<float,192> sharedData{};
    sharedData[137]=-1.0f/1280; sharedData[138]=1.0f/720;
    sharedData[148]=sharedData[149]=1;
    upload(shared.Get(),sharedData.data(),sizeof(sharedData));
    auto a=draw(normal.Get()),b=draw(motion.Get());
    UINT covered=0,mismatch=0,coverageDifference=0;
    for(UINT i=0;i<width*height;++i){
      bool ca=a[2][i]!=1,cb=b[2][i]!=1;
      coverageDifference+=ca!=cb;covered+=ca||cb;
      mismatch+=std::memcmp(&a[2][i],&b[2][i],4)!=0;
    }
    printf("CAPTURE vertices=%u covered=%u depth_mismatches=%u coverage_differences=%u\n",vertexCount,covered,mismatch,coverageDifference);
    CloseHandle(event);return mismatch?9:0;
  }
  if(pairDepth){
    // Shader pairs see identical geometry/constants. Large world translations
    // cancelled by the view matrix exercise real floating-point reassociation.
    UINT random=0x19597A31,total=0,mismatches=0,coverageDifferences=0;
    auto sample=[&]{random^=random<<13;random^=random>>17;random^=random<<5;
      return float(random&0xFFFFFF)/float(0x1000000)*2-1;};
    for(UINT trial=0;trial<128;++trial){
      std::array<float,1024> curr{};
      for(UINT i=0;i<3;++i)curr[(48+i)*4+i]=0.8f+sample()*0.1f;
      curr[51*4+3]=1;
      for(UINT i=0;i<3;++i)curr[51*4+i]=sample()*256;
      for(UINT row=0;row<3;++row)for(UINT col=0;col<3;++col)
        curr[row*4+col]=(row==col?1.0f:0.0f)+sample()*0.02f;
      curr[15]=1;
      for(UINT col=0;col<3;++col){
        curr[12+col]=-(curr[51*4]*curr[col]+curr[51*4+1]*curr[4+col]+curr[51*4+2]*curr[8+col]);
      }
      upload(current.Get(),curr.data(),sizeof(curr));
      auto first=draw(normal.Get()),second=draw(motion.Get());
      for(UINT i=0;i<width*height;++i){
        const bool coveredA=first[2][i]!=1,coveredB=second[2][i]!=1;
        coverageDifferences+=coveredA!=coveredB;
        if(!coveredA && !coveredB)continue;
        ++total;
        if(std::memcmp(&first[2][i],&second[2][i],4)){
          if(mismatches<4)printf("pair depth mismatch trial=%u pixel=%u a=%.9g b=%.9g\n",trial,i,first[2][i],second[2][i]);
          ++mismatches;
        }
      }
    }
    CloseHandle(event);
    printf("PAIR trials=128 covered=%u depth_mismatches=%u coverage_differences=%u\n",total,mismatches,coverageDifferences);
    return total<1000?8:mismatches?9:0;
  }
  struct Case { const char* name;float camera,object,wind;bool valid;float alpha;};
  const Case cases[]={ {"static",0,0,0,true,-1}, {"camera",0.25f,0,0,true,-1},
    {"object",0,0.125f,0,true,-1}, {"wind",0,0,0.25f,true,-1},
    {"alpha-cutout",0,0.125f,0,true,0}, {"new-object",0,0.125f,0,false,-1},
    {"view-only",0,0,0,true,-1} };
  for(UINT trial=0;trial<(textureVertex?128u:1u);++trial){
  for(auto& c:cases){
    if(realVertex && c.wind && !textureVertex)continue;
    if(!textureVertex && std::strcmp(c.name,"view-only")==0)continue;
    alignas(16) std::array<float,1088> prev{};std::array<float,1024> curr{};
    for(UINT i=0;i<4;++i)curr[i*4+i]=prev[i*4+i]=1;
    if(realVertex)for(UINT i=0;i<4;++i)curr[(48+i)*4+i]=prev[(48+i)*4+i]=1;
    if(textureVertex){
      const float angle=trial*.037f,sx=.65f+.1f*std::sin(angle),sy=.65f+.1f*std::cos(angle);
      curr[48*4]=prev[48*4]=std::cos(angle)*sx;curr[48*4+1]=prev[48*4+1]=std::sin(angle)*sx;
      curr[49*4]=prev[49*4]=-std::sin(angle)*sy;curr[49*4+1]=prev[49*4+1]=std::cos(angle)*sy;
      curr[50*4+2]=prev[50*4+2]=.65f+.1f*std::sin(angle);
      curr[51*4]=prev[51*4]=.05f*std::sin(angle);curr[51*4+1]=prev[51*4+1]=.05f*std::cos(angle);
      curr[51*4+2]=prev[51*4+2]=.2f;
      // Both static noise textures contain0.1 in every channel at mip0.
      // Their sampled sum is0.2; c19.y times that sum displaces in c22.xyz.
      curr[22*4]=prev[22*4]=curr[22*4+3]=prev[22*4+3]=1;
      prev[19*4+1]=c.wind;
      if(std::strcmp(c.name,"view-only")==0)prev[4*4]=1.25f;
    }
    prev[12]=c.camera;prev[realVertex?51*4:16]+=c.object;
    if(!textureVertex)prev[20]=c.wind;
    const UINT valid=c.valid;std::memcpy(prev.data()+1024,&valid,4);
    upload(current.Get(),curr.data(),sizeof(curr));upload(previous.Get(),prev.data(),sizeof(prev));
    float alpha[64]{};alpha[0]=c.alpha;upload(pixel.Get(),alpha,sizeof(alpha));
    auto original=draw(normal.Get()),generated=draw(motion.Get());UINT coverage=0;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const UINT i=y*width+x;
      // Depth and cutout coverage must stay bit-identical to the normal entry.
      if(std::memcmp(&original[2][i],&generated[2][i],4)){printf("depth mismatch %s %u,%u\n",c.name,x,y);return 5;}
      if(original[2][i]==1){if(generated[0][2*i]!=9||generated[1][i]!=9)return 6;continue;}
      // Baseline exposes the interpolated original vertex Y. This includes
      // hardware subpixel snapping; an ideal pixel-center formula does not.
      ++coverage;const float py=original[0][2*i+1];
      const float expected=c.valid?0.5f*(c.camera+c.object+c.wind*(textureVertex?(packedTangent?0.0f:.2f):py)):0;
      if(!std::isfinite(generated[0][2*i])||std::abs(generated[0][2*i]-expected)>0.000002f||
         std::abs(generated[0][2*i+1])>0.000002f||generated[1][i]!=(c.valid?0:1)){
        printf("motion mismatch %s %u,%u got=%f expected=%f mask=%f\n",c.name,x,y,generated[0][2*i],expected,generated[1][i]);return 7;}
    }
    if(coverage<1000)return 8;
    if(trial==0)printf("PASS %s pixels=%u depth/alpha unchanged\n",c.name,coverage);
  }
  }
  if(textureVertex)printf("PASS textured world profile:128 affine transforms, decoded wind weight=%u, camera/object/wind/view changes\n",packedTangent?0u:1u);
  CloseHandle(event);printf("PASS current/previous vertex evaluation, no game/window/frame generation\n");return 0;
}
