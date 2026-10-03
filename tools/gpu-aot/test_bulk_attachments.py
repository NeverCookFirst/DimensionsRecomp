"""Exercise the actual bulk attachment hook, including a clobbered guest r3."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
text = (root/'rexlego/src/gpu_native/hooks_state.cpp').read_text()
start = text.index('REX_HOOK_RAW(sub_83FBB110)')
pos = text.index('{', start) + 1
end, depth = pos, 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
hook = text[pos:end-1]
code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
using u8=uint8_t; using u32=uint32_t;
struct be_u32 {u32 raw; operator u32()const{return __builtin_bswap32(raw);}
 be_u32& operator=(u32 v){raw=__builtin_bswap32(v);return *this;}};
struct PPCRegister {u32 u32;};
struct PPCContext {PPCRegister r3,r4;u32 result;};
namespace legodimensions::gpu_native {
constexpr u32 kNativeRenderTargets=4;
struct D3DDevice { std::array<u8,12816> padding;be_u32 render_targets[4],depth_stencil; };
std::array<u32,5> bound;
void BindRenderTarget(u32 i,u32 v){bound[i]=v;}
void BindDepthStencil(u32 v){bound[4]=v;}
struct HostDevice {static auto LockRecording(){static std::recursive_mutex m;return std::unique_lock(m);}};
}
std::array<u8,65536> memory{};
std::array<u32,5> desired;
u32 original_calls=0;
void __imp__sub_83FBB110(PPCContext& ctx,u8* base){
 assert(base==memory.data());assert(ctx.r3.u32==512);assert(ctx.r4.u32==256);
 ++original_calls;
 auto* d=reinterpret_cast<legodimensions::gpu_native::D3DDevice*>(base+ctx.r3.u32);
 for(u32 i=0;i<4;++i)d->render_targets[i]=desired[i];
 d->depth_stencil=desired[4];
 ctx.r3.u32=0xDEADBEEF;ctx.result=0x1234ABCD;
}
#define REXLOG_INFO(...) ((void)0)
void Hook(PPCContext& ctx,u8* base){
''' + hook + r'''
}
int main(){
 for(u32 mask=0;mask<32;++mask){
   for(u32 i=0;i<5;++i){desired[i]=(mask&(1u<<i))?0x30200000+4096*i:0;
     legodimensions::gpu_native::bound[i]=0xBAD00000+i;}
   PPCContext ctx{{512},{256},0};Hook(ctx,memory.data());
   assert(original_calls==mask+1);
   assert(legodimensions::gpu_native::bound==desired);
   assert(ctx.r3.u32==0xDEADBEEF && ctx.result==0x1234ABCD);
 }
}
'''
cpp=a.output/'bulk.cpp'; exe=a.output/'bulk.exe'
cpp.write_text(code)
subprocess.run(['clang++','-std=c++20',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
report={'passed':True,'null_attachment_combinations':32,'actual_hook_body':True,
        'original_xdk_call_mocked':True,'original_called_once':True,
        'saved_device_survives_guest_r3_clobber':True,
        'all_native_attachments_follow_guest_shadow':True}
(a.output/'bulk-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
