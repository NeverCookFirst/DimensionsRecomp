"""Check fetched-byte equivalence and actual buffer conversion cache behavior."""
import argparse
import json
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/buffers.cpp').read_text()
def body(signature):
 start=source.index(signature);pos=source.index('{',start);end=pos+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
resource=source[source.index('struct BufferResource {'):source.index('std::mutex g_buffers_mutex;')]
h=a.output/'buffer-variants-test.cpp'
h.write_text(r'''
#include <array>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu_native/vertex_byte_order.h"
#include "gpu_native/memory_watch.h"
#include "gpu_native/buffer_header.h"
#include "gpu_native/buffer_window.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using be_u32=uint32_t;
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
enum class BufferKind:u32{kVertex=6,kIndex=7};
namespace plume {
enum class RenderHeapType {UPLOAD};
struct RenderBufferDesc {
 u32 size;static auto IndexBuffer(u32 n,RenderHeapType){return RenderBufferDesc{n};}
 static auto VertexBuffer(u32 n,RenderHeapType){return RenderBufferDesc{n};}
};
struct RenderBuffer {
 std::vector<u8> data; explicit RenderBuffer(u32 size):data(size){}
 void* map(){return data.data();}void unmap(){}void setName(const char*){}
};
struct Device {
 u32 allocations=0;
 std::unique_ptr<RenderBuffer> createBuffer(RenderBufferDesc d){++allocations;return std::make_unique<RenderBuffer>(d.size);}
};
}
struct HostDevice {
 static bool IsReady(){return true;}
 static inline plume::Device device;
 static inline std::vector<std::shared_ptr<void>> held;
 static auto Device(){return &device;}
 static void RetireResource(std::shared_ptr<void> r){held.push_back(std::move(r));}
};
struct Memory {
 std::array<u8,65536> bytes{};
 template<class T>T TranslateVirtual(u32 at){return reinterpret_cast<T>(bytes.data()+at);}
};
Memory memory;
#define REX_KERNEL_MEMORY() (&memory)
bool NativeTextureTimingEnabled(){return true;}
struct BufferUploadTiming {u64 calls=0,hashed_bytes=0,converted_bytes=0;double hash_ms=0;};
BufferUploadTiming g_buffer_timing;
'''+resource+r'''
std::shared_ptr<BufferResource> resource;
auto AdoptBuffer(u32,BufferKind)->std::shared_ptr<BufferResource>{return resource;}
struct BufferResourceView {
 plume::RenderBuffer* buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;
 u64 content_hash=0,byte_order_hash=0;
};
'''+body('void ByteSwapElements(')+'\n'+body('plume::RenderBuffer* ResolveBufferContents(')+'\n'+body('plume::RenderBuffer* ResolveBufferResource(')+'\n'+body('BufferResourceView ResolveBufferResourceWindow(')+r'''
// Exercise placement-header adoption too: the conversion tests above use a
// fixed resource and cannot detect selecting the wrong half of a pool owner.
struct D3DBuffer {struct {u32 common;u32 rest[5];} resource;u32 fetch_lo,fetch_hi;};
std::mutex g_buffers_mutex;
std::unordered_map<u32,std::shared_ptr<BufferResource>> g_buffers;
D3DBuffer* GuestBuffer(u32 at){return memory.TranslateVirtual<D3DBuffer*>(at);}
std::shared_ptr<BufferResource> FindBuffer(u32 at){
 std::lock_guard lock(g_buffers_mutex);auto it=g_buffers.find(at);
 return it==g_buffers.end()?nullptr:it->second;
}
'''+body('std::shared_ptr<BufferResource> AdoptBuffer(').replace('AdoptBuffer(', 'ActualAdoptBuffer(')+ '\n'+body('plume::RenderBuffer* ResolveBufferResource(').replace('ResolveBufferResource(', 'ResolveAdoptedBuffer(').replace('AdoptBuffer(', 'ActualAdoptBuffer(')+r'''
int main() {
 std::mt19937 random(17);
 // Canonicalization must preserve every byte the ORIGINAL stream can fetch.
 for(u32 n=0;n<10000;++n) {
  const u32 stride=4*(1+random()%24),offset=random()%512;
  const std::array<u32,2> fields{0, stride-4};
  std::vector<u8> old(1024),canonical(1024);
  for(auto& byte:old)byte=random();canonical=old;
  const VertexByteOrder order{stride,offset,fields};
  assert(ApplyVertexByteOrder(old.data(),old.size(),order));
  assert(ApplyVertexByteOrder(canonical.data(),canonical.size(),CanonicalVertexByteOrder(order)));
  assert(std::equal(old.begin()+offset,old.end(),canonical.begin()+offset));
 }
 resource=std::make_shared<BufferResource>();resource->length=1024;resource->mirror_address=32;
 for(u32 i=0;i<1024;++i)memory.bytes[32+i]=u8(i*7+3);
 const std::array<u32,1> field{4};VertexByteOrder order{16,32,field};
 auto* first=ResolveBufferResource(1,BufferKind::kVertex,order);assert(first);
 const auto original_bytes=first->data;
 assert(HostDevice::device.allocations==1);
 order.stream_offset=160;assert(ResolveBufferResource(1,BufferKind::kVertex,order)==first);
 assert(HostDevice::device.allocations==1);
 auto* plain=ResolveBufferResource(1,BufferKind::kVertex,{});assert(plain&&plain!=first);
 assert(ResolveBufferResource(1,BufferKind::kVertex,order)==first);
 assert(HostDevice::device.allocations==2);
 memory.bytes[32+164]^=0xFF; // CPU update between draws in the same frame.
 auto* changed=ResolveBufferResource(1,BufferKind::kVertex,order);assert(changed&&changed!=first);
 assert(changed->data!=original_bytes&&first->data==original_bytes);
 assert(resource->variants.size()==1&&HostDevice::held.size()==2);
 for(u32 phase=0;phase<16;++phase){order.stream_offset=phase;assert(ResolveBufferResource(1,BufferKind::kVertex,order));}
 assert(resource->variants.size()==8); // Evicted versions survive their fence.
 assert(first->data==original_bytes);
 order.stream_offset=2048;assert(!ResolveBufferResource(1,BufferKind::kVertex,order));
 resource->kind=BufferKind::kIndex;resource->variants.clear();resource->buffer.reset();resource->format=1;
 auto* index=ResolveBufferResource(1,BufferKind::kIndex,{});assert(index);
 assert(index->data[0]==memory.bytes[33]&&index->data[1]==memory.bytes[32]);
 assert(g_buffer_timing.hashed_bytes>=1024*20); // No per-frame/dirty-only skip.
 // Two TU23 headers, as at owner+24 and owner+56. Relocate only the selected
 // header, alternate its backing, and rewrite contents between recorded draws.
 constexpr u32 owner=128;
 for(u32 parity=0;parity<2;++parity){
  auto* header=GuestBuffer(owner+24+32*parity);
  header->resource.common=1;header->fetch_lo=4096+2048*parity;header->fetch_hi=1024;
 }
 std::vector<std::pair<plume::RenderBuffer*,std::vector<u8>>> recorded;
 for(u32 frame=0;frame<32;++frame){
  const u32 parity=frame%2,header_address=owner+24+32*parity;
  auto* header=GuestBuffer(header_address);
  // Phase-2 relocation updates the same header, not merely its owner address.
  header->fetch_lo=4096+2048*parity+((frame/2)%2)*1024;
  for(u32 i=0;i<1024;++i)memory.bytes[header->fetch_lo+i]=u8(frame*13+i);
  auto adopted=ActualAdoptBuffer(header_address,BufferKind::kVertex);
  assert(adopted&&adopted->mirror_address==header->fetch_lo);
  auto* uploaded=ResolveAdoptedBuffer(header_address,BufferKind::kVertex,{});
  assert(uploaded&&uploaded->data[0]==memory.bytes[header->fetch_lo+3]);
  assert(ResolveAdoptedBuffer(header_address,BufferKind::kVertex,{})==uploaded);
  recorded.emplace_back(uploaded,uploaded->data);
  memory.bytes[header->fetch_lo]^=0x80;
  auto* rewrite=ResolveAdoptedBuffer(header_address,BufferKind::kVertex,{});
  assert(rewrite!=uploaded&&rewrite->data!=uploaded->data);
  for(const auto& [old,expected]:recorded)assert(old->data==expected);
 }
 assert(g_buffers.size()==2);
 GuestBuffer(owner+24)->resource.common=2;
 assert(!ActualAdoptBuffer(owner+24,BufferKind::kVertex));
 // A rebased draw must fetch exactly the same bytes as the original whole
 // upload, including packed field reversal and a nonzero stream offset.
 resource=std::make_shared<BufferResource>();resource->length=32768;resource->mirror_address=16000;
 for(u32 i=0;i<resource->length;++i)memory.bytes[16000+i]=u8(random());
 for(u32 n=0;n<1000;++n){
  const u32 stride=4*(2+random()%10),stream_offset=4*(random()%8);
  const u32 low=1+random()%100,high=low+random()%8;
  const int32_t base=int32_t(random()%21)-10;
  const auto window=DrawVertexWindow(low,high,base,stride,stream_offset,resource->length);
  if(int64_t(low)+base<0){assert(!window.length);continue;}
  assert(window.length);
  const std::array<u32,1> packed{stride-4};
  const auto* full=ResolveBufferResource(1,BufferKind::kVertex,{stride,stream_offset,packed});
  auto slice=ResolveBufferResourceWindow(1,BufferKind::kVertex,window.offset,window.length,{stride,0,packed});
  assert(slice.buffer&&slice.mirror_address==16000+window.offset&&slice.length==window.length);
  assert(std::equal(slice.buffer->data.begin(),slice.buffer->data.end(),full->data.begin()+window.offset));
  for(u32 index=low;index<=high;++index){
   const u32 old_offset=stream_offset+(index+base)*stride;
   const u32 new_offset=(index-low)*stride;
   assert(std::equal(slice.buffer->data.begin()+new_offset,slice.buffer->data.begin()+new_offset+stride,
                    full->data.begin()+old_offset));
  }
 }
 assert(resource->windows.size()<=4096);
 assert(resource->window_cache_bytes<=256*1024);
 resource->windows.clear();resource->window_cache_bytes=0;
 for(u32 i=0;i<256;++i)
   assert(ResolveBufferResourceWindow(1,BufferKind::kVertex,i*32,32,{}).buffer);
 const auto warmed_allocations=HostDevice::device.allocations;
 for(u32 frame=0;frame<3;++frame)
   for(u32 i=0;i<256;++i)
     assert(ResolveBufferResourceWindow(1,BufferKind::kVertex,i*32,32,{}).buffer);
 assert(HostDevice::device.allocations==warmed_allocations);
 assert(!DrawVertexWindow(10,20,-11,16,0,32768).length);
 assert(!DrawVertexWindow(20,10,0,16,0,32768).length);
 assert(!DrawVertexWindow(UINT32_MAX,UINT32_MAX,INT32_MAX,UINT32_MAX,0,32768).length);
 auto slice=ResolveBufferResourceWindow(1,BufferKind::kVertex,128,256,{});
 const auto old_slice=slice.buffer->data;
 memory.bytes[16000+128+7]^=0x80;
 auto edited=ResolveBufferResourceWindow(1,BufferKind::kVertex,128,256,{});
 assert(edited.buffer!=slice.buffer&&edited.buffer->data!=old_slice&&slice.buffer->data==old_slice);
 const auto hashed_before=g_buffer_timing.hashed_bytes;
 for(u32 i=0;i<100;++i)assert(ResolveBufferResourceWindow(1,BufferKind::kVertex,128,256,{}).buffer==edited.buffer);
 assert(g_buffer_timing.hashed_bytes-hashed_before==256*100);
 assert(!ResolveBufferResourceWindow(1,BufferKind::kVertex,129,256,{}).buffer);
 assert(!ResolveBufferResourceWindow(1,BufferKind::kVertex,32760,16,{}).buffer);
}
''')
exe=a.output/'buffer-variants-test.exe'
subprocess.run(['clang++','-std=c++20','-DNOMINMAX','-I'+str(root/'rexlego/src'),
 '-I'+str(root/'rexglue-sdk/thirdparty/xxHash'),str(h),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
(a.output/'verification.json').write_text(json.dumps({'actual_buffer_resolve_body':True,'fake_driver':True,
 'canonical_equivalence_cases':10000,'checks':['same-phase reuse','alternating declarations','same-frame CPU update',
 'immutable old versions','8-version bound','invalid offset','index endian','every-use content hashing',
 'actual placement adoption','32 alternating owner-header frames','same-header relocation',
 'same-frame rewritten placement data','all recorded placement versions preserved',
 '1000 window rebasing/conversion cases','byte/entry cache bound','256-range working set reused without allocations','immutable rewritten windows',
 'every-use hashing restricted to requested window','invalid window/overflow rejection'],
 'actual_buffer_adoption_body':True,'passed':True},indent=2))
print('Passed 10000 fetched-byte equivalence cases and actual conversion-cache body, including same-frame writes and old-version lifetime.')
