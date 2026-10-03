"""Run TU23 viewport setter and emitted HLSL math offline, without a game."""
import argparse
import json
from pathlib import Path
import subprocess


def body(s, signature):
    start = s.index(signature)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('common', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
original = body((root/'rexlego/generated/default/legodimensions_recomp.70.cpp').read_text(),
                'DEFINE_REX_FUNC(sub_83FB8FD0)')
helper = body(a.common.read_text(), 'float4 LegoViewportPosition(')
draw = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
key = body(draw, 'struct PipelineKey {')+';\n'+body(draw, 'struct PipelineKeyHash {')+';\n'
cpp = a.output/'test.cpp'
cpp.write_text(r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using uint=uint32_t;
constexpr u32 kNativeRenderTargets=4,kNativeVertexStreams=16;
union PPCRegister {u64 u64;int64_t s64;u32 u32;int32_t s32;};
struct CR {bool eq=false;template<class T>void compare(T a,T b,int){eq=a==b;}};
struct PPCContext {PPCRegister r3{},r4{},r10{},r11{};CR cr6;int xer=0;};
std::array<u8,65536> ram{};
u32 Load(u32 p){return u32(ram[p])<<24|u32(ram[p+1])<<16|u32(ram[p+2])<<8|ram[p+3];}
void Store(u32 p,u32 v){for(u32 i=0;i<4;++i)ram[p+i]=u8(v>>(24-8*i));}
u64 Load64(u32 p){return (u64(Load(p))<<32)|Load(p+4);}
void Store64(u32 p,u64 v){Store(p,u32(v>>32));Store(p+4,u32(v));}
#define REX_LOAD_U32(p) Load(p)
#define REX_STORE_U32(p,v) Store(p,v)
#define REX_LOAD_U64(p) Load64(p)
#define REX_STORE_U64(p,v) Store64(p,v)
#define REX_FUNC_PROLOGUE() ((void)0)
#define DEFINE_REX_FUNC(n) void n(PPCContext& ctx,u8* base)
struct float2 {float x,y;};
float2 operator*(float2 a,float2 b){return {a.x*b.x,a.y*b.y};}
float2 operator*(float2 a,float b){return {a.x*b,a.y*b};}
float2 operator+(float2 a,float2 b){return {a.x+b.x,a.y+b.y};}
struct float4 {float2 xy;float z,w;};
'''+original+'\n'+helper+'\n'+key+r'''
int main(){
 constexpr u32 dev=12288;
 for(u32 initial:{0u,0x80000u,0xFFFFFFFFu,0xA5317788u})
 for(u32 enable:{0u,1u,2u,0xFFFFFFFFu}){
  Store(dev+10564,initial);Store64(dev+16,0xF010000000000001);
  PPCContext c;c.r3.u32=dev;c.r4.u32=enable;sub_83FB8FD0(c,ram.data());
  assert(Load(dev+10572)==(enable?0x43Fu:0x400u));
  assert(Load(dev+10564)==((initial&~0x10000u)|(enable?0:0x10000u)));
  assert(Load64(dev+16)==0xF0100000000000A1);
 }
 for(float width:{128.f,720.f,1280.f,1920.f})
 for(float height:{128.f,720.f,1080.f})
 for(float w:{0.5f,1.f,2.f})
 for(float x:{-64.f,0.f,0.5f,33.f,1280.f})
 for(float y:{-64.f,0.f,0.5f,55.f,720.f}){
  float4 input={{x*w,y*w},0.3f*w,w};
  auto out=LegoViewportPosition(input,{2/width,-2/height},1);
  // Independent D3D12 viewport maps NDC back to the supplied pixel position.
  float pixel_x=(out.xy.x/out.w+1)*width/2;
  float pixel_y=(1-out.xy.y/out.w)*height/2;
  assert(std::abs(pixel_x-x)<0.001f&&std::abs(pixel_y-y)<0.001f);
  assert(out.z==input.z&&out.w==input.w);
  auto unchanged=LegoViewportPosition(input,{2/width,-2/height},0);
  assert(unchanged.xy.x==input.xy.x&&unchanged.xy.y==input.xy.y);
 }
 PipelineKey a,b;b.clip_disable=1;
 std::unordered_map<PipelineKey,int,PipelineKeyHash> cache;
 cache[a]=1;cache[b]=2;assert(cache.size()==2&&cache[a]==1&&cache[b]==2);
}
''')
exe = a.output/'test.exe'
subprocess.run(['clang++','-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
proof = {'passed': True, 'original_setter_cases': 16, 'pixel_mapping_cases': 900,
         'modes': ['TU23 0x43F', 'TU23 0x400'], 'shared_bytes': 624,
         'depth_clip_pipeline_cache': True, 'game_launched': False,
         'scope': 'CPU setter, exact emitted HLSL arithmetic, host viewport mapping and PSO identity; no pixel/character repair claim'}
(a.output/'verification.json').write_text(json.dumps(proof,indent=2)+'\n')
print('PASS: TU23 viewport setter, 900 screen coordinates, unchanged normal mode and distinct clip PSOs')
