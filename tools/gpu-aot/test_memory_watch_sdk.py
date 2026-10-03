"""Exercise native texture watches with the installed SDK's real page faults."""
import argparse
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root/'rexglue-sdk/out/install/win-amd64'
stub=a.output/'include/rex/system';stub.mkdir(parents=True,exist_ok=True)
(stub/'kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
extern rex::memory::Memory* tested_memory;
#define REX_KERNEL_MEMORY() tested_memory
''')
cpp=a.output/'watch-sdk.cpp';exe=a.output/'watch-sdk.exe'
cpp.write_text(r'''
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/system/xmemory.h>
#include "gpu_native/memory_watch.h"
#include <cassert>
#include <iostream>
rex::memory::Memory* tested_memory=nullptr;
using namespace legodimensions::gpu_native;
REXCVAR_DECLARE(bool, gpu_native_texture_watch);
REXCVAR_DECLARE(bool, gpu_native_buffer_watch);
int main(int argc,char**){
 rex::InitLogging();
 if(argc==2) REXCVAR_SET(gpu_native_texture_watch,true);
 if(argc==3) REXCVAR_SET(gpu_native_buffer_watch,true);
 rex::memory::Memory memory;tested_memory=&memory;
 assert(memory.Initialize());
 auto* heap=memory.LookupHeapByType(true,4096);
 uint32_t address=0;
 constexpr uint32_t rw=rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite;
 assert(heap->Alloc(16384,4096,rex::memory::kMemoryAllocationReserve|
     rex::memory::kMemoryAllocationCommit,rw,false,&address));
 auto* bytes=memory.TranslateVirtual<volatile uint8_t*>(address);
 CpuMemorySpan spans[]={{address,4096},{address+8192,4096}};
 auto stamp=WatchCpuMemory(spans);assert(CpuMemoryUnchanged(stamp));
 auto* physical=memory.GetPhysicalHeap();
 assert(physical->IsRangeCommittedReadable(memory.GetPhysicalAddress(address),16384));
 // Re-protecting a guest allocation must not silently erase its host watch.
 assert(heap->Protect(address,4096,rw,nullptr));
 bytes[4]=1;assert(!CpuMemoryUnchanged(stamp));
 for(unsigned i=0;i<100;++i){
   stamp=WatchCpuMemory(spans);assert(CpuMemoryUnchanged(stamp));
   bytes[(i%2)*8192+17]=uint8_t(i);assert(!CpuMemoryUnchanged(stamp));
 }
 stamp=WatchCpuMemory(spans);
 auto physical_address=memory.GetPhysicalAddress(address);
 InvalidateCpuPhysicalMemory(physical_address,16);
 *memory.TranslatePhysical<uint8_t*>(physical_address)=42;
 assert(!CpuMemoryUnchanged(stamp));
 stamp=WatchCpuMemory(spans);assert(CpuMemoryUnchanged(stamp));
 assert(heap->Decommit(address+8192,4096));assert(!CpuMemoryUnchanged(stamp));
 assert(!physical->IsRangeCommittedReadable(physical_address,16384));
 ShutdownCpuMemoryWatch();
 assert(heap->Release(address,nullptr));
 std::cout<<"PASS real SDK page writes/rearm/protect, physical copies, decommit and bounded commitment check\n";
}
''')
subprocess.run(['clang++','-std=c++23','-DNOMINMAX','-fms-runtime-lib=dll','-fuse-ld=lld',
 '-DSPDLOG_COMPILED_LIB','-DSPDLOG_FMT_EXTERNAL','-D_CRT_SECURE_NO_WARNINGS',
 '-Xclang','--dependent-lib=msvcrt','-I'+str(a.output/'include'),'-I'+str(root/'rexlego/src'),
 '-I'+str(sdk/'include'),str(cpp),str(root/'rexlego/src/gpu_native/memory_watch.cpp'),
 str(sdk/'lib/rexruntime.lib'),str(sdk/'lib/fmt.lib'),str(sdk/'lib/spdlog.lib'),'-o',str(exe)],check=True)
env=dict(os.environ);env['PATH']=str(sdk/'bin')+os.pathsep+env.get('PATH','')
env['LEGO_NATIVE_STATIC_TEXTURE_WATCH']='1';env.pop('LEGO_NATIVE_NO_MEMORY_WATCH',None)
subprocess.run([str(exe)],cwd=a.output,env=env,check=True,timeout=30)
env.pop('LEGO_NATIVE_STATIC_TEXTURE_WATCH')
subprocess.run([str(exe),'configured'],cwd=a.output,env=env,check=True,timeout=30)
env['LEGO_NATIVE_STATIC_TEXTURE_WATCH']='0';env['LEGO_NATIVE_BUFFER_WATCH']='1'
subprocess.run([str(exe)],cwd=a.output,env=env,check=True,timeout=30)
env.pop('LEGO_NATIVE_BUFFER_WATCH')
subprocess.run([str(exe),'buffer','configured'],cwd=a.output,env=env,check=True,timeout=30)
