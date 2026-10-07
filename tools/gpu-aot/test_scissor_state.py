"""Execute native setters/draw scissor code; optionally compare actual TU23 bodies.

The base fixture needs no SDK, generated game code, assets or graphics device.
When the two generated TU23 files exist, execute their unchanged original
SetScissorRect and enable bodies with bounded big-endian memory as an oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='g++')
p.add_argument('--no-oracle', action='store_true', help='Run the asset-free base fixture only')
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
native_setters = '\n'.join(body(hooks, sig) for sig in
    ['void SetScissorRectHook(', 'void SetScissorEnableHook('])
start = draw.index('  const auto effective_scissor = DecodeNativeScissor(')
end = draw.index('\n  std::array<plume::RenderVertexBufferView', start)
native_draw = draw[start:end]
assert 'REX_HOOK(sub_83FBA968, legodimensions::gpu_native::SetScissorEnableHook)' in hooks
generated = sorted((root/'rexlego/generated/default').glob('*.cpp'))
has_oracle = not a.no_oracle and bool(generated)
oracle = ''
provenance = []
if has_oracle:
    names = ['sub_83FBA068', 'sub_83FBA968']
    definitions = {name: [] for name in names}
    signatures = {f'DEFINE_REX_FUNC({name}) {{\n': name for name in names}
    for path in generated:
        with path.open() as source_file:
            for line in source_file:
                if line in signatures:
                    definitions[signatures[line]].append(path)
    bodies = []
    for name in names:
        if len(definitions[name]) != 1:
            raise RuntimeError(f'Expected one definition of {name}; found {definitions[name]}')
        path = definitions[name][0]
        code = body(path.read_text(), f'DEFINE_REX_FUNC({name})')
        bodies.append(code)
        provenance.append({'function': name, 'path': str(path.relative_to(root)),
                           'body_sha256': hashlib.sha256(code.encode()).hexdigest()})
    oracle = '\n'.join(bodies)

source = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include "gpu_native/scissor_state.h"
#include "gpu_native/cull_state.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
struct be_u32 {
 u32 raw=0;operator u32()const{return __builtin_bswap32(raw);}
 be_u32& operator=(u32 v){raw=__builtin_bswap32(v);return *this;}
};
struct be_i32 {
 u32 raw=0;operator i32()const{return i32(__builtin_bswap32(raw));}
 be_i32& operator=(i32 v){raw=__builtin_bswap32(u32(v));return *this;}
};
struct D3DViewport9 {be_u32 x,y,width,height,min_z,max_z;};
struct be_f32 {
 u32 raw=0;operator float()const{return std::bit_cast<float>(__builtin_bswap32(raw));}
 be_f32& operator=(float v){raw=__builtin_bswap32(std::bit_cast<u32>(v));return *this;}
};
struct D3DViewportState {be_f32 x,y,width,height,min_z,max_z;};
struct D3DRect {be_i32 left,top,right,bottom;};
struct D3DDevice {u8 prefix[13024]{};D3DViewportState viewport;be_u32 reserved;D3DRect scissor;};
static_assert(offsetof(D3DDevice,scissor)==13052);
using namespace legodimensions::gpu_native;
namespace plume {using RenderRect=NativeScissorRect;}
struct Commands {NativeScissorRect result;void setScissors(NativeScissorRect r){result=r;}};
''' + native_setters + '\n' + body(draw, 'plume::RenderRect CullDrawScissor(') + r'''
NativeScissorRect NativeDraw(D3DDevice* device,u32 width,u32 height,
                            u32 raster_control=0,bool polygonal=true){
 Commands recorded;auto* commands=&recorded;
 const auto* state_bytes=reinterpret_cast<const u8*>(device);
 struct {u32 width,height;}color{width,height};
 struct {u32 raster_control;}key{raster_control};
''' + native_draw + r'''
 return recorded.result;
}
// Exact removed draw expression, preserved as the deliberate negative control.
NativeScissorRect LegacyDraw(D3DDevice* device,u32 width,u32 height){
 const i32 scissor_right=static_cast<i32>(device->scissor.right);
 const i32 scissor_bottom=static_cast<i32>(device->scissor.bottom);
 return {static_cast<i32>(device->scissor.left),static_cast<i32>(device->scissor.top),
   scissor_right>0?scissor_right:static_cast<i32>(width),
   scissor_bottom>0?scissor_bottom:static_cast<i32>(height)};
}
D3DRect Rect(i32 l,i32 t,i32 r,i32 b){D3DRect v;v.left=l;v.top=t;v.right=r;v.bottom=b;return v;}
D3DViewport9 View(u32 x,u32 y,u32 w,u32 h){D3DViewport9 v;v.x=x;v.y=y;v.width=w;v.height=h;return v;}
// This fixture supplies API inputs numerically. The separate viewport fixture
// executes the production RAW routing and the exact original TU23 setters.
void SetViewportFixture(D3DDevice* d,const D3DViewport9* v){if(!d||!v)return;
 d->viewport.x=float(u32(v->x));d->viewport.y=float(u32(v->y));
 d->viewport.width=float(u32(v->width));d->viewport.height=float(u32(v->height));}
void Expect(D3DDevice& d,NativeScissorRect r){assert(NativeDraw(&d,64,48)==r);}
'''
if has_oracle:
    source += r'''
union PPCRegister {uint64_t u64;int64_t s64;uint32_t u32;int32_t s32;float f32;};
union FRegister {double f64;uint64_t u64;int64_t s64;};
struct CR {bool eq=false,gt=false,lt=false;template<class T>void compare(T a,T b,int){eq=a==b;gt=a>b;lt=a<b;}};
struct FPSCR {void disableFlushMode(){}};
struct PPCContext {
''' + ''.join(f'PPCRegister r{i}{{}};FRegister f{i}{{}};\n' for i in range(32)) + r'''
 u64 lr=0;int xer=0;CR cr0,cr6;FPSCR fpscr;
};
std::array<u8,0x20000> ram{};
u32 Load(u32 at){assert(at+4ull<=ram.size());u32 v;std::memcpy(&v,ram.data()+at,4);return __builtin_bswap32(v);}
void Store(u32 at,u32 v){assert(at+4ull<=ram.size());v=__builtin_bswap32(v);std::memcpy(ram.data()+at,&v,4);}
u64 Load64(u32 at){return (u64(Load(at))<<32)|Load(at+4);}
void Store64(u32 at,u64 v){Store(at,u32(v>>32));Store(at+4,u32(v));}
u64 Rotate(u64 x,int n){return std::rotl(x,n);}
double simde_mm_load_sd(const double* x){return *x;}
i32 simde_mm_cvttsd_si32(double x){return std::isfinite(x)&&x>=INT_MIN&&x<=INT_MAX?i32(x):INT_MIN;}
#define __builtin_rotateleft64 Rotate
#define REX_LOAD_U32(p) Load(u32(p))
#define REX_STORE_U32(p,v) Store(u32(p),u32(v))
#define REX_LOAD_U64(p) Load64(u32(p))
#define REX_STORE_U64(p,v) Store64(u32(p),u64(v))
#define REX_FUNC_PROLOGUE() ((void)0)
#define DEFINE_REX_FUNC(n) void n(PPCContext& ctx,u8* base)
void sub_83FB7AB8(PPCContext&,u8*){} // Packet emission has no shadow effect.
''' + oracle + r'''
NativeScissorRect Oracle(D3DDevice& d){
 constexpr u32 address=0x1000;
 // Native and original TU23 now share the same floating point device shadow.
 Store(address+13024,std::bit_cast<u32>(float(d.viewport.x)));
 Store(address+13028,std::bit_cast<u32>(float(d.viewport.y)));
 Store(address+13032,std::bit_cast<u32>(float(d.viewport.width)));
 Store(address+13036,std::bit_cast<u32>(float(d.viewport.height)));
 Store(address+13052,u32(i32(d.scissor.left)));Store(address+13056,u32(i32(d.scissor.top)));
 Store(address+13060,u32(i32(d.scissor.right)));Store(address+13064,u32(i32(d.scissor.bottom)));
 PPCContext c{};c.r1.u32=0x1F000;c.r3.u32=address;
 c.r4.u32=u32(*reinterpret_cast<be_u32*>(reinterpret_cast<u8*>(&d)+12288));
 sub_83FBA968(c,ram.data());
 auto tl=Load(address+10436),br=Load(address+10440);
 i32 l=std::min(i32(tl&32767),64),t=std::min(i32((tl>>16)&32767),48);
 return {l,t,std::max(l,std::min(i32(br&32767),64)),
             std::max(t,std::min(i32((br>>16)&32767),48))};
}
'''
source += r'''
int main(){
 D3DDevice d{};
 // Detached-device zero initialization resets enable; no global cache survives.
 auto view=View(0,0,64,48);SetViewportFixture(&d,&view);
 auto requested=Rect(8,7,20,19);SetScissorRectHook(&d,&requested);
 Expect(d,{0,0,64,48});assert(LegacyDraw(&d,64,48)!=NativeDraw(&d,64,48));
 SetScissorEnableHook(&d,1);Expect(d,{8,7,20,19});
 SetScissorEnableHook(&d,0);Expect(d,{0,0,64,48});
 SetScissorEnableHook(&d,2);Expect(d,{8,7,20,19}); // All nonzero enables.
 assert(i32(d.scissor.left)==8&&i32(d.scissor.right)==20);
 requested=Rect(0,0,0,0);SetScissorRectHook(&d,&requested);
 Expect(d,{0,0,0,0});assert(LegacyDraw(&d,64,48)!=NativeDraw(&d,64,48));
 requested=Rect(0,0,32,0);SetScissorRectHook(&d,&requested);Expect(d,{0,0,32,0});
 requested=Rect(40,30,10,20);SetScissorRectHook(&d,&requested);Expect(d,{40,30,40,30});
 requested=Rect(-10,-20,100,80);SetScissorRectHook(&d,&requested);Expect(d,{0,0,64,48});
 view=View(4,5,20,16);SetViewportFixture(&d,&view);Expect(d,{4,5,24,21});
 requested=Rect(8,9,30,40);SetScissorRectHook(&d,&requested);Expect(d,{8,9,24,21});
 SetScissorEnableHook(&d,0);Expect(d,{4,5,24,21});
 view=View(60,40,UINT32_MAX,UINT32_MAX);SetViewportFixture(&d,&view);Expect(d,{60,40,64,48});
 view=View(UINT32_MAX,UINT32_MAX,1,1);SetViewportFixture(&d,&view);Expect(d,{64,48,64,48});
 view=View(4,5,0,0);SetViewportFixture(&d,&view);Expect(d,{4,5,4,5});
 auto saved=d;SetViewportFixture(nullptr,&view);SetViewportFixture(&d,nullptr);
 SetScissorRectHook(nullptr,&requested);SetScissorRectHook(&d,nullptr);
 SetScissorEnableHook(nullptr,1);assert(std::memcmp(&d,&saved,sizeof(d))==0);
 d={};view=View(0,0,64,48);SetViewportFixture(&d,&view);Expect(d,{0,0,64,48});
 assert((NativeDraw(&d,64,48,kNativeCullCandidate|3)==NativeScissorRect{0,0,0,0}));
'''
if has_oracle:
    source += r'''
 std::mt19937 random(0x83FBA068);
 for(u32 n=0;n<2000;++n){
  view=View(random()%64,random()%48,random()%65,random()%49);
  SetViewportFixture(&d,&view);
  requested=Rect(random()%80,random()%60,random()%80,random()%60);
  SetScissorRectHook(&d,&requested);
  SetScissorEnableHook(&d,n%3);assert(NativeDraw(&d,64,48)==Oracle(d));
  const auto before=d.scissor;
  SetScissorEnableHook(&d,0);assert(NativeDraw(&d,64,48)==Oracle(d));
  SetScissorEnableHook(&d,1);assert(NativeDraw(&d,64,48)==Oracle(d));
  assert(std::memcmp(&before,&d.scissor,sizeof(before))==0);
 }
'''
source += '\n}\n'
cpp = a.output / 'scissor-state.cpp'
exe = a.output / 'scissor-state.exe'
cpp.write_text(source)
subprocess.run([a.compiler, '-std=c++20', '-O0', '-UNDEBUG',
                '-I', str(root / 'rexlego/src'), str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe.resolve())], check=True, timeout=20)
report = {'passed': True, 'actual_native_setters_and_draw_section': True,
          'actual_tu23_oracle': has_oracle, 'oracle_cases': 6000 if has_oracle else 0,
          'original_bodies': provenance,
          'legacy_negative_cases': ['disabled stale rect', 'enabled empty rect'],
          'limits': 'CPU recording only; no game or GPU'}
(a.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
