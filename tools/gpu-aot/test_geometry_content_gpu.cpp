// Production geometry conversion/cache/promotion with real tapes and D3D12.
// Guest memory, draw view and retirement queue are fixtures.
#define NOMINMAX
#include "gpu_native/native_recording.h"
#include "gpu_native/geometry_content_cache.h"
#include "gpu_native/upload_buffer_pool.h"
#include "gpu_native/vertex_upload.h"
#include "gpu_native/index_snapshot_cache.h"
#include "gpu_native/memory_watch.h"
#include <plume_d3d12.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
#include <immintrin.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using be_u32=uint32_t;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
enum class BufferKind:u32{kVertex=6,kIndex=7};
#include "geometry-resource.inc"
std::unique_ptr<plume::RenderDevice> device;
plume::RenderCommandList* active_commands=nullptr;
std::vector<std::shared_ptr<void>>* active_retirement=nullptr;
struct HostDevice {
 static auto Device(){return device.get();}
 struct View{plume::RenderDevice* device;plume::RenderCommandList* commands;};
 static View CurrentDrawDeviceView(){return {device.get(),active_commands};}
 static void RetireResource(std::shared_ptr<void> value){CHECK(active_retirement);active_retirement->push_back(std::move(value));}
};
struct Memory {
 std::array<u8,65536> bytes{};
 template<class T>T TranslateVirtual(u32 at){return reinterpret_cast<T>(bytes.data()+at);}
}memory;
#define REX_KERNEL_MEMORY() (&memory)
struct BufferUploadTiming {u64 calls=0,hashed_bytes=0,converted_bytes=0,watch_hits=0,watch_audits=0,watch_mismatches=0;double hash_ms=0;};
BufferUploadTiming g_buffer_timing;
std::mutex g_buffer_timing_mutex;
std::atomic<u32> g_probe_frame{100};
bool NativeTextureTimingEnabled(){return true;}
bool LongProbeEnabled(){return false;}
std::array<u64,16> versions{};
bool legodimensions::gpu_native::CpuBufferMemoryWatchEnabled(){return true;}
CpuMemoryStamp legodimensions::gpu_native::WatchCpuMemory(std::span<const CpuMemorySpan> spans){
 CpuMemoryStamp stamp;for(auto span:spans)
  for(u32 page=span.address/4096;page<=(span.address+span.length-1)/4096;++page)stamp.pages.emplace_back(page,versions.at(page));
 return stamp;
}
bool legodimensions::gpu_native::CpuMemoryUnchanged(const CpuMemoryStamp& stamp){
 if(stamp.pages.empty())return false;
 for(auto[page,version]:stamp.pages)if(versions.at(page)!=version)return false;return true;
}
void TraceBufferAllocation(const BufferResource&,const VertexByteOrder&,u64,u64,u64,size_t,bool){}
struct BufferResourceView {plume::RenderBuffer* buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;u64 content_hash=0,byte_order_hash=0;};
#include "geometry-production.inc"
auto resource(u32 address,BufferKind kind,u32 format){
 auto r=std::make_shared<BufferResource>();r->guest_address=address+0x100;r->mirror_address=address;r->length=256;
 r->kind=kind;r->format=format;r->owns_guest_memory=false;return r;
}
std::vector<u8> expected(u32 address,BufferKind kind,u32 format,const VertexByteOrder& order){
 std::vector<u8> result(256);const u32 element=kind==BufferKind::kIndex&&format==1?2:4;
 for(u32 i=0;i<256;++i){
  u32 from=(i/element)*element+element-1-i%element;
  if(kind==BufferKind::kVertex && !order.reversed_elements.empty() && i>=order.stream_offset){
   const u32 phase=(i-order.stream_offset)%order.stride;
   for(u32 field:order.reversed_elements)if((phase&~3u)==VertexFieldOffset(field))
    from=(field&kHalfSwapField)?(i&~3u)+(i%4^1):i;
  }
  result[i]=memory.bytes[address+from];
 }
 return result;
}
int main(){
 auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=device->createCommandFence();
 u32 readbacks=0;
 auto read=[&](plume::RenderBuffer* buffer,NativeDrawCommands* tape){
  auto target=device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(256));CHECK(target);
  commands->begin();if(tape)tape->Replay(commands.get());
  commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderBufferBarrier(buffer,plume::RenderBufferAccess::READ));
  commands->copyBufferRegion(target->at(0),buffer->at(0),256);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderBufferBarrier(buffer,plume::RenderBufferAccess::READ));
  commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
  auto* mapped=static_cast<const u8*>(target->map());CHECK(mapped);std::vector<u8> bytes(mapped,mapped+256);target->unmap();++readbacks;return bytes;
 };
 const std::array<u32,2> fields{4,kHalfSwapField|8};
 for(u32 test=0;test<4;++test){
  g_geometry_content_cache.Reset();g_probe_frame+=20;
  const auto kind=test<2?BufferKind::kVertex:BufferKind::kIndex;const u32 format=test==2?1:0;
  const VertexByteOrder order=test==1?VertexByteOrder{16,0,fields}:VertexByteOrder{};
  for(u32 i=0;i<256;++i)memory.bytes[4096+i]=memory.bytes[8192+i]=u8(i*19+test*3+7);
  auto earlier=resource(4096,kind,format),later=resource(8192,kind,format);
  auto early_tape=CreateNativeDrawCommands(),late_tape=CreateNativeDrawCommands();
  std::vector<std::shared_ptr<void>> early_retired,late_retired;
  active_commands=late_tape.get();active_retirement=&late_retired;
  auto* late=ResolveBufferContents(later,later->guest_address,kind,order);CHECK(late);
  active_commands=early_tape.get();active_retirement=&early_retired;
  auto* early=ResolveBufferContents(earlier,earlier->guest_address,kind,order);CHECK(early);
  if(kind==BufferKind::kVertex)CHECK(early!=late);
  const auto original=expected(4096,kind,format,order);
  CHECK(read(early,early_tape.get())==original);
  auto second_fence=early_retired;early_retired.clear();second_fence.clear();
  CHECK(read(late,late_tape.get())==original);late_retired.clear();
  auto relocated=resource(4096,kind,format);auto reuse_tape=CreateNativeDrawCommands();
  std::vector<std::shared_ptr<void>> reuse_retired;
  active_commands=reuse_tape.get();active_retirement=&reuse_retired;
  const auto converted=g_buffer_timing.converted_bytes;
  auto* shared=ResolveBufferContents(relocated,relocated->guest_address,kind,order);
  CHECK(shared && (shared==early||shared==late) && reuse_tape->Empty());
  CHECK(g_buffer_timing.converted_bytes==converted && read(shared,nullptr)==original);
  memory.bytes[4096+7]^=0x80;++versions[1];g_probe_frame+=20;
  auto* modified=ResolveBufferContents(relocated,relocated->guest_address,kind,order);
  CHECK(modified && modified!=shared);
  CHECK(read(modified,reuse_tape.get())==expected(4096,kind,format,order));reuse_retired.clear();
  CHECK(read(shared,nullptr)==original);
  if(kind==BufferKind::kVertex){
   g_geometry_content_cache.Reset();
   auto abandoned=resource(4096,kind,format),next=resource(4096,kind,format);
   auto abandoned_tape=CreateNativeDrawCommands(),next_tape=CreateNativeDrawCommands();
   std::vector<std::shared_ptr<void>> abandoned_retired,next_retired;
   active_commands=abandoned_tape.get();active_retirement=&abandoned_retired;
   auto* missing=ResolveBufferContents(abandoned,abandoned->guest_address,kind,order);CHECK(missing);
   abandoned_tape->Reset();abandoned_retired.clear();
   active_commands=next_tape.get();active_retirement=&next_retired;
   auto* fresh=ResolveBufferContents(next,next->guest_address,kind,order);CHECK(fresh && fresh!=missing);
   CHECK(read(fresh,next_tape.get())==expected(4096,kind,format,order));next_retired.clear();
  }
 }
 g_geometry_content_cache.Reset();g_upload_pool.Reset();g_resident_pool.Reset();
 std::printf("PASS %u GPU geometry readbacks: actual conversion/promotion/tapes, reverse preparation, fenced relocation reuse, immutable CPU updates, abandoned tape rejection\n",readbacks);
}
