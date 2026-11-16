"""Execute actual buffer adoption/upload/view composition during replacement.

Allocation/map and guest memory are bounded driver mocks. Production hash,
conversion, immutable versioning and view/adoption bodies execute unchanged.
"""
import argparse,hashlib,json,os,subprocess
from pathlib import Path

def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
    p.add_argument('--compiler',default='clang++');p.add_argument('--expect-old-failure',action='store_true')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2];source=(root/'rexlego/src/gpu_native/buffers.cpp').read_text()
    resource=source[source.index('struct BufferResource {'):source.index('UploadBufferPool<')]
    code=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#define assert(c) do{if(!(c)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu_native/buffer_header.h"
#include "gpu_native/resource_registry.h"
#include "gpu_native/vertex_upload.h"
#include "gpu_native/upload_buffer_pool.h"
#include "gpu_native/memory_watch.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using be_u32=uint32_t;
enum class BufferKind:u32{kVertex=6,kIndex=7};
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
std::function<void()> on_map;
bool fail_allocate=false,fail_map=false;
namespace plume {
enum class RenderHeapType {UPLOAD};
struct RenderBufferDesc {u32 size;static auto IndexBuffer(u32 n,RenderHeapType){return RenderBufferDesc{n};}
 static auto VertexBuffer(u32 n,RenderHeapType){return RenderBufferDesc{n};}};
struct RenderBuffer {std::vector<u8>data;explicit RenderBuffer(u32 n):data(n){}
 void*map(){if(on_map){auto next=std::move(on_map);on_map={};next();}return fail_map?nullptr:data.data();}
 void unmap(){}void setName(const char*){}};
struct Device {u32 allocations=0;std::unique_ptr<RenderBuffer>createBuffer(RenderBufferDesc d){
 ++allocations;return fail_allocate?nullptr:std::make_unique<RenderBuffer>(d.size);}};
}
struct BufferResourceView {
 plume::RenderBuffer*buffer=nullptr;u32 length=0,guest_format=0,mirror_address=0;
 u64 content_hash=0,byte_order_hash=0;
};
struct BufferUploadTiming {u64 calls=0,hashed_bytes=0,converted_bytes=0,watch_hits=0,watch_audits=0,watch_mismatches=0;double hash_ms=0;};
struct HostDevice {
 static bool IsReady(){return true;}static inline plume::Device device;
 static auto Device(){return &device;}
 static inline std::vector<std::shared_ptr<void>>held;
 template<class T>static void RetireResource(std::shared_ptr<T>r){held.push_back(std::move(r));}
};
void PromoteImmutableGeometry(std::shared_ptr<plume::RenderBuffer>&,BufferKind,u32){}
struct D3DBuffer{struct{u32 common=0,pad[5]{};}resource;u32 fetch_lo=0,fetch_hi=0;};
struct Memory {std::array<u8,65536>bytes{};
 template<class T>T TranslateVirtual(u32 at){assert(at<bytes.size());return reinterpret_cast<T>(bytes.data()+at);}}memory;
#define REX_KERNEL_MEMORY() (&memory)
bool NativeTextureTimingEnabled(){return true;}
bool legodimensions::gpu_native::CpuBufferMemoryWatchEnabled(){return false;}
CpuMemoryStamp legodimensions::gpu_native::WatchCpuMemory(std::span<const CpuMemorySpan>){return {};}
bool legodimensions::gpu_native::CpuMemoryUnchanged(const CpuMemoryStamp&){return false;}
BufferUploadTiming g_buffer_timing;
'''+resource+r'''
UploadBufferPool<plume::RenderBuffer>g_upload_pool;
std::mutex g_buffers_mutex;legodimensions::gpu_native::ResourceRegistry<BufferResource>g_buffers;
u32 lookups=0;
D3DBuffer*GuestBuffer(u32 at){return memory.TranslateVirtual<D3DBuffer*>(at);}
void TraceBufferAllocation(const BufferResource&,const VertexByteOrder&,u64,u64,u64,size_t,bool){}
'''
    for signature in ['std::shared_ptr<BufferResource> FindBuffer(','std::shared_ptr<BufferResource> AdoptBuffer(',
                      'void ByteSwapElements(','std::shared_ptr<plume::RenderBuffer> CreateImmutableUploadBuffer(',
                      'plume::RenderBuffer* ResolveBufferContents(','plume::RenderBuffer* ResolveBufferResource(',
                      'BufferResourceView ResolveBufferResourceView(']:
        body=block(source,signature)
        if signature.endswith(' FindBuffer('):
            body=body.replace('  return g_buffers.Lookup(', '  ++lookups;return g_buffers.Lookup(')
        code+=body+'\n'
    code+=r'''
constexpr u32 guest=128;
void Header(BufferKind kind,u32 mirror,u32 size,u32 format=1){auto*h=GuestBuffer(guest);
 h->resource.common=kind==BufferKind::kVertex?1:2|(format==2?0x80000000u:0);
 h->fetch_lo=mirror;h->fetch_hi=size;}
void Reset(BufferKind kind,u32 format=1){on_map={};fail_allocate=fail_map=false;
 g_buffers.clear();HostDevice::held.clear();g_upload_pool.Reset();memory.bytes.fill(0);lookups=0;g_buffer_timing={};HostDevice::device.allocations=0;
 Header(kind,4096,64,format);for(u32 i=0;i<512;++i){memory.bytes[4096+i]=u8(i*13+7);memory.bytes[8192+i]=u8(i*3+19);}}
void Coherent(const BufferResourceView&v,u32 mirror,u32 size,u32 format,BufferKind kind){
 assert(v.buffer && v.mirror_address==mirror && v.length==size && v.guest_format==format);
 assert(v.buffer->data.size()>=size);
 assert(v.content_hash==XXH3_64bits(memory.bytes.data()+mirror,size));
 std::vector<u8>expected(size);ByteSwapElements(expected.data(),memory.bytes.data()+mirror,size,kind==BufferKind::kIndex&&format==1?2:4);
 assert(std::equal(expected.begin(),expected.end(),v.buffer->data.begin()));}
int main(){
 for(auto kind:{BufferKind::kVertex,BufferKind::kIndex}){
  const u32 format=kind==BufferKind::kIndex?1:0;Reset(kind);
  auto first=ResolveBufferResourceView(guest,kind,{});Coherent(first,4096,64,format,kind);
  assert(lookups==1);const auto saved=first.buffer->data;
  auto same=ResolveBufferResourceView(guest,kind,{});Coherent(same,4096,64,format,kind);
  assert(same.buffer==first.buffer&&HostDevice::device.allocations==1&&lookups==2&&g_buffer_timing.calls==2);
  memory.bytes[4096+7]^=0x40;auto changed=ResolveBufferResourceView(guest,kind,{});
  Coherent(changed,4096,64,format,kind);assert(changed.buffer!=first.buffer&&first.buffer->data==saved&&g_buffer_timing.calls==3);
  Reset(kind);auto old=AdoptBuffer(guest,kind);std::shared_ptr<BufferResource>replacement;
  on_map=[&]{Header(kind,8192,256,2);replacement=AdoptBuffer(guest,kind);assert(replacement&&replacement!=old);};
  auto selected=ResolveBufferResourceView(guest,kind,{});
  // A publication during map cannot mix A's GPU bytes with B's metadata.
  Coherent(selected,4096,64,format,kind);assert(selected.buffer==old->buffer.get());
  assert(g_buffers.find(guest)->second==replacement&&HostDevice::held.size()==1);
  const auto recorded=selected.buffer->data;auto next=ResolveBufferResourceView(guest,kind,{});
  Coherent(next,8192,256,kind==BufferKind::kIndex?2:0,kind);
  assert(next.buffer!=selected.buffer&&selected.buffer->data==recorded);
  Header(kind==BufferKind::kIndex?BufferKind::kVertex:BufferKind::kIndex,8192,256);
  auto invalid=ResolveBufferResourceView(guest,kind,{});assert(!invalid.buffer&&!invalid.length&&!invalid.mirror_address&&!invalid.content_hash);
  for(bool mapping:{false,true}){Reset(kind);fail_map=mapping;fail_allocate=!mapping;
   auto failed=ResolveBufferResourceView(guest,kind,{});assert(!failed.buffer&&!failed.length&&!failed.mirror_address&&!failed.content_hash);
   fail_map=fail_allocate=false;Coherent(ResolveBufferResourceView(guest,kind,{}),4096,64,format,kind);}
  Reset(kind);auto owned=AdoptBuffer(guest,kind);owned->owns_guest_memory=true;
  Header(kind,8192,256,2);Coherent(ResolveBufferResourceView(guest,kind,{}),4096,64,format,kind);
  const auto wrong=ResolveBufferResourceView(guest,kind==BufferKind::kIndex?BufferKind::kVertex:BufferKind::kIndex,{});assert(!wrong.buffer&&!wrong.length);
 }
 auto zero=ResolveBufferResourceView(0,BufferKind::kVertex,{});assert(!zero.buffer&&!zero.length);
 std::puts("PASS production whole-buffer view: one lookup, coherent upload owner, next-call relocation/format, immutable versions, failures and owned headers");
}
'''
    def run(name,body):
        cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
        cmd=[a.compiler,'-std=c++20','-D_CRT_SECURE_NO_WARNINGS','-UNDEBUG','-I'+str(root/'rexlego/src'),
             '-I'+str(root/'rexglue-sdk/thirdparty/xxHash'),str(cpp),'-o',str(exe)]
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
        (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10,env=dict(os.environ,LEGO_NATIVE_UPLOAD_POOL='0'))
        (out/(name+'.log')).write_text(r.stdout+r.stderr);return r
    r=run('view',code)
    if a.expect_old_failure:
        assert r.returncode==23,r.stdout+r.stderr
        # Ignore only the expected extra lookup to isolate the ownership race.
        legacy=code.replace('assert(lookups==1);','assert(lookups==2);').replace('lookups==2&&g_buffer_timing.calls==2','lookups==4&&g_buffer_timing.calls==2')
        race=run('view-race',legacy);assert race.returncode==23,race.stdout+race.stderr
        (out/'verification.json').write_text(json.dumps(dict(before_fix=True,exit=r.returncode,stderr=r.stderr,
            race_exit=race.returncode,race_stderr=race.stderr,production_view=block(source,'BufferResourceView ResolveBufferResourceView(')),indent=2))
        print('REPRODUCED old view failure:',race.stderr);return
    assert r.returncode==0,r.stdout+r.stderr
    legacy=code.replace('assert(lookups==1);','assert(lookups==2);').replace('lookups==2&&g_buffer_timing.calls==2','lookups==4&&g_buffer_timing.calls==2')
    before=block(source,'BufferResourceView ResolveBufferResourceView(')
    old='''BufferResourceView ResolveBufferResourceView(u32 guest_address, BufferKind kind,const VertexByteOrder& byte_order) {
 auto* buffer=ResolveBufferResource(guest_address,kind,byte_order);
 const auto resource=FindBuffer(guest_address);
 if(!resource || resource->kind!=kind)return {};
 return {buffer,resource->length,resource->format,resource->mirror_address,resource->content_hash,resource->byte_order_hash};
}'''
    n=run('old-mixed-owner',legacy.replace(before,old));assert n.returncode==23,n.stdout+n.stderr
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,production_adoption_upload_and_view=True,driver_boundaries_mocked=True,
        source_sha256=hashlib.sha256(source.encode()).hexdigest(),positive=dict(exit=r.returncode,stdout=r.stdout),
        negative=[dict(name='old-mixed-owner',exit=n.returncode,stderr=n.stderr)],
        scope='actual hash/conversion/immutable versioning/adoption/view; publication injected during mocked map; no full game/GPU/FPS proof'),indent=2))
    print(r.stdout)
if __name__=='__main__':main()
