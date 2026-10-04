"""Compile actual portrait hook bodies with a bounded fake guest-memory SDK."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];inc=a.output/'include/rex'
(inc/'system').mkdir(parents=True,exist_ok=True)
(inc/'types.h').write_text('''#pragma once
#include <cstdint>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
struct Register {uint32_t u32=0;};
struct PPCContext {Register r3;uint64_t result=0,canary=0x1122334455667788ull;};
''')
(inc/'hook.h').write_text('''#pragma once
#include <rex/types.h>
#define REX_FUNC(name) void name(PPCContext& ctx,u8* base)
''')
(inc/'logging.h').write_text('''#pragma once
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
''')
(inc/'cvar.h').write_text('''#pragma once
#define REXCVAR_DECLARE(type,name) type& FLAGS_##name##_storage_()
#define REXCVAR_GET(name) FLAGS_##name##_storage_()
''')
(inc/'system/kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
#define REX_KERNEL_MEMORY() (&fixture_memory)
''')
(inc/'system/xmemory.h').write_text('''#pragma once
#include <rex/types.h>
#include <array>
struct Heap {int reads=0;bool IsRangeCommittedReadable(u32 at,u32 size){++reads;return uint64_t(at)+size<=65536;}};
struct Memory {Heap heap;std::array<u8,65536> bytes{};
 Heap* LookupHeap(u32){return &heap;}
 template<class T>T TranslateVirtual(u32 at){return reinterpret_cast<T>(bytes.data()+at);}
};
inline Memory fixture_memory;
''')
cpp=a.output/'probe-test.cpp'
cpp.write_text(r'''
#include <rex/system/xmemory.h>
#include "gpu_native/portrait_probe.h"
#include <cassert>
#include <cstring>
bool& FLAGS_gpu_native_pm4_storage_(){static bool selected=false;return selected;}
extern "C" void sub_833738B8(PPCContext&,u8*);
extern "C" void sub_83373850(PPCContext&,u8*);
int setups=0,destroys=0;
extern "C" void __imp__sub_833738B8(PPCContext& ctx,u8*){++setups;ctx.r3.u32=0xCAFE;ctx.result=42;}
extern "C" void __imp__sub_83373850(PPCContext& ctx,u8*){++destroys;ctx.r3.u32=0xBEEF;ctx.result=43;}
void store(u32 at,u32 value){value=__builtin_bswap32(value);std::memcpy(fixture_memory.bytes.data()+at,&value,4);}
int main(int argc,char**){
 using namespace legodimensions::gpu_native;
 const bool enabled=argc>1;
 store(0x1000+84,0x2000);store(0x1000+200,0x3000);
 store(0x3000+948,0x1234);store(0x3000+952,0x5678);store(0x3000+956,0x4000);
 store(0x4000+4,3);store(0x4000+44,0x5000);store(0x5000+116,1);store(0x5000+120+52,0x10001);
 auto before=fixture_memory.bytes;PPCContext ctx;ctx.r3.u32=0x1000;
 sub_833738B8(ctx,fixture_memory.bytes.data());
 assert(setups==1 && ctx.r3.u32==0xCAFE && ctx.result==42 && ctx.canary==0x1122334455667788ull);
 assert(fixture_memory.bytes==before);
 assert(PortraitProbeEnabled()==enabled);
 TracePortraitDraw(0x5000,0,123,456); // Backend pointer must not match the D3D header.
 TracePortraitDraw(0x5000+120+52,0,123,456);
 ctx.r3.u32=0x1000;sub_83373850(ctx,fixture_memory.bytes.data());
 assert(destroys==1 && ctx.r3.u32==0xBEEF && ctx.result==43 && fixture_memory.bytes==before);
 const auto reads=fixture_memory.heap.reads;
 TracePortraitDraw(0x5000+120+52,0,123,456);assert(fixture_memory.heap.reads==reads);
 if(!enabled)assert(reads==0);
}
''')
exe=a.output/'portrait-probe-test.exe'
subprocess.run(['clang++','-std=c++20','-D_CRT_SECURE_NO_WARNINGS','-I'+str(a.output/'include'),
 '-I'+str(root/'rexlego/src'),str(root/'rexlego/src/gpu_native/hooks_portrait.cpp'),str(cpp),'-o',str(exe)],check=True)
for name,trace,reference,enabled in [('disabled',None,'0',False),('zero','0','0',False),
                                  ('enabled','1','0',True),('reference','1','1',False)]:
 env=dict(os.environ);directory=a.output/name;directory.mkdir(exist_ok=True)
 events=directory/'events.tsv'
 if events.exists():events.unlink()
 env['LEGO_NATIVE_TRACE_DIR']=str(directory.resolve());env['LEGO_NATIVE_PM4_REFERENCE']=reference
 env.pop('LEGO_NATIVE_PORTRAIT_TRACE',None)
 if trace is not None:env['LEGO_NATIVE_PORTRAIT_TRACE']=trace
 # The reference bypasses hooks; the standalone enable predicate still sees
 # trace settings. Leave its flag off here; routing itself is covered separately.
 if reference=='1':env.pop('LEGO_NATIVE_PORTRAIT_TRACE',None)
 subprocess.run([str(exe.resolve())]+(['enabled'] if enabled else []),env=env,check=True)
 if enabled:
  text=events.read_text();assert text.count('\tportrait_setup\t')==1
  assert text.count('\tportrait_texture_draw\t')==1 and text.count('\tportrait_destroy\t')==1
 else:assert not events.exists()
print('PASS: actual portrait hooks, original register/results preserved, no guest writes, exact header match, destructor removal, disabled reads=0')
