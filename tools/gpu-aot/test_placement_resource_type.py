"""Execute actual TU23 GetResourceType / XGOffsetResource with native type hooks."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
def body(path,signature):
 text=(root/path).read_text();start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
get=body('rexlego/generated/default/legodimensions_recomp.209.cpp','DEFINE_REX_FUNC(sub_83FC49E8)')
get=get.replace('DEFINE_REX_FUNC(sub_83FC49E8)','void __imp__sub_83FC49E8(PPCContext& ctx,u8* base)')
offset=body('rexlego/generated/default/legodimensions_recomp.314.cpp','DEFINE_REX_FUNC(sub_83F9B640)')
offset=offset.replace('DEFINE_REX_FUNC(sub_83F9B640)','void sub_83F9B640(PPCContext& ctx,u8* base)')
hook=body('rexlego/src/gpu_native/hooks_buffer.cpp','REX_HOOK_RAW(sub_83FC49E8)')
hook=hook.replace('REX_HOOK_RAW(sub_83FC49E8)','void sub_83FC49E8(PPCContext& ctx,u8* base)')
buf=body('rexlego/src/gpu_native/buffers.cpp','u32 NativeBufferType(')
tex=body('rexlego/src/gpu_native/textures.cpp','u32 NativeTextureType(')
h=a.output/'placement-resource-type.cpp'
h.write_text(r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
union PPCRegister {u64 u64;int64_t s64;u32 u32;int32_t s32;u8 u8;};
struct XER {u8 ca=0;};
struct CR {bool eq=false,gt=false;template<class T>void compare(T a,T b,XER){eq=a==b;gt=a>b;}};
struct PPCContext {
'''+''.join(f' PPCRegister r{i}{{}};\n' for i in range(32))+r'''
 u64 lr=0;PPCRegister ctr{};XER xer{};CR cr0{},cr6{};
};
#define REX_FUNC_PROLOGUE() ((void)0)
#define REXLOG_INFO(...) ((void)0)
std::array<u8,65536> ram{};
u32 Load(u32 p){return (u32(ram[p])<<24)|(u32(ram[p+1])<<16)|(u32(ram[p+2])<<8)|ram[p+3];}
void Store(u32 p,u32 v){for(u32 i=0;i<4;++i)ram[p+i]=u8(v>>(24-8*i));}
void Store64(u32 p,u64 v){Store(p,u32(v>>32));Store(p+4,u32(v));}
u64 Load64(u32 p){return (u64(Load(p))<<32)|Load(p+4);}
#define REX_LOAD_U32(p) Load(p)
#define REX_STORE_U32(p,v) Store(p,v)
#define REX_LOAD_U64(p) Load64(p)
#define REX_STORE_U64(p,v) Store64(p,v)
u32 texture_offset_calls=0,last_texture=0,last_base_delta=0,last_mip_delta=0;
void sub_83F9B5D8(PPCContext& c,u8*){++texture_offset_calls;last_texture=c.r3.u32;last_base_delta=c.r4.u32;last_mip_delta=c.r5.u32;}
namespace legodimensions::gpu_native {
struct Buffer {bool owns_guest_memory;u32 kind;};struct Texture {bool owns_guest_memory;u32 d3d_type;};
std::shared_ptr<Buffer> buffer;std::shared_ptr<Texture> texture;
auto FindBuffer(u32)->std::shared_ptr<Buffer>{return buffer;}
auto FindTexture(u32)->std::shared_ptr<Texture>{return texture;}
bool IsNativeBuffer(u32){return bool(buffer);}
'''+buf+'\n'+tex+'\n}\n'+get+'\n'+hook+'\n'+offset+r'''
void Reset(u32 common,u32 fetch5=0){ram.fill(0);Store(1024,common);Store(1048,0x2003);Store(1056,0xAABBCCDD);Store(1072,fetch5);texture_offset_calls=0;}
PPCContext RunOffset(){PPCContext c{};c.r1.u32=60000;c.r3.u32=1024;c.r4.u32=0x1000;sub_83F9B640(c,ram.data());return c;}
int main(){
 using namespace legodimensions::gpu_native;
 // The original owner relocation must update +24 for adopted VB, not +32.
 buffer=std::make_shared<Buffer>(Buffer{false,6});Reset(0x200001);RunOffset();
 assert(Load(1048)==0x3003&&Load(1056)==0xAABBCCDD);
 // Native-owned compatibility override remains as before.
 buffer->owns_guest_memory=true;Reset(6);RunOffset();
 assert(Load(1048)==0x2003&&Load(1056)==0xAABBDCDD);
 buffer->owns_guest_memory=false;buffer->kind=7;Reset(0x80000002);RunOffset();
 assert(Load(1048)==0x3003&&Load(1056)==0xAABBCCDD);
 buffer.reset();texture=std::make_shared<Texture>(Texture{false,3});
 const std::array<u32,4> types{20,3,17,18};
 for(u32 dimension=0;dimension<4;++dimension){
  Reset(3,dimension<<9);Store(1056,0);PPCContext c{};c.r3.u32=1024;sub_83FC49E8(c,ram.data());assert(c.r3.u32==types[dimension]);
  RunOffset();assert(texture_offset_calls==1&&last_texture==1024&&last_base_delta==0x1000&&last_mip_delta==0);
 }
 Reset(3,(1u<<9));Store(1056,0x400);PPCContext c{};c.r3.u32=1024;sub_83FC49E8(c,ram.data());assert(c.r3.u32==19);
 texture->owns_guest_memory=true;texture->d3d_type=18;c.r3.u32=1024;sub_83FC49E8(c,ram.data());assert(c.r3.u32==18);
 // Deliberately recreate the old adopted-VB override to demonstrate wrong write.
 texture.reset();buffer=std::make_shared<Buffer>(Buffer{true,6});Reset(1);RunOffset();
 assert(Load(1048)==0x2003&&Load(1056)==0xAABBDCDD);
}
''')
exe=a.output/'placement-resource-type.exe'
subprocess.run(['clang++','-std=c++20',str(h),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
(a.output/'verification.json').write_text(json.dumps({'actual_generated_functions':['83FC49E8','83F9B640'],
 'actual_native_functions':['NativeBufferType','NativeTextureType','83FC49E8 hook'],
 'checks':['borrowed VB correct +24','owned VB compatibility +32','borrowed IB +24',
 'four texture dimensions','stacked texture','owned texture compatibility','old bug reproduced'],
 'runtime_title_idle_not_proven':True,'passed':True},indent=2))
print('Passed actual generated relocation/type functions: borrowed VB/IB use +24; owned-header overrides preserved.')
