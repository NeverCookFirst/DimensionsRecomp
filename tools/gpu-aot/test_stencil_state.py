"""Execute TU23 stencil leaves and the native decoder without launching a game."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
def body(s, sig):
    start = s.index(sig)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]

image = (root / 'xexdump/dump/default.bin').read_bytes()
table = [struct.unpack_from('>III', image, 0x847F9B18-0x82000000+12*k)
         for k in range(27, 43)]
functions = {}
for p in (root / 'rexlego/generated/default').glob('*.cpp'):
    s = p.read_text()
    for _, setter, _ in table:
        sig = f'DEFINE_REX_FUNC(sub_{setter:08X})'
        if sig in s:
            f = body(s, sig)
            instructions = re.findall(r'^\s*// ([^\n]+)', f, re.M)
            for instruction in instructions:
                branch = re.match(r'b[l]? 0x([a-fA-F0-9]+)', instruction)
                if branch:
                    assert setter <= int(branch[1],16) < setter+4*len(instructions)
                assert not re.match(r'bctrl|bctr', instruction)
            assert not re.search(r'\bsub_\w+\(ctx, base\)', f)
            functions[setter] = f
assert len(functions) == 16
initializer = body((root/'rexlego/src/gpu_native/hooks_device.cpp').read_text(),
                   'bool InitializeStencilDefaults(')
rebind = body((root/'rexlego/generated/default/legodimensions_recomp.324.cpp').read_text(),
              'DEFINE_REX_FUNC(sub_83FBAE38)')
start = rebind.index('\t// lwz r9,12308(r31)')
end = rebind.index('\t// ld r11,16(r31)', rebind.index('\t// stw r11,10548(r31)', rebind.index('\t// lwz r11,12312(r31)', start)))
oracle = 'void OriginalRebind(PPCContext& ctx,u8* base) { PPCRegister temp{};\n' + rebind[start:end] + '\n}\n'
assert 'RebindDepthStencilControl' in (root/'rexlego/src/gpu_native/hooks_state.cpp').read_text()
source = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "gpu_native/stencil_state.h"
#include "gpu_native/depth_state.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
union PPCRegister {uint64_t u64;int64_t s64;uint32_t u32;int32_t s32;uint8_t u8;};
struct XER {u8 ca=0;};
struct CR {bool eq=false;template<class T>void compare(T a,T b,XER){eq=a==b;}};
struct PPCContext {
''' + ''.join(f'PPCRegister r{i}{{}};\n' for i in range(32)) + r'''
 XER xer;CR cr0,cr6;
};
std::array<u8,65536> ram{};
u32 Load(u32 p){return (u32(ram[p])<<24)|(u32(ram[p+1])<<16)|(u32(ram[p+2])<<8)|ram[p+3];}
void Store(u32 p,u32 v){for(u32 i=0;i<4;++i)ram[p+i]=u8(v>>(24-8*i));}
u64 Load64(u32 p){return (u64(Load(p))<<32)|Load(p+4);}
void Store64(u32 p,u64 v){Store(p,u32(v>>32));Store(p+4,u32(v));}
#define REX_LOAD_U32(p) Load(p)
#define REX_LOAD_U8(p) (ram[p])
#define REX_STORE_U32(p,v) Store(p,v)
#define REX_LOAD_U64(p) Load64(p)
#define REX_STORE_U64(p,v) Store64(p,v)
#define REX_STORE_U8(p,v) (ram[p]=u8(v))
#define REX_FUNC_PROLOGUE() ((void)0)
#define DEFINE_REX_FUNC(n) void n(PPCContext& ctx,u8* base)
#define REXLOG_INFO(...) ((void)0)
''' + '\n'.join(functions.values()) + oracle + r'''
struct be_u32 {u32 raw;operator u32()const{return __builtin_bswap32(raw);}};
constexpr u32 kRenderStateTable=4096;
std::vector<u32> calls;
struct Dispatcher {u32 missing=0;u32 GetFunction(u32 p){return p==missing?0:p;}} dispatcher;
struct Kernel {auto function_dispatcher(){return &dispatcher;}} fixture_kernel;
#define REX_KERNEL_STATE() (&fixture_kernel)
namespace rex::ppc {
template<class T>void GuestToHostFunction(u32 f,u32 device,u32 value){
 PPCContext c{};c.r3.u32=device;c.r4.u32=value;calls.push_back(f);
''' + '\n'.join((' if' if i==0 else ' else if') +
                f'(f==0x{t[1]:08X})sub_{t[1]:08X}(c,ram.data());'
                for i,t in enumerate(table)) + r'''
 else assert(false);
}
}
''' + initializer + r'''
using namespace legodimensions::gpu_native;
int main(){
 constexpr u32 device=12288;
''' + ''.join(f'Store(kRenderStateTable+12*{27+i}+4,{t[1]}u);Store(kRenderStateTable+12*{27+i}+8,{t[2]}u);\n'
               for i,t in enumerate(table)) + r'''
 assert(InitializeStencilDefaults(device,ram.data()));assert(calls.size()==16);
 assert(Load(device+10496)==0x00FFFF00 && Load(device+10492)==0x00FFFF00);
 assert(Load(device+10548)==0x00700700); // ALWAYS both faces, KEEP, disabled.
 dispatcher.missing=0x83FB8828;calls.clear();
 assert(!InitializeStencilDefaults(device,ram.data()));assert(calls.empty());
 dispatcher.missing=0;
 // Actual enable leaf preserves requested enable even with no depth target.
 PPCContext c{};c.r3.u32=device;c.r4.u32=1;
 sub_83FB8570(c,ram.data());assert(Load(device+12312)==1);
 assert((Load(device+10548)&1)==0);
 Store(device+12832,1);sub_83FB8570(c,ram.data());
 assert(Load(device+10548)&1);
 for(u32 ref=0;ref<256;++ref){
  c.r4.u32=ref;sub_83FB8788(c,ram.data());
  assert(DecodeStencilState(Load(device+10548),Load(device+10496),true,true).reference==ref);
 }
 // Exercise independent front/back compare and op fields through real setters.
 for(u32 value=0;value<8;++value){
  c.r4.u32=value;
  sub_83FB85E8(c,ram.data());sub_83FB8618(c,ram.data());
  sub_83FB8650(c,ram.data());sub_83FB8688(c,ram.data());
  auto s=DecodeStencilState(Load(device+10548),Load(device+10496),true,true);
  assert(s.enabled&&s.front.comparison==value&&s.front.fail==value&&s.front.pass==value&&s.front.depth_fail==value);
  assert(s.back==s.front);
  c.r4.u32=7-value;
  sub_83FB86B8(c,ram.data());sub_83FB86E8(c,ram.data());
  sub_83FB8720(c,ram.data());sub_83FB8758(c,ram.data());
  c.r4.u32=1;sub_83FB85B0(c,ram.data());
  s=DecodeStencilState(Load(device+10548),Load(device+10496),true,true);
  assert(s.back.comparison==7-value&&s.back.fail==7-value&&s.back.pass==7-value&&s.back.depth_fail==7-value);
  assert(DecodeStencilState(Load(device+10548),Load(device+10496),true,false).back==s.front);
  assert(!DecodeStencilState(Load(device+10548),Load(device+10496),false,true).enabled);
  c.r4.u32=0;sub_83FB85B0(c,ram.data());
 }
 for(u32 mask=0;mask<256;++mask){
  c.r4.u32=mask;sub_83FB87A8(c,ram.data());
  c.r4.u32=255-mask;sub_83FB87C8(c,ram.data());
  auto s=DecodeStencilState(Load(device+10548),Load(device+10496),true,true);
  assert(s.read_mask==mask&&s.write_mask==255-mask);
 }
 // Execute the original DS-rebind tail, including its PPC masking, against
 // the host helper. Preserve requested bits across unbind/rebind sequences.
 for(u32 control : {0u,0x00700700u,0xC07E07B3u,0xFFFFFFFFu})
 for(u32 depth_request : {0u,1u,2u,3u,0xFFFFFFFFu})
 for(u32 stencil_request : {0u,1u,2u,3u,0xFFFFFFFFu})
 for(u32 surface : {0u,1u,0xAB001234u}) {
  Store(device+10548,control);Store(device+12308,depth_request);
  Store(device+12312,stencil_request);Store(device+12832,surface);
  c.r31.u32=device;OriginalRebind(c,ram.data());
  assert(Load(device+10548)==RebindDepthStencilControl(control,depth_request,stencil_request,surface!=0));
  assert(Load(device+12308)==depth_request&&Load(device+12312)==stencil_request);
 }
}
'''
cpp = out/'test.cpp'
cpp.write_text(source)
exe = out/'test.exe'
subprocess.run(['clang++','-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
(out/'verification.json').write_text(json.dumps({'passed':True,'game_launched':False,
    'original_setters':16,'reference_cases':256,'mask_cases':256,'operation_values':8,
    'original_rebind_cases':300,
    'note':'CPU semantics/defaults only; no GPU pixels or missing-mesh claim'},indent=2))
print('Stencil TU23 leaf/default/decoder tests PASS')
