#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <bit>
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h,int line){if(FAILED(h)){printf("HRESULT %08X line=%d\n",unsigned(h),line);std::exit(2);}}
#define HR(h) Hr(h,__LINE__)
std::vector<char> Read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)std::exit(3);std::vector<char>b(size_t(f.tellg()));f.seekg(0);f.read(b.data(),b.size());return b;}
int main(int argc,char**argv){
 if(argc<2||argc>3)return 1;const bool expect_mismatch=argc==3&&std::string(argv[2])=="--expect-mismatch";std::filesystem::path folder=argv[1];std::ifstream index(folder/"index.txt");
 std::vector<std::string> names;std::string line;while(std::getline(index,line))if(!line.empty())names.push_back(line);
 ComPtr<ID3D12Device>d;HR(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12CommandQueue>q;D3D12_COMMAND_QUEUE_DESC qd{};HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
 ComPtr<ID3D12CommandAllocator>a;HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
 ComPtr<ID3D12GraphicsCommandList>l;HR(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));HR(l->Close());
 D3D12_ROOT_PARAMETER params[4]{};for(int i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor={UINT(i*2),4};params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
 D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,UINT_MAX,0,3,0}};
 for(unsigned i=2;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i-2]};params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
 D3D12_ROOT_SIGNATURE_DESC rd{4,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
 ComPtr<ID3DBlob>blob,err;HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err));
 ComPtr<ID3D12RootSignature>root;HR(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
 auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;
  D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=size;b.Height=b.DepthOrArraySize=b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource>r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,state,nullptr,IID_PPV_ARGS(&r)));return r;};
 constexpr unsigned N=2048;struct Vertex{float position[4],normal[4],tangent[4],uv0[4],uv1[4],color[4];};
 auto vb=buffer(sizeof(Vertex)*N,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto cb=buffer(4096,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ),shared=buffer(768,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 float* constants;void* mapped;HR(cb->Map(0,nullptr,reinterpret_cast<void**>(&constants)));std::memset(constants,0,4096);
 void* shared_data;HR(shared->Map(0,nullptr,&shared_data));std::memset(shared_data,0,768);*reinterpret_cast<float*>(static_cast<char*>(shared_data)+608)=1;
 Vertex* vertices;HR(vb->Map(0,nullptr,reinterpret_cast<void**>(&vertices)));
 uint32_t random=0x12345678;auto sample=[&]{random=random*1664525u+1013904223u;return float(int32_t(random>>8)-0x800000)/8388608.f;};
 for(unsigned i=0;i<N;++i){auto&v=vertices[i];for(float&x:v.position)x=sample()*1000;v.position[3]=1;
  for(float&x:v.normal)x=.5f+.3f*sample();v.normal[3]=1;for(float&x:v.tangent)x=.5f+.3f*sample();v.tangent[3]=.6f+.4f*sample();
  for(float&x:v.uv0)x=.5f+.5f*sample();v.uv0[2]=v.uv0[0];v.uv0[3]=v.uv0[1];std::memcpy(v.uv1,v.uv0,16);for(float&x:v.color)x=1;}
 auto stream=buffer(N*16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_STREAM_OUT);
 auto counter=buffer(8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto zero=buffer(8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);HR(zero->Map(0,nullptr,&mapped));std::memset(mapped,0,8);zero->Unmap(0,nullptr);
 auto readback=buffer(N*16+8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 ComPtr<ID3D12Fence>fence;HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
 ComPtr<ID3D12DescriptorHeap>views,samplers,rtvs;D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};HR(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&views)));
 heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;HR(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&samplers)));
 heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;HR(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvs)));
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=td.Height=16;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 ComPtr<ID3D12Resource>height;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&height)));
 D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;d->CreateShaderResourceView(height.Get(),&srv,views->GetCPUDescriptorHandleForHeapStart());
 d->CreateRenderTargetView(height.Get(),nullptr,rtvs->GetCPUDescriptorHandleForHeapStart());
 D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.MaxLOD=FLT_MAX;sampler.MaxAnisotropy=1;d->CreateSampler(&sampler,samplers->GetCPUDescriptorHandleForHeapStart());
 HR(a->Reset());HR(l->Reset(a.Get(),nullptr));const float heights[]{.125f,.3f,.55f,.8f};l->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(),heights,0,nullptr);
 D3D12_RESOURCE_BARRIER hb{};hb.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;hb.Transition={height.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};l->ResourceBarrier(1,&hb);HR(l->Close());ID3D12CommandList*init[]{l.Get()};q->ExecuteCommandLists(1,init);HR(q->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));WaitForSingleObject(event,30000);
 D3D12_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  {"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
  {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
 D3D12_SO_DECLARATION_ENTRY declaration{0,"SV_Position",0,0,4,0};UINT stride=16;
 std::vector<ComPtr<ID3D12PipelineState>>pipelines;
 for(auto&name:names){auto vs=Read(folder/(name+".dxil"));D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.VS={vs.data(),vs.size()};
  p.StreamOutput={&declaration,1,&stride,1,D3D12_SO_NO_RASTERIZED_STREAM};p.InputLayout={elements,UINT(std::size(elements))};
  p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.SampleMask=UINT_MAX;
  p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;p.SampleDesc.Count=1;ComPtr<ID3D12PipelineState>pipeline;
  const auto h=d->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline));if(FAILED(h))printf("shader=%s\n",name.c_str());HR(h);pipelines.push_back(std::move(pipeline));}
 auto transition=[&](ID3D12Resource*r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};l->ResourceBarrier(1,&b);};
 std::vector<std::array<float,4>>reference(N);uint64_t unequal=0,finite=0,nonzero=0;std::vector<uint64_t>differences(names.size());
 for(unsigned scene=0;scene<32;++scene){
  for(unsigned i=0;i<16;++i)constants[i]=sample();
  for(unsigned i=0;i<16;++i)constants[48*4+i]=sample();
  constants[19*4]=.1f;constants[19*4+1]=2;constants[19*4+2]=constants[19*4+3]=4;
  constants[22*4]=.1f;constants[22*4+1]=.3f;constants[22*4+2]=.2f;constants[22*4+3]=.5f;
  for(unsigned i=23;i<=28;i+=2){constants[i*4]=1;constants[(i+1)*4+1]=1;}
  auto* shared_bytes=static_cast<char*>(shared_data);
  *reinterpret_cast<float*>(shared_bytes+592)=2.f/1920;*reinterpret_cast<float*>(shared_bytes+596)=-2.f/1080;
  *reinterpret_cast<uint32_t*>(shared_bytes+604)=scene%2;
  *reinterpret_cast<float*>(shared_bytes+548)=scene%4>=2?1.f/1920:0;
  *reinterpret_cast<float*>(shared_bytes+552)=scene%4>=2?-1.f/1080:0;
  // Vary actual GPU texture contents as well as the material interpolation.
  HR(a->Reset());HR(l->Reset(a.Get(),nullptr));transition(height.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){D3D12_RECT rect{LONG(x),LONG(y),LONG(x+1),LONG(y+1)};
   const float h[]{float(x+scene%5)/24,float(y)/17,float(x^y)/16,float(x+y)/32};l->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(),h,1,&rect);}
  transition(height.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);HR(l->Close());ID3D12CommandList*texture_lists[]{l.Get()};q->ExecuteCommandLists(1,texture_lists);HR(q->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return 4;
  // One common camera/world transform; auxiliary material constants differ only in outputs we do not capture.
  for(unsigned j=0;j<names.size();++j){HR(a->Reset());HR(l->Reset(a.Get(),nullptr));
   l->CopyBufferRegion(counter.Get(),0,zero.Get(),0,8);transition(counter.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT);
   l->SetGraphicsRootSignature(root.Get());l->SetGraphicsRootConstantBufferView(0,cb->GetGPUVirtualAddress());l->SetGraphicsRootConstantBufferView(1,shared->GetGPUVirtualAddress());
   ID3D12DescriptorHeap*heaps[]{views.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);l->SetGraphicsRootDescriptorTable(2,views->GetGPUDescriptorHandleForHeapStart());l->SetGraphicsRootDescriptorTable(3,samplers->GetGPUDescriptorHandleForHeapStart());
   l->SetPipelineState(pipelines[j].Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
   D3D12_VERTEX_BUFFER_VIEW v{vb->GetGPUVirtualAddress(),sizeof(Vertex)*N,sizeof(Vertex)};l->IASetVertexBuffers(0,1,&v);
   D3D12_STREAM_OUTPUT_BUFFER_VIEW so{stream->GetGPUVirtualAddress(),N*16,counter->GetGPUVirtualAddress()};l->SOSetTargets(0,1,&so);l->DrawInstanced(N,1,0,0);
   transition(stream.Get(),D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(readback.Get(),0,stream.Get(),0,N*16);
   transition(stream.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_STREAM_OUT);transition(counter.Get(),D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE);
   l->CopyBufferRegion(readback.Get(),N*16,counter.Get(),0,8);transition(counter.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
   HR(l->Close());ID3D12CommandList*lists[]{l.Get()};q->ExecuteCommandLists(1,lists);HR(q->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return 4;
   const std::array<float,4>*data;HR(readback->Map(0,nullptr,reinterpret_cast<void**>(const_cast<std::array<float,4>**>(&data))));
   if(*reinterpret_cast<const uint64_t*>(data+N)!=N*16)return 26;
   for(unsigned n=0;n<N;++n)for(int c=0;c<4;++c){if(!std::isfinite(data[n][c]))return 25;++finite;if(data[n][c]!=0)++nonzero;
    if(j&&std::bit_cast<uint32_t>(data[n][c])!=std::bit_cast<uint32_t>(reference[n][c])){++unequal;++differences[j];}}
   if(!j)std::memcpy(reference.data(),data,N*16);
   readback->Unmap(0,nullptr);
  }
 }
 for(unsigned i=0;i<names.size();++i)printf("%s position_component_differences=%llu\n",names[i].c_str(),static_cast<unsigned long long>(differences[i]));
 printf("executed VS=%zu scenes=32 vertices=%u finite_components=%llu nonzero_components=%llu component_differences=%llu\n",names.size(),N,static_cast<unsigned long long>(finite),static_cast<unsigned long long>(nonzero),static_cast<unsigned long long>(unequal));CloseHandle(event);
 return names.size()<2||!nonzero||((unequal!=0)!=expect_mismatch)?23:0;
}
