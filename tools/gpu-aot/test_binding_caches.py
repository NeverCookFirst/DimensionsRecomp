"""Exercise production binding-cache bodies with explicit invalidation and lifetime changes."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
textures = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
buffers = (root / 'rexlego/src/gpu_native/buffers.cpp').read_text()
texture_body = textures[textures.index('TexturePreparationResult PrepareTextureBinding('):textures.index('bool ResolveTextureFromSurface(')]
buffer_body = buffers[buffers.index('BufferResourceView ResolveBufferWindowCached('):buffers.index('BufferResourceView ResolveBufferResourceView(')]
texture_probe = textures[textures.index('TexturePreparationSnapshot CaptureTexturePreparation('):textures.index('bool UploadTextureResource(u32 guest_address,')]
buffer_probe = buffers[buffers.index('PreparedBufferVersion PrepareCachedBuffer('):buffers.index('BufferResourceView InspectBufferResource(')]
source = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <future>
#include "gpu_native/vertex_byte_order.h"
using u32=uint32_t; using u64=uint64_t;
using NativeRecordingGate=std::recursive_mutex;
bool UnlockedTextureUploadEnabled(){return false;}
bool CurrentDrawPreparation(){return false;}
#define REXLOG_INFO(...) ((void)0)
struct HostDevice {
  static auto LockRecording() { static std::recursive_mutex mutex; return std::unique_lock(mutex); }
};
using legodimensions::gpu_native::VertexByteOrder;
using legodimensions::gpu_native::CanonicalVertexByteOrder;
namespace plume { struct RenderTexture {}; struct RenderBuffer {}; struct RenderCommandList {}; }
void PrepareVertexTextureShaderRead(plume::RenderCommandList*,plume::RenderTexture*){}
plume::RenderCommandList* NativeResourceCommands(plume::RenderCommandList* value){return value;}
constexpr u32 kNativeTextureSlots=26,kNativeVertexStreams=16;
u64 epoch=1;
struct Stamp { bool valid=true; };
bool CpuMemoryUnchanged(const Stamp& s) { return s.valid; }
u64 CpuMemoryWriteEpoch() { return epoch; }
bool CpuBufferMemoryWatchEnabled() { return true; }
bool LongProbeEnabled() { return false; }
bool DepthAliasesEnabled() { return true; }
bool IsDepthFormat(u32 format) { return format==99; }
struct D3DTexture { struct {u32 common=3;} resource; struct {u32 dword[6]={1,2,3,4,5,6};} format; } header;
template<class T> T* GuestAt(u32) { return &header; }
struct TextureResourceView { plume::RenderTexture* texture=nullptr; u32 descriptor_index=0; u64 content_hash=0; };
enum class TexturePreparationResult { kUnavailable,kReady,kUploadFailed };
struct TextureResource {
 std::mutex mutex;std::atomic<bool>replaced{false};
 u32 adopted_header_type=3,guest_address=123;
 bool guest_uploaded=true, resolved_on_host=false, owns_guest_memory=false, surface=false;
 bool guest_backing_current=false,upload_source_key_valid=false,sampled_valid=false;
 u64 depth_resolve_generation=0;
 struct {void Reset(){}}writeback_coverage;
 u64 depth_alias_generation=0,guest_content_hash=10;
 u32 descriptor_index=4,guest_format=18,format=18;
 std::array<u32,6> guest_fetch{1,2,3,4,5,6};
 std::unique_ptr<plume::RenderTexture> texture=std::make_unique<plume::RenderTexture>();
 Stamp cpu_stamp;
};
std::atomic<u64> g_textures_generation{1};
struct TextureRegistry{u64 BucketVersion(u32){return g_textures_generation;}}g_textures;
auto texture_owner=std::make_shared<TextureResource>();
auto FindTexture(u32) { return texture_owner; }
struct TexturePreparationSnapshot { std::shared_ptr<void> owner; std::array<u32,7> header{}; bool alias_settled=false; };
TexturePreparationSnapshot CaptureTexturePreparation(u32,std::shared_ptr<void>);
bool SettleDepthAlias(u32,TextureResource&,plume::RenderCommandList*,bool& aliased){aliased=false;return false;}
TextureResourceView TextureResourceViewOf(u32, const std::shared_ptr<TextureResource>& r) {
 return {r->texture.get(),r->descriptor_index,r->guest_content_hash};
}
u32 prepares=0;
TexturePreparationResult PrepareTextureResource(u32,plume::RenderCommandList*,bool,TextureResourceView& view) {
 ++prepares;
 if(!texture_owner) return TexturePreparationResult::kUnavailable;
 auto& r=*texture_owner;
 std::copy(std::begin(header.format.dword),std::end(header.format.dword),r.guest_fetch.begin());
 if(!r.cpu_stamp.valid) {r.cpu_stamp.valid=true;++r.guest_content_hash;}
 view={r.texture.get(),r.descriptor_index,r.guest_content_hash};return TexturePreparationResult::kReady;
}
enum class BufferKind { kVertex,kIndex };
struct D3DBuffer { struct {u32 common=1;} resource; u32 fetch_lo=64,fetch_hi=256; } buffer_header;
D3DBuffer* GuestBuffer(u32) { return &buffer_header; }
struct BufferResourceView { plume::RenderBuffer* buffer=nullptr; u32 length=0,guest_format=0,mirror_address=0; u64 content_hash=0,byte_order_hash=0; };
struct BufferResource {
 u32 length=256,format=0,mirror_address=64;
 bool owns_guest_memory=false; std::mutex upload_mutex;
 std::unordered_map<u64,std::shared_ptr<BufferResource>> windows;
 std::shared_ptr<plume::RenderBuffer> buffer=std::make_shared<plume::RenderBuffer>();
 u64 content_hash=10; Stamp cpu_stamp;
 std::unordered_map<u64,std::shared_ptr<plume::RenderBuffer>> variants;
};
struct BufferPreparationSnapshot { std::shared_ptr<void> owner; BufferResourceView metadata; };
struct PreparedBufferVersion { std::shared_ptr<void> owner; std::shared_ptr<plume::RenderBuffer> storage; BufferResourceView view; };
u64 XXH3_64bits_withSeed(const void* bytes,size_t count,u64 seed) {
 auto p=static_cast<const uint8_t*>(bytes);
 while(count--)seed=(seed*1099511628211ull)^*p++;
 return seed;
}
struct Registry {u64 generation=1;u64 Generation(){return generation;}u64 BucketVersion(u32){return generation;}} g_buffers;
auto buffer_parent=std::make_shared<BufferResource>();
auto FindBuffer(u32) { return buffer_parent; }
u32 resolves=0;
BufferResourceView ResolveBufferResourceWindow(u32,BufferKind,u32 offset,u32 length,const VertexByteOrder&) {
 ++resolves;
 auto& r=buffer_parent->windows[(u64(offset)<<32)|length];
 if(!r)r=std::make_shared<BufferResource>();
 if(!r->cpu_stamp.valid){r->cpu_stamp.valid=true;++r->content_hash;r->buffer=std::make_shared<plume::RenderBuffer>();}
 return {r->buffer.get(),length,0,offset,r->content_hash,0};
}
'''
authority=textures[textures.index('void RefreshPublishedTextureAuthority('):textures.index('// Only matching raw storage formats')]
source += authority + texture_body + buffer_body + texture_probe + buffer_probe
source += r'''
int main() {
 _putenv_s("LEGO_NATIVE_TEXTURE_BINDING_CACHE","1");
 _putenv_s("LEGO_NATIVE_BUFFER_STREAM_CACHE","1");
 std::array<u32,6> fetch{1,2,3,4,5,6}; TextureResourceView view;
 auto texture=[&](bool force=false,u32 overrides=0){return PrepareTextureBinding(0,123,fetch,overrides,nullptr,force,view);};
 texture();texture();assert(prepares==1);
 ++epoch;texture();assert(prepares==1);
 ++epoch;texture_owner->cpu_stamp.valid=false;texture();assert(prepares==2&&view.content_hash==11);
 texture();assert(prepares==2);
 ++header.format.dword[1];texture();assert(prepares==3);
 --header.format.dword[1];texture();assert(prepares==4);
 texture(true);assert(prepares==5);
 texture(false,7);assert(prepares==6);
 texture(false,7);assert(prepares==6);
 ++g_textures_generation;texture_owner=std::make_shared<TextureResource>();texture(false,7);assert(prepares==7);
 texture_owner->resolved_on_host=true;texture(false,7);assert(prepares==8);
 texture(false,7);assert(prepares==9);
 texture_owner->resolved_on_host=false;texture(false,7);assert(prepares==10);
 texture_owner.reset();assert(texture(false,7)==TexturePreparationResult::kUnavailable);
 assert(prepares==11);
 const u32 fields[]{4,8}; VertexByteOrder order{16,0,fields};
 auto buffer=[&](u32 length=64){return ResolveBufferStreamWindow(0,456,0,length,order);};
 auto first=buffer();buffer();assert(resolves==1);
 ++epoch;buffer();assert(resolves==1);
 auto& owner=buffer_parent->windows[64];
 ++epoch;owner->cpu_stamp.valid=false;
 auto second=buffer();assert(resolves==2&&second.content_hash!=first.content_hash);
 buffer();assert(resolves==2);
 ++owner->content_hash;owner->buffer=std::make_shared<plume::RenderBuffer>();
 buffer();assert(resolves==3);
 ++buffer_header.fetch_lo;buffer();assert(resolves==4);
 buffer();assert(resolves==4);
 ++g_buffers.generation;buffer();assert(resolves==5);
 order.stride=32;buffer();assert(resolves==6);
 const u32 other[]{4,12};order.reversed_elements=other;buffer();assert(resolves==7);
 buffer_parent->windows.clear();buffer();assert(resolves==8);
 buffer(128);assert(resolves==9);
 buffer();assert(resolves==9);
 buffer(128);assert(resolves==9);
 ++epoch;buffer_parent->windows[128]->cpu_stamp.valid=false;
 buffer(128);assert(resolves==10);
 buffer();assert(resolves==10);
 // Per-bucket versions do not reserve zero: changing to zero invalidates once, then the clean entry is reusable.
 g_buffers.generation=0;buffer();buffer();assert(resolves==11);
 texture_owner=std::make_shared<TextureResource>();
 auto captured=CaptureTexturePreparation(123);
 auto valid=PrepareCachedTexture(captured); assert(valid.texture==texture_owner->texture.get());
 texture_owner->cpu_stamp.valid=false; assert(!PrepareCachedTexture(captured).texture);
 texture_owner->cpu_stamp.valid=true;
 ++captured.header[1]; assert(!PrepareCachedTexture(captured).texture); --captured.header[1];
 texture_owner->resolved_on_host=true; assert(PrepareCachedTexture(captured).texture==texture_owner->texture.get());
 texture_owner->resolved_on_host=false;
 auto old_texture=texture_owner->texture.get();
 texture_owner=std::make_shared<TextureResource>();
 assert(PrepareCachedTexture(captured).texture==old_texture);
 auto parent=std::make_shared<BufferResource>();
 auto range=std::make_shared<BufferResource>(); range->length=64;
 parent->windows[64]=range; range->variants[0]=range->buffer;
 BufferPreparationSnapshot snapshot{parent,{}};
 auto prepared=PrepareCachedBuffer(snapshot,0,64,true,{});
 assert(prepared.view.buffer==range->buffer.get() && prepared.view.length==64);
 range->cpu_stamp.valid=false; assert(!PrepareCachedBuffer(snapshot,0,64,true,{}).view.buffer);
 range->cpu_stamp.valid=true;
 assert(!PrepareCachedBuffer(snapshot,0,128,true,{}).view.buffer);
 assert(!PrepareCachedBuffer(snapshot,0,64,true,order).view.buffer);
 std::vector<std::future<void>> probes;
 for(unsigned worker=0;worker<3;++worker)probes.push_back(std::async(std::launch::async,[&] {
   for(unsigned i=0;i<1000;++i) {
     assert(PrepareCachedBuffer(snapshot,0,64,true,{}).view.buffer==prepared.view.buffer);
     assert(PrepareCachedTexture(captured).texture==old_texture);
   }
 }));
 for(auto& probe:probes)probe.get();
 std::weak_ptr<plume::RenderBuffer> retained=prepared.storage;
 parent->windows.clear(); range.reset(); snapshot={}; parent.reset();
 assert(!retained.expired());
 prepared={}; assert(retained.expired());
}
'''
cpp = a.output / 'binding-caches.cpp'
cpp.write_text(source)
exe = a.output / 'binding-caches.exe'
subprocess.run(['clang++', '-std=c++20', '-O2', '-UNDEBUG', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(root / 'rexlego/src'), str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True, timeout=15)
print('PASS production texture/stream caches and parallel probes: writes, headers, replacement, retained lifetime, aliases, windows and 3000 concurrent validations')
