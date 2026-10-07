"""Exercise native viewport routing/layout and optional unchanged TU23 setters.

Base tests are asset-free. Initialized generated sources add the exact unsigned
API setter, float restore setter, shared writer, AA18 reset and complete bulk
attachment setter. Reset bytes were read from the actual patched TU23 image;
provenance: .local-testing/reports/TU23-viewport-reference-byte-proof.json.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='g++')
p.add_argument('--no-oracle', action='store_true')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


hooks = (root / 'rexlego/src/gpu_native/hooks_state.cpp').read_text()
draw = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
native = '\n'.join(body(hooks, signature) for signature in
    ['bool NativeViewportRange(', 'bool NativeViewportDeviceArguments(', 'bool NativeViewportArguments(',
     'void SetScissorRectHook(', 'void SetScissorEnableHook('])
native += '\n' + body(hooks, 'class ScopedNativeViewportExtent') + ';'
textures = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
raw = '\n'.join(body(hooks, f'REX_HOOK_RAW({name})').replace(
    f'REX_HOOK_RAW({name})', f'void {name}(PPCContext& ctx,u8* base)', 1)
    for name in ('sub_83FBA978', 'sub_83FBA710'))
begin = draw.index('  const float viewport_width =')
end = draw.index('\n  std::array<plume::RenderVertexBufferView', begin)
record = draw[begin:end]
begin = draw.index('  if (!NativeViewportValid(')
end = draw.index('  // Trace attempted bindings', begin)
draw_guard = draw[begin:end]
names = ['__savefpr_26', '__restfpr_26', '__savegprlr_28', '__restgprlr_28',
         'sub_83FBA978', 'sub_83FBA9F8', 'sub_83FBA710', 'sub_83FBAA18', 'sub_83FBB110']
generated = sorted((root/'rexlego/generated/default').glob('*.cpp'))
has_oracle = not a.no_oracle and bool(generated)
originals = []
provenance = []
if has_oracle:
    definitions = {name: [] for name in names}
    signatures = {f'DEFINE_REX_FUNC({name}) {{\n': name for name in names}
    for path in generated:
        with path.open() as source_file:
            for line in source_file:
                if line in signatures:
                    definitions[signatures[line]].append(path)
    for name in names:
        if len(definitions[name]) != 1:
            raise RuntimeError(f'Expected one definition of {name}; found {definitions[name]}')
        path = definitions[name][0]
        code = body(path.read_text(), f'DEFINE_REX_FUNC({name})')
        provenance.append({'function': name, 'path': str(path.relative_to(root)),
                           'body_sha256': hashlib.sha256(code.encode()).hexdigest()})
        if name in ('sub_83FBA978', 'sub_83FBA710'):
            code = code.replace(f'DEFINE_REX_FUNC({name})', f'DEFINE_REX_FUNC(__imp__{name})', 1)
        originals.append(code)

shim = a.output / 'include/rex/types.h'
shim.parent.mkdir(parents=True, exist_ok=True)
shim.write_text(r'''
#pragma once
#include <bit>
#include <cstdint>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
template<class T>struct BigEndian {
 uint32_t raw=0;
 operator T()const{auto v=__builtin_bswap32(raw);return std::bit_cast<T>(v);}
 BigEndian& operator=(T v){raw=__builtin_bswap32(std::bit_cast<uint32_t>(v));return *this;}
};
using be_u32=BigEndian<uint32_t>;using be_i32=BigEndian<int32_t>;using be_f32=BigEndian<float>;
''')
source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include "gpu_native/d3d.h"
#include "gpu_native/scissor_state.h"
#include "gpu_native/cull_state.h"
using namespace legodimensions::gpu_native;
union PPCRegister {uint64_t u64;int64_t s64;uint32_t u32;int32_t s32;uint8_t u8;float f32;};
union FRegister {double f64;uint64_t u64;int64_t s64;};
struct XER {u8 ca=0;};
struct CR {bool eq=false,gt=false,lt=false;template<class T>void compare(T a,T b,XER){eq=a==b;gt=a>b;lt=a<b;}};
struct FPSCR {void disableFlushMode(){}};
struct PPCContext {
''' + ''.join(f'PPCRegister r{i}{{}};FRegister f{i}{{}};\n' for i in range(32)) + r'''
 u64 lr=0;XER xer;CR cr0,cr6;FPSCR fpscr;
};
std::array<u8,0x20000> ram{};
std::map<u32,u32> constants;
u32 Load(u32 at){if(at>=ram.size()){if(!constants.contains(at))std::fprintf(stderr,"unmapped oracle load %08X\n",at);assert(constants.contains(at));return constants.at(at);}
 assert(at+4ull<=ram.size());u32 v;std::memcpy(&v,ram.data()+at,4);return __builtin_bswap32(v);}
void Store(u32 at,u32 v){assert(at+4ull<=ram.size());v=__builtin_bswap32(v);std::memcpy(ram.data()+at,&v,4);}
u64 Load64(u32 at){return (u64(Load(at))<<32)|Load(at+4);}
void Store64(u32 at,u64 v){Store(at,u32(v>>32));Store(at+4,u32(v));}
void Float(u32 at,float value){Store(at,std::bit_cast<u32>(value));}
u64 Rotate(u64 v,int n){return std::rotl(v,n);}
u32 Rotate32(u32 v,int n){return std::rotl(v,n);}
double simde_mm_load_sd(const double* v){return *v;}
i32 simde_mm_cvttsd_si32(double v){return std::isfinite(v)&&v>=INT_MIN&&v<=INT_MAX?i32(v):INT_MIN;}
#define __builtin_rotateleft64 Rotate
#define __builtin_rotateleft32 Rotate32
#define REX_LOAD_U32(p) Load(u32(p))
#define REX_STORE_U32(p,v) Store(u32(p),u32(v))
#define REX_LOAD_U64(p) Load64(u32(p))
#define REX_STORE_U64(p,v) Store64(u32(p),u64(v))
#define REX_LOAD_U8(p) (ram.at(u32(p)))
#define REX_STORE_U8(p,v) (ram.at(u32(p))=u8(v))
#define REX_LOAD_U16(p) ((u32(ram.at(u32(p)))<<8)|ram.at(u32(p)+1))
#define REX_FUNC_PROLOGUE() ((void)0)
#define REXLOG_INFO(...) ((void)0)
#define DEFINE_REX_FUNC(n) void n(PPCContext& ctx,u8* base)
namespace rex::memory {
constexpr u32 kMemoryProtectRead=1,kMemoryProtectWrite=2,kMemoryProtectWriteCombine=8;
struct HeapAllocationInfo {u32 protect=3;};
}
struct Heap {
 std::set<u32> unreadable,readonly;u32 page_size()const{return 4096;}
 bool IsRangeCommittedReadable(u32 at,u32 bytes){
  if(!bytes||u64(at)+bytes>0x100000000ull)return false;
  if(at>=ram.size()){
   for(u64 x=at;x<u64(at)+bytes;x+=4)if(!constants.contains(u32(x)))return false;
   return true;
  }
  if(u64(at)+bytes>ram.size())return false;
  for(u64 page=at/4096;page<=(u64(at)+bytes-1)/4096;++page)
   if(unreadable.contains(u32(page)))return false;
  return true;
 }
 bool QueryProtect(u32 at,u32* protect){
  *protect=at>=ram.size()||readonly.contains(at/4096)?1:3;return true;
 }
}heap;
struct Memory {
 Heap* LookupHeap(u32){return &heap;}
 template<class T>T TranslateVirtual(u32 at){assert(at<ram.size());return reinterpret_cast<T>(ram.data()+at);}
}memory;
#define REX_KERNEL_MEMORY() (&::memory)
namespace legodimensions::gpu_native {
struct SurfaceMetadata {u32 width,height,host_width,host_height;bool surface=true;void* texture=reinterpret_cast<void*>(1);};
std::map<u32,SurfaceMetadata> surfaces;
SurfaceMetadata* FindTexture(u32 at){auto it=surfaces.find(at);return it==surfaces.end()?nullptr:&it->second;}
''' + body(textures, 'bool NativePromotedSurfaceExtent(') + r'''
struct HostDevice {static auto LockRecording(){return std::unique_lock(recording);}static inline std::recursive_mutex recording;};
''' + native + r'''
}
void sub_83FBA710(PPCContext&,u8*);void sub_83FBA978(PPCContext&,u8*);
void __imp__sub_83FBA978(PPCContext&,u8*);void __imp__sub_83FBA710(PPCContext&,u8*);
void sub_83FBA068(PPCContext& c,u8*){
 auto* d=memory.TranslateVirtual<D3DDevice*>(c.r3.u32);
 SetScissorRectHook(d,memory.TranslateVirtual<const D3DRect*>(c.r4.u32));
}
''' + raw + '\n' + '\n'.join(originals)
if not has_oracle:
    source += 'u32 original_calls=0;void __imp__sub_83FBA978(PPCContext&,u8*){++original_calls;}void __imp__sub_83FBA710(PPCContext&,u8*){}\n'
source += r'''
namespace plume {
using RenderRect=NativeScissorRect;
struct RenderViewport {float x,y,w,h,min_z,max_z;RenderViewport(float x,float y,float w,float h,float z,float m):x(x),y(y),w(w),h(h),min_z(z),max_z(m){}};
}
struct Commands {plume::RenderViewport viewport{0,0,0,0,0,0};NativeScissorRect scissor{};
 void setViewports(plume::RenderViewport v){viewport=v;}void setScissors(NativeScissorRect v){scissor=v;}};
''' + body(draw, 'plume::RenderRect CullDrawScissor(') + r'''
u32 host_records=0;
bool GuardedDraw(D3DDevice* device){
''' + draw_guard + r'''
 ++host_records;return true;
}
bool query_failed=false;
void AttemptDraw(D3DDevice* device){if(!GuardedDraw(device))query_failed=true;}
Commands Record(D3DDevice* device){
 Commands output;auto* commands=&output;const auto* state_bytes=reinterpret_cast<const u8*>(device);
 const bool viewport_candidate=false;struct{u32 width=64,height=48;}color;struct{u32 raster_control=0;}key;bool polygonal=true;
''' + record + r'''
 return output;
}
constexpr u32 device=0x1000,argument=0x9000,color_surface=0xA000,depth_surface=0xB000,stack=0x1F000;
D3DDevice& Device(){return *memory.TranslateVirtual<D3DDevice*>(device);}
PPCContext Context(){PPCContext c{};c.r1.u32=stack;c.r3.u32=device;c.r4.u32=argument;return c;}
void Api(float x,float y,u32 w,u32 h){auto* a=memory.TranslateVirtual<D3DViewport9*>(argument);
 a->x=u32(x);a->y=u32(y);a->width=w;a->height=h;a->min_z=0.25f;a->max_z=0.75f;
 auto c=Context();sub_83FBA978(c,ram.data());}
void Reset(){surfaces.clear();ram.fill(0);heap.unreadable.clear();heap.readonly.clear();
 Device().render_targets[0]=color_surface;Device().depth_stencil=depth_surface;
 auto* color=memory.TranslateVirtual<D3DSurface*>(color_surface);color->size_bits=((64-1)<<18)|((48-1)<<3);
 auto* depth=memory.TranslateVirtual<D3DSurface*>(depth_surface);depth->size_bits=((32-1)<<18)|((24-1)<<3);
}
int main(){
 Reset();assert(NativeViewportArguments(device,argument,stack));
 assert(!NativeViewportArguments(0,argument,stack));assert(!NativeViewportArguments(device,0,stack));
 assert(!NativeViewportArguments(device,argument,128));
 assert(!NativeViewportArguments(0xFFFFFFF0,argument,stack));
 heap.unreadable.insert(2);assert(!NativeViewportArguments(device,argument,stack));heap.unreadable.clear();
 heap.readonly.insert(3);assert(!NativeViewportArguments(device,argument,stack));heap.readonly.clear();
 heap.readonly.insert((stack-512)/4096);assert(!NativeViewportArguments(device,argument,stack));heap.readonly.clear();
 Device().render_targets[0]=0xFFFFFFF0;assert(!NativeViewportArguments(device,argument,stack));Reset();
 auto c=Context();c.r4.u32=0;auto before=ram;sub_83FBA978(c,ram.data());assert(ram==before);
 assert(NativeViewportInteger(12.75f)==12);assert(NativeViewportInteger(-12.75f)==-12);
 assert(NativeViewportInteger(INFINITY)==INT_MAX);assert(NativeViewportInteger(-INFINITY)==INT_MIN);
 assert(NativeViewportInteger(NAN)==INT_MIN);
 const auto saved_scope_ram=ram;
 try {ScopedNativeViewportExtent scope(&Device(),1280,720);throw 17;}
 catch(int value){assert(value==17);}
 assert(ram==saved_scope_ram); // Exception also restores transient tile state.
 Device().viewport.x=3.0f;Device().viewport.y=4.0f;Device().viewport.width=20.0f;Device().viewport.height=16.0f;
 auto recorded=Record(&Device());assert(recorded.viewport.x==3&&recorded.viewport.w==20);
 assert((recorded.scissor==NativeScissorRect{3,4,23,20}));
 Device().viewport.width=0.0f;assert(Record(&Device()).viewport.w==0); // No fabricated full-target viewport.
 assert(NativeViewportValid(3,4,20,16,0.25f,0.75f));
 assert(NativeViewportValid(3,4,0,16,0,1));assert(!NativeViewportValid(3,4,-1,16,0,1));
 for(float bad:{NAN,INFINITY,-INFINITY}){
  assert(!NativeViewportValid(bad,4,20,16,0,1));assert(!NativeViewportValid(3,bad,20,16,0,1));
  assert(!NativeViewportValid(3,4,bad,16,0,1));assert(!NativeViewportValid(3,4,20,bad,0,1));
  assert(!NativeViewportValid(3,4,20,16,bad,1));assert(!NativeViewportValid(3,4,20,16,0,bad));
 }
 assert(!NativeViewportValid(3,4,20,16,-1,1));assert(!NativeViewportValid(3,4,20,16,0,2));
 assert(NativeViewportValid(3,4,20,16,0.75f,0.25f));
 Device().viewport.min_z=0;Device().viewport.max_z=1;
 AttemptDraw(&Device());assert(!query_failed&&host_records==0&&float(Device().viewport.width)==0);
 Device().viewport.width=20;AttemptDraw(&Device());assert(!query_failed&&host_records==1);
 Device().viewport.width=NAN;AttemptDraw(&Device());assert(query_failed&&host_records==1);
'''
if has_oracle:
    source += r'''
 Reset();Api(3,4,40,30);
 assert(float(Device().viewport.x)==3&&float(Device().viewport.y)==4);
 assert(float(Device().viewport.width)==40&&float(Device().viewport.height)==30);
 assert(float(Device().viewport.min_z)==0.25f&&float(Device().viewport.max_z)==0.75f);
 assert(Load(device+10504)==std::bit_cast<u32>(20.0f)); // X scale
 assert(Load(device+10508)==std::bit_cast<u32>(23.0f)); // X offset
 assert(Load(device+10512)==std::bit_cast<u32>(-15.0f)); // Y scale
 assert(Load(device+10516)==std::bit_cast<u32>(19.0f));
 assert(Load(device+10520)==std::bit_cast<u32>(0.5f));assert(Load(device+10524)==std::bit_cast<u32>(0.25f));
 assert(Load(device+13048)==0&&(Load64(device+16)&0x07E00000)==0x07E00000);
 recorded=Record(&Device());assert(recorded.viewport.w==40&&recorded.viewport.h==30);
 // Real original private save28 -> float setter restore, same memory ABI.
 std::memcpy(ram.data()+argument,reinterpret_cast<u8*>(&Device().viewport),28);
 const auto saved=Device().viewport;Device().viewport={};c=Context();sub_83FBA9F8(c,ram.data());
 assert(std::memcmp(&saved,&Device().viewport,sizeof(saved))==0);
 // Deliberate former native memcpy makes internal float width subnormal.
 auto api=memory.TranslateVirtual<D3DViewport9*>(argument);api->x=3;api->y=4;api->width=40;api->height=30;
 api->min_z=0.25f;api->max_z=0.75f;
 std::memcpy(&Device().viewport,api,sizeof(*api));assert(float(Device().viewport.width)!=40);
 std::memcpy(ram.data()+argument,reinterpret_cast<u8*>(&Device().viewport),28);
 c=Context();sub_83FBA9F8(c,ram.data());assert(float(Device().viewport.width)==0);
 // Surface bounds, depth fallback, no attachment early return, reversed bounds.
 Reset();Api(60,40,100,100);assert(float(Device().viewport.width)==4&&float(Device().viewport.height)==8);
 Device().render_targets[0]=0;Api(3,4,100,100);assert(float(Device().viewport.width)==29&&float(Device().viewport.height)==20);
 const auto no_target=Device().viewport;Device().depth_stencil=0;Api(0,0,12,13);
 assert(std::memcmp(&no_target,&Device().viewport,sizeof(no_target))==0);
 Reset();Api(70,0,10,10);assert(float(Device().viewport.width)==0&&float(Device().viewport.height)==0);
 // Original tiled extent selection with exactly matching shadow attachments.
 Reset();ram[device+11068]=0x10;Store(device+13556,80);Store(device+13560,60);
 Api(0,0,100,100);assert(float(Device().viewport.width)==80&&float(Device().viewport.height)==60);
 // Actual patched TU23 default API bytes, observed by mapped-image probe.
 Reset();constants[0x824CAF44]=0;constants[0x824CAF48]=0;
 constants[0x824CAF4C]=65535;constants[0x824CAF50]=65535;
 constants[0x824CAF54]=0;constants[0x824CAF58]=0x3F800000;
 constants[0x824CAF5C]=0;constants[0x824CAF60]=0;constants[0x824CAF64]=65535;constants[0x824CAF68]=65535;
 c=Context();c.r4.u32=color_surface;sub_83FBAA18(c,ram.data());
 assert(float(Device().viewport.width)==64&&float(Device().viewport.height)==48);
 assert(float(Device().viewport.max_z)==1&&i32(Device().scissor.right)==65535);
 // Complete exact original bulk setter, including reset and native API route.
 auto* attachments=memory.TranslateVirtual<be_u32*>(argument);
 attachments[0]=depth_surface;attachments[1]=color_surface;
 attachments[2]=attachments[3]=attachments[4]=0;
 c=Context();c.r5.u32=0;sub_83FBB110(c,ram.data());
 assert(float(Device().viewport.width)==64&&float(Device().viewport.height)==48);
 recorded=Record(&Device());assert(recorded.viewport.w==64&&recorded.viewport.h==48);

 // Reproduce the reported menu: physical tile512, promoted host720.
 Reset();auto* tiled=memory.TranslateVirtual<D3DSurface*>(color_surface);
 tiled->size_bits=((1280-1)<<18)|((512-1)<<3);
 Api(0,0,1280,720);assert(float(Device().viewport.height)==512);
 surfaces[color_surface]={1280,512,1280,720};
 // Promotion alone cannot recover a previously clipped request. The real
 // scene must set its viewport after promotion; live bounded logs verify it.
 assert(float(Device().viewport.height)==512);
 ram[device+11068]=0xA0;Store(device+13556,17);Store(device+13560,19);
 const auto unchanged_size=u32(tiled->size_bits);
 const auto check_extent_restore=[&]{
  assert(ram[device+11068]==0xA0&&Load(device+13556)==17&&Load(device+13560)==19);
  assert(u32(tiled->size_bits)==unchanged_size);
 };
 Api(0,0,1280,720);assert(float(Device().viewport.height)==720);check_extent_restore();
 assert(Load(device+10512)==std::bit_cast<u32>(-360.0f));
 // Unadapted original is the exact512px regression negative oracle.
 c=Context();c.f1.f64=0;c.f2.f64=0;c.f3.f64=1280;c.f4.f64=720;c.f5.f64=0;c.f6.f64=1;
 __imp__sub_83FBA710(c,ram.data());assert(float(Device().viewport.height)==512);
 Api(0,0,1280,720);
 std::memcpy(ram.data()+argument,reinterpret_cast<u8*>(&Device().viewport),28);
 Device().viewport={};c=Context();sub_83FBA9F8(c,ram.data());
 assert(float(Device().viewport.height)==720);check_extent_restore();
 attachments=memory.TranslateVirtual<be_u32*>(argument);
 attachments[0]=depth_surface;attachments[1]=color_surface;attachments[2]=attachments[3]=attachments[4]=0;
 c=Context();c.r5.u32=0;sub_83FBB110(c,ram.data());
 assert(float(Device().viewport.height)==720);check_extent_restore();
 Api(12,700,2000,200);assert(float(Device().viewport.width)==1268&&float(Device().viewport.height)==20);
 check_extent_restore();
 Api(0,0,1280,0);assert(float(Device().viewport.height)==0);check_extent_restore();
 Api(0,800,1280,100);assert(float(Device().viewport.width)==0&&float(Device().viewport.height)==0);
 check_extent_restore();
 // Depth fallback uses its own promotion, never the preceding color target.
 Device().render_targets[0]=0;surfaces[depth_surface]={32,24,32,80};
 Api(0,0,100,100);assert(float(Device().viewport.width)==32&&float(Device().viewport.height)==80);
 check_extent_restore();
 // Unknown, ordinary, non-surface and unrepresentable metadata stay untouched.
 u32 ew=123,eh=456;assert(!NativePromotedSurfaceExtent(0,ew,eh)&&ew==123&&eh==456);
 surfaces[color_surface]={1280,512,1280,512};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 surfaces[color_surface]={1280,512,1280,720,false};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 surfaces[color_surface]={1280,512,1280,720,true,nullptr};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 surfaces[color_surface]={1280,512,0,720};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 surfaces[color_surface]={1280,512,16385,720};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 surfaces[color_surface]={1280,512,1280,32769};assert(!NativePromotedSurfaceExtent(color_surface,ew,eh));
 // Shared raw entry validates arguments before touching state.
 Reset();c=Context();c.r1.u32=128;before=ram;sub_83FBA710(c,ram.data());assert(ram==before);
 c=Context();heap.readonly.insert(3);before=ram;sub_83FBA710(c,ram.data());assert(ram==before);
 Reset();
 // API's valid512-byte stack guard must not grow to624 through its112-byte
 // frame. The shared writer uses its exact160-byte scratch, all on same page.
 heap.readonly.insert(0x1D);
 assert(NativeViewportArguments(device,argument,0x1E200));
 assert(NativeViewportDeviceArguments(device,0x1E190,160));
 api=memory.TranslateVirtual<D3DViewport9*>(argument);
 api->x=0;api->y=0;api->width=64;api->height=48;api->min_z=0;api->max_z=1;
 c=Context();c.r1.u32=0x1E200;sub_83FBA978(c,ram.data());
 assert(float(Device().viewport.width)==64&&float(Device().viewport.height)==48);
 Reset();
 // Repeated bulk binds/reset cannot resurrect the preceding small viewport.
 Api(3,4,5,6);attachments[0]=depth_surface;attachments[1]=color_surface;
 attachments[2]=attachments[3]=attachments[4]=0;
 c=Context();c.r5.u32=0;sub_83FBB110(c,ram.data());
 assert(float(Device().viewport.x)==0&&float(Device().viewport.width)==64);
'''
else:
    source += 'Reset();Api(3,4,40,30);assert(original_calls==1);\n'
source += '\n}\n'
if has_oracle:
    # Shared writer uses the actual image constant proved by the same probe.
    source = source.replace('Reset();assert(NativeViewportArguments',
        'constants[0x82005D7C]=0x3F000000;Reset();assert(NativeViewportArguments', 1)
cpp = a.output / 'viewport-shadow.cpp'
exe = a.output / 'viewport-shadow.exe'
cpp.write_text(source)
subprocess.run([a.compiler, '-std=c++20', '-O0', '-UNDEBUG', '-I', str(a.output/'include'),
                '-I', str(root/'rexlego/src'), str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe.resolve())], check=True, timeout=20)
report = {'passed': True, 'actual_native_raw_guard_and_recording': True,
          'actual_d3d_layout': True, 'actual_original_tu23_setters_and_bulk': has_oracle,
          'legacy_uint_memcpy_negative': has_oracle,
          'promoted_tile_extent_and_512px_regression_negative': has_oracle,
          'actual_metadata_only_extent_query': True,
          'original_bodies': provenance,
          'image_constants_provenance': '.local-testing/reports/TU23-viewport-reference-byte-proof.json',
          'limits': 'Bounded CPU fixture; no game or GPU execution'}
(a.output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
