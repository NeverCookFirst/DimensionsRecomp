"""Compile the production watcher against a bounded SDK memory callback model."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__); p.add_argument('output',type=Path); a=p.parse_args()
root=Path(__file__).resolve().parents[2]; a.output.mkdir(parents=True,exist_ok=True)
stub=a.output/'include/rex/system'; stub.mkdir(parents=True,exist_ok=True)
(stub.parent/'cvar.h').write_text(r'''
#pragma once
namespace rex::cvar {
enum class Lifecycle { kRequiresRestart };
struct FixtureCvar { FixtureCvar lifecycle(Lifecycle) { return *this; } };
}
#define REXCVAR_DEFINE_BOOL(name,...) static auto fixture_##name = rex::cvar::FixtureCvar{}
#define REXCVAR_GET(...) false
''')
(stub/'xmemory.h').write_text(r'''
#pragma once
#include <cstdint>
#include <mutex>
#include <utility>
#include <unordered_set>
#include <cassert>
namespace rex::thread {
struct global_critical_region {
  static inline std::recursive_mutex mutex;
  auto Acquire() { return std::unique_lock<std::recursive_mutex>(mutex); }
}; }
namespace rex::memory {
constexpr uint32_t kMemoryAllocationCommit=1,kMemoryProtectRead=1;
struct HeapAllocationInfo { uint32_t state=1,protect=1,region_size=4096; };
struct Heap {
 bool QueryRegionInfo(uint32_t at,HeapAllocationInfo* info) {
   if(at>=0x100000) return false; *info={}; return true;
 }
 uint32_t heap_base() { return 0; } uint32_t page_size() { return 4096; }
};
struct Memory {
 using Callback=std::pair<uint32_t,uint32_t>(*)(void*,uint32_t,uint32_t,bool);
 Callback callback=nullptr; void* context=nullptr; Heap heap;
 std::unordered_set<uint32_t> armed;
 uint32_t GetPhysicalAddress(uint32_t at) {
   if(at>=0xE0000000 && at<0xE0100000) return (at&0x1FFFFFFF)+0x1000;
   if((at>=0xA0000000&&at<0xA0100000)||(at>=0xC0000000&&at<0xC0100000)) return at&0x1FFFFFFF;
   return UINT32_MAX;
 }
 Heap* GetPhysicalHeap() { return &heap; }
 void* RegisterPhysicalMemoryInvalidationCallback(Callback c,void* ptr) { assert(!callback);callback=c;context=ptr;return this; }
 void UnregisterPhysicalMemoryInvalidationCallback(void*) { callback=nullptr; }
 void EnablePhysicalMemoryAccessCallbacks(uint32_t at,uint32_t size,bool notify,bool data) {
   assert(notify&&!data);
   for(auto page=at/4096;page<=(at+size-1)/4096;++page) armed.insert(page);
 }
 void Write(uint32_t alias) {
   auto lock=rex::thread::global_critical_region().Acquire();
   uint32_t phys=GetPhysicalAddress(alias),page=phys/4096;
   if(!armed.contains(page)||!callback)return;
   auto [start,size]=callback(context,page*4096,4096,false);
   assert(start==page*4096&&size==4096);
   armed.erase(page);
 }
};
inline Memory memory;
}
''')
(stub/'kernel_state.h').write_text('#pragma once\n#include "xmemory.h"\n#define REX_KERNEL_MEMORY() (&rex::memory::memory)\n')
h=a.output/'watch-test.cpp'
h.write_text(r'''
#include "gpu_native/memory_watch.h"
#include <rex/system/xmemory.h>
#include <cassert>
#include <thread>
using namespace legodimensions::gpu_native;
CpuMemoryStamp Stamp(uint32_t at,uint32_t length) {
 CpuMemorySpan span{at,length}; return WatchCpuMemory({&span,1});
}
int main(int argc,char**) {
 if(argc>1) { assert(Stamp(0xA0002000,64).pages.empty()); return 0; }
 auto& memory=rex::memory::memory;
 assert(!CpuMemoryUnchanged({}));
 assert(Stamp(0x70002000,64).pages.empty()); // Virtual-only data always hash.
 assert(Stamp(0xA00FFFF0,128).pages.empty()); // Uncommitted end.
 assert(Stamp(0xFFFFFFF0,128).pages.empty()); // Address overflow.
 assert(Stamp(0xA0002000,0).pages.empty());
 auto first=Stamp(0xA0002001,8192); // Three pages.
 assert(first.pages.size()==3&&CpuMemoryUnchanged(first));
 auto neighbor=Stamp(0xC0005000,4096);
 memory.Write(0xC0003004); // Same backing, other alias.
 assert(!CpuMemoryUnchanged(first)&&CpuMemoryUnchanged(neighbor));
 first=Stamp(0xA0002001,8192); assert(CpuMemoryUnchanged(first));
 memory.Write(0xE0002004); // E view's +4 KB maps to physical 0x3004.
 assert(!CpuMemoryUnchanged(first)&&CpuMemoryUnchanged(neighbor));
 first=Stamp(0xA0002001,8192);
 memory.Write(0xA0003004); // Another write in the SAME frame.
 assert(!CpuMemoryUnchanged(first));
 const CpuMemorySpan spans[]={{0xA0002000,4096},{0xC0007000,4096}};
 auto two=WatchCpuMemory(spans); assert(CpuMemoryUnchanged(two));
 memory.Write(0xE0006001); assert(!CpuMemoryUnchanged(two)); // Mip tail only.
 two=WatchCpuMemory(spans);
 InvalidateCpuPhysicalMemory(0x2004,64); // TranslatePhysical host memmove.
 assert(!CpuMemoryUnchanged(two));
 auto intervening=Stamp(0xA0009000,4096);
 std::thread writer([&]{memory.Write(0xC0009001);});writer.join();
 assert(!CpuMemoryUnchanged(intervening)); // Arm before copy/hash.
 auto after=Stamp(0xA0009000,4096);assert(CpuMemoryUnchanged(after));
 ShutdownCpuMemoryWatch();assert(!CpuMemoryUnchanged(after));
 auto recreated=Stamp(0xA0009000,4096);assert(CpuMemoryUnchanged(recreated));
 memory.Write(0xE0008001);assert(!CpuMemoryUnchanged(recreated));
 ShutdownCpuMemoryWatch();
}
''')
exe=a.output/'memory-watch-test.exe'
subprocess.run(['clang++','-std=c++20','-DNOMINMAX','-I'+str(a.output/'include'),
 '-I'+str(root/'rexlego/src'),str(root/'rexlego/src/gpu_native/memory_watch.cpp'),str(h),'-o',str(exe)],check=True)
env=dict(os.environ);env.pop('LEGO_NATIVE_NO_MEMORY_WATCH',None);env['LEGO_NATIVE_STATIC_TEXTURE_WATCH']='1'
subprocess.run([str(exe.resolve())],env=env,check=True)
env['LEGO_NATIVE_NO_MEMORY_WATCH']='1';subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
env.pop('LEGO_NATIVE_NO_MEMORY_WATCH');env['LEGO_NATIVE_STATIC_TEXTURE_WATCH']='0'
subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
env.pop('LEGO_NATIVE_STATIC_TEXTURE_WATCH')
subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
env['LEGO_NATIVE_BUFFER_WATCH']='1';env['LEGO_NATIVE_STATIC_TEXTURE_WATCH']='0'
subprocess.run([str(exe.resolve())],env=env,check=True)
env['LEGO_NATIVE_NO_MEMORY_WATCH']='1'
subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
(a.output/'verification.json').write_text(json.dumps({'production_source':True,'sdk_model':True,'checks':[
 'A/C/E aliases and +4KB','same-frame writes','neighbor watch retained','base+mips','physical host write',
 'write between arm and reuse','unknown/uncommitted/overflow fallback','device recreation','disabled fallback'],
 'passed':True},indent=2))
print('Passed production watcher with SDK callback model: aliases, same-frame writes, separate mips, races, neighbor protection, physical copies and fallback.')
