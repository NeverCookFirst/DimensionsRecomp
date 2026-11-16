// Windowless differential oracle. The shader is supplied from the owner's
// existing ReXGlue dump; no game shader or generated CPU code is embedded here.
#define NOMINMAX
#include <plume_d3d12.h>
#include <wrl/client.h>
#include <immintrin.h>
#include "gpu_native/pool_copy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u8=uint8_t;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
#define HR(c) CHECK(SUCCEEDED(c))
#define REXLOG_ERROR(...) ((void)0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}

// The production coordinator body is extracted by the Python driver. Only
// surrounding registries/queues are substituted; its actual byte publication
// and access checks execute on the same input bytes as the reference GPU VS.
struct Memory {std::vector<u8> data;u8* TranslatePhysical(u32 at){return data.data()+at;}} memory;
#define REX_KERNEL_MEMORY() (&::memory)
struct Batch {bool active=false,waited=false;std::vector<PhysicalCopyRange>textures,buffers,shaders;} g_pool_batch;
void BeginPoolCopyBatch(){g_pool_batch.active=true;}
void legodimensions::gpu_native::FlushPoolCopyBatch(){g_pool_batch={};}
bool BatchAccessible(PhysicalCopyRange r,bool){return uint64_t(r.address)+r.length<=memory.data.size();}
bool PoolCopyMayTouchTextures(PhysicalCopyRange){return false;}
bool PoolCopyMayTouchBuffers(PhysicalCopyRange){return true;}
bool PoolCopyMayTouchShaders(PhysicalCopyRange){return false;}
bool PoolCopyGpuSourceIndexed(PhysicalCopyRange){return false;}
bool PoolCopyGpuDestinationIndexed(PhysicalCopyRange){return false;}
bool MaterializePoolCopyTextureSource(PhysicalCopyRange){return false;}
bool MaterializePoolCopyTextureDestination(PhysicalCopyRange){return true;}
bool MaterializePoolCopyTextureDependencies(PhysicalCopyRange,PhysicalCopyRange){return true;}
void WaitAllPreparation(){}
void NativeRecorderSubmit(std::function<void()> fn,bool,bool){fn();}
u32 NativeRecorderBarrier(){return 0;}
void NativeRecorderWaitExecuted(u32){}
void InvalidateCpuPhysicalMemory(u32,u32){}
#include "pool-copy-recorded.inc"
#include "reference-merge.inc"

ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,uint64_t bytes,D3D12_HEAP_TYPE heap,
                            D3D12_RESOURCE_STATES state,bool uav=false){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;
 D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;
 rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(uav)rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&barrier);
}
int main(int argc,char**argv){
 CHECK(argc==2||argc==3);
 const bool omit_endian=argc==3&&std::strcmp(argv[2],"omit-export-endian")==0;
 const bool omit_merge=argc==3&&std::strcmp(argv[2],"omit-merge")==0;
 std::ifstream in(argv[1],std::ios::binary);std::vector<char> vs{std::istreambuf_iterator<char>(in),{}};
 CHECK(!vs.empty());
 auto api=plume::CreateD3D12Interface();auto render=api->createDevice();CHECK(render);
 auto*device=static_cast<plume::D3D12Device*>(render.get())->d3d;
 auto queue=render->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=render->createCommandFence();
 auto*list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
 auto finish=[&]{commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());};
 std::array<D3D12_ROOT_PARAMETER,5> rp{};
 rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;rp[0].Descriptor.ShaderRegister=0;
 rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;rp[1].Descriptor.ShaderRegister=0;
 for(u32 i=2;i<5;++i){rp[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[i].Descriptor.ShaderRegister=i==2?0:i==3?3:1;}
 D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=rp.size();rs.pParameters=rp.data();
 ComPtr<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
 ComPtr<ID3D12RootSignature> root;HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
 D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs.data(),vs.size()};
 pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
 pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
 pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
 ComPtr<ID3D12PipelineState> pipeline;HR(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline)));
 constexpr u32 capacity=8u<<20;
 auto gpu=Buffer(device,capacity,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,true);
 auto upload=Buffer(device,capacity,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto readback=Buffer(device,capacity,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 auto cb=Buffer(device,1u<<20,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 void*up=nullptr;void*constants=nullptr;D3D12_RANGE no_read{0,0};
 HR(upload->Map(0,&no_read,&up));HR(cb->Map(0,&no_read,&constants));
 std::vector<u8> initial(capacity);std::mt19937 random(0x9e10);
 for(auto&b:initial)b=u8(random());
 // Include all 16-bit values: the patched reference fetch/export pair uses
 // unsigned integer 16_16_16_16 and must preserve every bit pattern.
 for(u32 i=0;i<65536;++i){const uint16_t v=uint16_t(i);std::memcpy(initial.data()+i*2,&v,2);}
 struct Case {u32 source,destination,bytes;bool overlap;};
 const Case cases[]={{0,2u<<20,64,false},{64,(2u<<20)+64,4096,false},
  {0,2u<<20,1u<<20,false},{0,2u<<20,(1u<<20)+64,false},
  {4096,8192,65536,true},{4096,4160,4096,true},
  {4096,4096+128,65536,true},{128,2u<<20,131072,false}};
 u32 completed=0,draws=0;
 bool first=true;
 for(const auto&c:cases){
  memory.data=initial;g_pool_batch={};std::vector<u8> expected=initial;
  std::memmove(expected.data()+c.destination,expected.data()+c.source,c.bytes);
  std::memcpy(up,initial.data(),initial.size());std::memset(constants,0,1u<<20);
  commands->begin();list=static_cast<plume::D3D12CommandList*>(commands.get())->d3d;
  if(!first)Transition(list,gpu.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);first=false;
  list->CopyBufferRegion(gpu.Get(),0,upload.Get(),0,capacity);
  Transition(list,gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(pipeline.Get());list->SetGraphicsRootSignature(root.Get());
  list->SetGraphicsRootUnorderedAccessView(0,gpu->GetGPUVirtualAddress());
  list->SetGraphicsRootShaderResourceView(1,upload->GetGPUVirtualAddress());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  u32 slot=0;
  auto draw=[&](u32 src,u32 dst,u32 bytes){
   CHECK((bytes&63)==0);
   // ReXGlue passes StartVertexLocation=0 and applies the guest start through
   // xe_vertex_index_offset. Keep exactly that setup, with one immutable
   // constant block for each of TU23's <=16384-record draws.
   for(u32 at=0;at<bytes/64;){
   const u32 n=std::min(16384u,bytes/64-at);CHECK(slot+2048<=1u<<20);
   auto*base=static_cast<u8*>(constants)+slot;
   auto*system=reinterpret_cast<u32*>(base);auto*fetch=reinterpret_cast<u32*>(base+768);auto*floats=reinterpret_cast<u32*>(base+1536);
   system[0]=1;system[3]=UINT_MAX;system[5]=at;system[7]=UINT_MAX;
   system[32]=system[33]=system[34]=std::bit_cast<u32>(1.0f);
   fetch[190]=src|3u;fetch[191]=((bytes/4)<<2)|2u;
   floats[0]=0x40000000u|(dst>>2);floats[1]=0x4b000000;
   floats[2]=omit_endian?0x4b021a00:0x4b021a01;floats[3]=0x4b000000u|(bytes/8);
   floats[8]=0;floats[9]=std::bit_cast<u32>(1.0f);floats[10]=std::bit_cast<u32>(4.0f);floats[11]=std::bit_cast<u32>(8.0f);
   const auto addr=cb->GetGPUVirtualAddress()+slot;
   list->SetGraphicsRootConstantBufferView(2,addr);list->SetGraphicsRootConstantBufferView(3,addr+768);list->SetGraphicsRootConstantBufferView(4,addr+1536);
   list->DrawInstanced(n,1,0,0);at+=n;++draws;slot+=2048;
   D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=gpu.Get();list->ResourceBarrier(1,&b);
   }
   PoolCopyRecorded(dst,src,bytes,{src,bytes},{dst,bytes});
  };
  if(c.overlap){
   // Actual TU23 compactor: backwards chunks no larger than dst-source.
   for(u32 end=c.bytes;end;){u32 n=std::min(end,c.destination-c.source);end-=n;draw(c.source+end,c.destination+end,n);}
  }else draw(c.source,c.destination,c.bytes);
  Transition(list,gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(),0,gpu.Get(),0,capacity);finish();
  const void*mapped=nullptr;D3D12_RANGE range{0,capacity};HR(readback->Map(0,&range,const_cast<void**>(&mapped)));
  const auto*bytes=static_cast<const u8*>(mapped);
  for(u32 i=0;i<capacity;++i)if(bytes[i]!=expected[i]||bytes[i]!=memory.data[i]){
   std::fprintf(stderr,"case %u byte %u: GPU=%u expected=%u native=%u\n",completed,i,bytes[i],expected[i],memory.data[i]);std::exit(23);
  }
  // Compare the reference snapshot-merge retirement with native's immediate
  // CPU copy when the game makes later byte writes. Values deliberately differ
  // from the snapshot, which is the reference merge's write-detection contract.
  auto reference_guest=initial;
  for(u32 offset:{0u,1u,7u,32u,c.bytes-1}){
   const u32 at=c.destination+offset;const u8 value=initial[at]^0xa5;
   reference_guest[at]=value;memory.data[at]=value;
  }
  const u32 padding=c.destination+c.bytes+4;
  reference_guest[padding]^=0x40;memory.data[padding]^=0x40;
  if(omit_merge)std::memcpy(reference_guest.data()+c.destination,bytes+c.destination,c.bytes);
  else CHECK(MergeMemexportBytes(reference_guest.data()+c.destination,bytes+c.destination,
                                initial.data()+c.destination,c.bytes));
  CHECK(reference_guest==memory.data);
  readback->Unmap(0,&no_read);++completed;
 }
 upload->Unmap(0,&no_read);cb->Unmap(0,&no_read);
 std::printf("PASS saved ReXGlue VS versus production native copy: %u cases, %u GPU draws, exact bytes plus later CPU writes/reference snapshot merge, no window/game\n",completed,draws);
}
