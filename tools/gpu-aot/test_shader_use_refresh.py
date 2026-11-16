"""Exercise production placement shader adoption and draw-time refresh offline."""
import argparse
from pathlib import Path
import subprocess
import json
import hashlib

def body(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++')
p.add_argument('--capture',type=Path,action='append',default=[])
a=p.parse_args();root=Path(__file__).resolve().parents[2]
src=(root/'rexlego/src/gpu_native/shaders.cpp').read_text()
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
assert draw.index('RefreshBoundShaderResources();') < draw.index('if (ShouldSkipBoundPixelShader())')
registry=src[src.index('struct ShaderResource {'):src.index('std::shared_ptr<ShaderResource> FindResource(')]
bodies='\n'.join(body(src,s) for s in (
 'std::shared_ptr<ShaderResource> FindResource(', 'bool PlacementShaderUnchanged(',
 'void InvalidatePlacementShader(', 'std::shared_ptr<ShaderResource> AdoptShader(',
 'bool BindShader(', 'void RefreshBoundShaderResources(', 'u32 BoundShaderAddress(',
 'u64 BoundShaderHash(', 'u32 BoundShaderTextureMask(', 'void ResetShaderResources('))
prefix=r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
using u32=uint32_t;using u64=uint64_t;
constexpr u32 kVertexShaderGuestSize=872,kPixelShaderGuestSize=40;
enum class ShaderStage:u32 {kVertex=6,kPixel=7};
namespace plume {struct RenderShader {};}
struct ShaderCacheEntry {u64 hash;};
struct be_u32 {
 uint8_t bytes[4]{};
 operator u32() const{return u32(bytes[0])<<24|u32(bytes[1])<<16|u32(bytes[2])<<8|bytes[3];}
 be_u32& operator=(u32 v){for(int i=0;i<4;++i)bytes[i]=uint8_t(v>>(24-i*8));return *this;}
};
struct D3DResource {be_u32 common;};
alignas(16) std::array<uint8_t,65536> guest{};
bool readable=true;
u32 reads=0,lookups=0;
struct Memory {
 template<class T>T TranslateVirtual(u32 address){assert(address<guest.size());++reads;return reinterpret_cast<T>(guest.data()+address);}
 void SystemHeapFree(u32){}
} memory;
#define REX_KERNEL_MEMORY() (&::memory)
bool ReadableGuestRange(u32 address,u32 size){return readable && address && size && u64(address)+size<=guest.size();}
size_t ReadableGuestSpan(u32 address){return ReadableGuestRange(address,32)?32:0;}
std::atomic<u32> g_probe_frame{1};
std::vector<std::shared_ptr<void>> retired;
struct HostDevice {
 static int LockRecording(){return 0;}
 template<class T>static void RetireResource(std::shared_ptr<T> r){retired.push_back(std::move(r));}
};
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
template<class... T>void LongProbeEvent(T&&...){}
template<class... T>void CaptureMissingPlacementShader(T&&...){}
void PollPrecompiledShaders(){}
ShaderCacheEntry programs[]{{0x111},{0x222}};
const ShaderCacheEntry* FindShaderByMicrocode(const void* data,u32 stage,size_t size){
 ++lookups;if(stage!=1 || size<32)return nullptr;
 const auto* bytes=static_cast<const uint8_t*>(data);
 // Same prefix; a later instruction selects the independently indexed program.
 if(bytes[24]==1)return &programs[0];if(bytes[24]==2)return &programs[1];return nullptr;
}
u32 FindShaderPhysicalSize(u64,u32){return 32;}
u32 FindShaderTextureMask(u64 hash){return hash==0x111?1:32;}
'''
suffix=r'''
void header(u32 code){*reinterpret_cast<be_u32*>(guest.data()+0x100)=7;*reinterpret_cast<be_u32*>(guest.data()+0x118)=code;}
int main(){
 header(0x1000);guest[0x1018]=1;
 assert(BindShader(ShaderStage::kPixel,0x100));
 auto first=g_bound_shaders[1];std::weak_ptr<ShaderResource> old=first;
 assert(first->hash==0x111 && first->texture_mask==1);
 const auto before=reads;for(int i=0;i<1000;++i)RefreshBoundShaderResources();
 assert(reads==before); // No per-mesh scan of an unchanged program.
 guest[0x1018]=2;++g_probe_frame;RefreshBoundShaderResources();
 if(BoundShaderHash(ShaderStage::kPixel)!=0x222 || BoundShaderTextureMask(ShaderStage::kPixel)!=32)return 23;
 assert(first->hash==0x111 && first->texture_mask==1); // Previous generation immutable.
 first.reset();assert(!old.expired());retired.clear();assert(old.expired());
 // Same header, new backing allocation, no setter/initializer notification.
 header(0x2000);guest[0x2018]=1;++g_probe_frame;RefreshBoundShaderResources();
 assert(BoundShaderHash(ShaderStage::kPixel)==0x111);
 // Temporarily unavailable program must not keep the previous texture mask.
 guest[0x2018]=9;++g_probe_frame;RefreshBoundShaderResources();
 bool failed=false;assert(BoundShaderAddress(ShaderStage::kPixel,&failed)==0x100 && failed);
 assert(BoundShaderHash(ShaderStage::kPixel)==0 && BoundShaderTextureMask(ShaderStage::kPixel)==0);
 const auto attempts=lookups;for(int i=0;i<1000;++i)RefreshBoundShaderResources();assert(lookups==attempts);
 // Streaming finishes without another SetShader; next draw-frame recovers.
 guest[0x2018]=2;++g_probe_frame;RefreshBoundShaderResources();
 if(BoundShaderHash(ShaderStage::kPixel)!=0x222)return 24;
 readable=false;++g_probe_frame;RefreshBoundShaderResources();
 BoundShaderAddress(ShaderStage::kPixel,&failed);assert(failed);
 readable=true;++g_probe_frame;RefreshBoundShaderResources();assert(BoundShaderHash(ShaderStage::kPixel)==0x222);
 assert(BindShader(ShaderStage::kPixel,0));RefreshBoundShaderResources();
 assert(BoundShaderAddress(ShaderStage::kPixel,&failed)==0 && !failed);
 ResetShaderResources();assert(g_registry.empty());
 assert(g_shader_retry_addresses[1]==0 && g_shader_retry_frames[1]==~u64{0});
}
'''
a.output.mkdir(parents=True,exist_ok=True)
captures=[]
capture_checks=''
for path in a.capture:
    data=path.read_bytes()
    virtual=int.from_bytes(data[4:8],'big');size=int.from_bytes(data[8:12],'big')
    start=virtual+(4 if int.from_bytes(data[virtual:virtual+4],'big')==size else 0)
    code=data[start:start+size]
    assert size and len(code)==size and size<0x8000
    capture_checks+='''{
      const uint8_t actual[]={'''+','.join(str(b) for b in code)+'''};
      header(0x3000);std::memcpy(guest.data()+0x3000,actual,sizeof(actual));
      ShaderResource resource;resource.owns_guest_memory=false;resource.stage=ShaderStage::kPixel;
      resource.guest_address=0x100;resource.physical_code_address=0x3000;
      resource.physical_code_size=sizeof(actual);resource.physical_code_hash=XXH3_64bits(actual,sizeof(actual));
      ++g_probe_frame;assert(PlacementShaderUnchanged(resource));
      for(size_t offset:{size_t(0),size_t(8),sizeof(actual)-1}) {
        guest[0x3000+offset]^=1;++g_probe_frame;assert(!PlacementShaderUnchanged(resource));
        guest[0x3000+offset]^=1;++g_probe_frame;assert(PlacementShaderUnchanged(resource));
      }
    }'''
    captures.append({'path':str(path),'sha256':hashlib.sha256(data).hexdigest(),'physical_bytes':size})
suffix=suffix.replace(' ResetShaderResources();assert(g_registry.empty());',
    ' ResetShaderResources();assert(g_registry.empty());'+capture_checks)
results=[]
for name,source,expected in (
 ('refresh',bodies,0),
 ('stale-cache',bodies.replace('if (PlacementShaderUnchanged(*existing)) return existing;','return existing;'),23),
 ('no-retry',bodies.replace('if (!addresses[i]) continue;','if (!addresses[i] || !resources[i]) continue;'),24)):
    cpp=a.output.resolve()/(name+'.cpp');exe=a.output.resolve()/(name+'.exe')
    cpp.write_text(prefix+registry+source+suffix)
    subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexglue-sdk/thirdparty/xxHash'),str(cpp),'-o',str(exe)],check=True)
    run=subprocess.run([str(exe)]);assert run.returncode==expected,(name,run.returncode)
    results.append({'case':name,'exit_code':run.returncode})
(a.output/'verification.json').write_text(json.dumps({'game_launched':False,'production_bodies':True,'results':results,'captures':captures},indent=2))
print('PASS: changed program/slot mask, relocation, unreadable source, streaming retry, old generation retention, bounded checks and both mutations')
