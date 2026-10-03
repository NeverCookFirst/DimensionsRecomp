"""TU23 alpha setters/defaults, all compares, and actual microcode -> DXIL.

No game or window is started. C++ tests share the exact emitted HLSL helper.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
# The portable embedded Python does not add the script directory to sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_depth_export import fixture

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('compiler', type=Path)
p.add_argument('common', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)

def body(source, sig):
    start = source.index(sig)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

image = (root / 'xexdump/dump/default.bin').read_bytes()
indices = (15,16,18,19,20,21,22,23,24,26,53,54,55,56,59)
table = [struct.unpack_from('>III', image, 0x847F9B18 - 0x82000000 + 12*k) for k in indices]
assert [t[1] for t in table] == [0x83FB7E38,0x83FB81C8,0x83FB7F58,0x83FB7FE8,
    0x83FB7EC8,0x83FB80E8,0x83FB8158,0x83FB8078,0x83FB7E00,0x83FB82C0,
    0x83FB8A68,0x83FB8AA8,0x83FB8AE8,0x83FB8B28,0x83FB92B8]
assert [t[2] for t in table] == [0,0,1,0,0,1,0,0,0,7,15,15,15,15,0x3F800000]
functions = []
for path in (root / 'rexlego/generated/default').glob('*.cpp'):
    s = path.read_text()
    for _, setter, _ in table:
        sig = f'DEFINE_REX_FUNC(sub_{setter:08X})'
        if sig in s:
            f = body(s, sig)
            # All direct branches stay inside these CPU-only setters.
            instructions = re.findall(r'^\s*// ([^\n]+)', f, re.M)
            for asm in instructions:
                branch = re.match(r'b[l]? 0x([A-Fa-f0-9]+)', asm)
                if branch:
                    assert setter <= int(branch[1],16) < setter+4*len(instructions)
                assert not re.match(r'bctr|bctrl', asm)
            assert not re.search(r'13928\(r\d+\)|13932\(r\d+\)', f)
            functions.append(f)
assert len(functions) == 15
initializer = body((root / 'rexlego/src/gpu_native/hooks_device.cpp').read_text(),
                   'bool InitializeRenderDefaults(')
shared = body((root / 'rexlego/src/gpu_native/draw.cpp').read_text(), 'struct SharedConstants') + ';'
h = a.output / 'alpha-test.cpp'
h.write_text(r'''
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include "gpu_native/alpha_test.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using uint=unsigned;
#include "alpha_compare.hlsli"
union PPCRegister {u64 u64;int64_t s64;u32 u32;int32_t s32;u8 u8;float f32;};
struct FReg {double f64=0;};struct Fpscr {void disableFlushMode(){}};
struct XER {u8 ca=0;};
struct CR {bool eq=false,gt=false,lt=false;template<class T>void compare(T a,T b,XER){eq=a==b;gt=a>b;lt=a<b;}};
struct PPCContext {
'''+''.join(f'PPCRegister r{i}{{}};\n' for i in range(32))+r'''
 FReg f0;Fpscr fpscr;XER xer;CR cr0,cr6;
};
std::array<u8,65536> ram{};
u32 Load(u32 p){return (u32(ram[p])<<24)|(u32(ram[p+1])<<16)|(u32(ram[p+2])<<8)|ram[p+3];}
void Store(u32 p,u32 v){for(u32 i=0;i<4;++i)ram[p+i]=u8(v>>(24-8*i));}
u64 Load64(u32 p){return (u64(Load(p))<<32)|Load(p+4);}
void Store64(u32 p,u64 v){Store(p,u32(v>>32));Store(p+4,u32(v));}
#define REX_LOAD_U32(p) Load(p)
#define REX_STORE_U32(p,v) Store(p,v)
#define REX_LOAD_U64(p) Load64(p)
#define REX_STORE_U64(p,v) Store64(p,v)
#define REX_FUNC_PROLOGUE() ((void)0)
#define DEFINE_REX_FUNC(name) void name(PPCContext& ctx,u8* base)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
'''+ '\n'.join(functions) + r'''
struct be_u32 {u32 raw;operator u32()const{return __builtin_bswap32(raw);}};
constexpr u32 kRenderStateTable=4096;
std::vector<u32> calls;
struct Dispatcher {u32 missing=0;u32 GetFunction(u32 p){return p==missing?0:p;}} dispatcher;
struct Kernel {auto function_dispatcher(){return &dispatcher;}} kernel;
#define REX_KERNEL_STATE() (&kernel)
namespace rex::ppc {
template<class T>void GuestToHostFunction(u32 f,u32 device,u32 value){
 PPCContext c{};c.r1.u32=60000;c.r3.u32=device;c.r4.u32=value;calls.push_back(f);
'''+ '\n'.join((' if' if i==0 else ' else if')+f'(f==0x{t[1]:08X})sub_{t[1]:08X}(c,ram.data());'
               for i,t in enumerate(table))+r'''
 else assert(false);
}
}
'''+initializer+'\n'+shared+r'''
int main(){
 constexpr u32 device=12288;
'''+''.join(f'Store(kRenderStateTable+12*{k}+4,{t[1]}u);Store(kRenderStateTable+12*{k}+8,{t[2]}u);\n'
           for k, t in zip(indices, table))+r'''
 assert(InitializeRenderDefaults(device,ram.data()));assert(calls.size()==15);
 assert(Load(device+10556)==7&&Load(device+10620)==0x3F800000);
 assert(Load(device+10568)==0); // Cull/fill untouched.
 for(u32 at: {10552u,10584u,10588u,10592u})assert(Load(device+at)==0x00010001);
 for(u32 at: {12292u,12296u,12300u,12304u})assert(Load(device+at)==15);
 dispatcher.missing=0x83FB92B8;calls.clear();assert(!InitializeRenderDefaults(device,ram.data()));assert(calls.empty());
 dispatcher.missing=0;Store(kRenderStateTable+24*12+4,0xDEADBEEF);
 assert(!InitializeRenderDefaults(device,ram.data()));assert(calls.empty());
 using namespace legodimensions::gpu_native;
 const float values[]={-INFINITY,-32.f,-1.f,-0.f,0.f,0.2f,0.5f,1.f,32.f,INFINITY,NAN};
 for(float alpha:values)for(float reference:values)for(u32 f=0;f<8;++f){
  // Independent Xenos less/equal/greater pass mask, with its NOTEQUAL/ALWAYS rules.
  bool expected=f==7||(f==5?alpha!=reference:
    (((f&1)!=0&&alpha<reference)||((f&2)!=0&&alpha==reference)||((f&4)!=0&&alpha>reference)));
  assert(LegoAlphaPass(alpha,reference,f)==expected);
  auto decoded=DecodeNativeAlphaState(0xF0000000|8|f,reference);
  assert(decoded.enabled&&decoded.function==f);
  assert(std::bit_cast<u32>(decoded.reference)==std::bit_cast<u32>(reference));
  assert(!DecodeNativeAlphaState(f,reference).enabled);
 }
 // Execute original setters for every compare and non-integer reference.
 for(u32 f=0;f<8;++f){
  rex::ppc::GuestToHostFunction<void>(0x83FB82C0,device,f);
  rex::ppc::GuestToHostFunction<void>(0x83FB7E00,device,1);
  rex::ppc::GuestToHostFunction<void>(0x83FB92B8,device,std::bit_cast<u32>(0.37f));
  auto d=DecodeNativeAlphaState(Load(device+10556),std::bit_cast<float>(Load(device+10620)));
  assert(d.enabled&&d.function==f&&d.reference==0.37f);
 }
 static_assert(sizeof(SharedConstants)==624);
 static_assert(offsetof(SharedConstants,alpha_function)==600);
 static_assert(offsetof(SharedConstants,alpha_threshold)==556);
 static_assert(offsetof(SharedConstants,color_output_scale)==608);
}
''')
exe = a.output / 'alpha-test.exe'
subprocess.run(['clang++', '-std=c++20', '-DNOMINMAX', '-I'+str(root/'rexlego/src'),
                '-I'+str(root/'tools/gpu-aot'), str(h), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)

inputs = a.output / 'input'
inputs.mkdir(exist_ok=True)
cases = {'color': {}, 'color_predicated': {'predicated': True}, 'depth': {'depth': True},
         'depth_scalar': {'depth': True, 'scalar': True}, 'depth_predicated': {'depth': True, 'predicated': True}}
for name, opts in cases.items():
    (inputs / f'{name}.bin').write_bytes(fixture(**opts))
env = os.environ.copy()
env['XENOS_RECOMP_DXIL_ONLY'] = '1'
env['XENOS_RECOMP_LEGO_NATIVE_SCALE'] = '1'
env.pop('XENOS_RECOMP_ONLY_HASH', None)
subprocess.run([str(a.compiler.resolve()), str(inputs.resolve()),
                str((a.output/'fixture-cache.cpp').resolve()), str(a.common.resolve()),
                str((a.output/'hlsl').resolve())], env=env, check=True)
for name, opts in cases.items():
    s = (a.output/'hlsl'/f'{name}.hlsl').read_text()
    assert f'#define LEGO_SHADER_WRITES_COLOR0 {int(not opts.get("depth",False))}' in s
    assert 'g_AlphaFunction : packoffset(c37.z)' in s
    assert s.index('if (!LegoAlphaPass(') < s.index('oC0 *= g_ColorOutputScale.x;')
    assert ('oDepth : SV_Depth' in s) == opts.get('depth',False)
(a.output/'verification.json').write_text(json.dumps({
    'passed': True, 'actual_tu23_setters': [f'{t[1]:08X}' for t in table],
    'table_defaults': [f'{t[2]:08X}' for t in table], 'shared_bytes': 624,
    'checks': ['original leaf setters and native initializer', 'table validation before mutation',
               '968 comparisons incl NaN/inf', 'raw float reference', 'no cull/fill writes',
               '5 actual microcode/HLSL/DXIL fixtures', 'depth-only gate', 'compare before EDRAM scale'],
    'game_visual_validation': False}, indent=2))
print('PASS: TU23 defaults/setters, eight compares, and five HLSL/DXIL fixtures; no game launched.')
