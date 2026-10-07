"""Exercise production shader parsing/lookup next to inaccessible guard pages."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
archive = (root / 'rexlego/src/gpu_native/shader_archive.cpp').read_text()
shaders = (root / 'rexlego/src/gpu_native/shaders.cpp').read_text()


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "gpu_native/shader_archive.h"
#include "gpu_native/shader_container.h"
using u32=uint32_t;
using namespace legodimensions::gpu_native;

struct GuardedBytes {
  uint8_t* allocation;
  uint8_t* bytes;
  size_t page, size;
  explicit GuardedBytes(size_t n):size(n) {
#ifdef _WIN32
    SYSTEM_INFO info{};GetSystemInfo(&info);page=info.dwPageSize;
    allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    assert(allocation);DWORD old=0;assert(VirtualProtect(allocation+page,page,PAGE_NOACCESS,&old));
#else
    page=size_t(sysconf(_SC_PAGESIZE));
    auto* mapped=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapped!=MAP_FAILED);allocation=static_cast<uint8_t*>(mapped);
    assert(mprotect(allocation+page,page,PROT_NONE)==0);
#endif
    assert(n<=page);bytes=allocation+page-n;std::memset(bytes,0,n);
  }
  ~GuardedBytes(){
#ifdef _WIN32
    VirtualFree(allocation,0,MEM_RELEASE);
#else
    munmap(allocation,page*2);
#endif
  }
  void word(size_t offset,uint32_t value){
    assert(offset+4<=size);
    for(size_t i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(24-8*i));
  }
};
uint64_t ZeroHash(size_t size) {
  uint64_t hash=14695981039346656037ull;
  for(size_t i=0;i<size;++i) hash*=1099511628211ull;
  return hash;
}
unsigned hash_calls=0;
uint64_t XXH3_64bits(const void* pointer,size_t size) {
  // A deterministic reading hash keeps the fixture SDK-free. Production
  // lookup body still chooses the exact lengths; guard pages enforce safety.
  ++hash_calls;auto* bytes=static_cast<const uint8_t*>(pointer);
  uint64_t hash=14695981039346656037ull;
  for(size_t i=0;i<size;++i) hash=(hash^bytes[i])*1099511628211ull;
  return hash;
}
ShaderMicrocodeEntry g_shaderMicrocodeEntries[]={
  {ZeroHash(8),0,8,4096,0,0},
  {ZeroHash(8),0,8,64,0,0},
  {ZeroHash(8),ZeroHash(16),7,16,0,0},
};
const size_t g_shaderMicrocodeEntryCount=3;
namespace legodimensions::gpu_native {
ShaderCacheEntry hit{7,0,0,0,0,0};
const ShaderCacheEntry* FindShader(uint64_t hash){return hash==7?&hit:nullptr;}
const ShaderCacheEntry* FindPrecompiledByMicrocode(const void*,uint32_t,size_t,uint64_t,
    std::unordered_map<uint32_t,uint64_t>&){return nullptr;}

''' + function(archive, 'size_t ShaderContainerByteLength(') + '\n' + function(
    archive, 'const ShaderCacheEntry* FindShaderByMicrocode(') + '\n' + function(
    archive, 'uint64_t HashShaderContainer(') + r'''
}
namespace rex::memory {
constexpr uint32_t kMemoryAllocationCommit=1,kMemoryProtectRead=1;
struct HeapAllocationInfo {
  uint32_t allocation_base=0,allocation_size=0,region_size=0,state=0,protect=0;
};
}
struct FakeHeap {
  uint32_t size=4096;
  uint32_t page_size()const{return size;}
  uint32_t heap_base()const{return size>0x10000?size:0x10000;}
  bool QueryRegionInfo(uint32_t page,rex::memory::HeapAllocationInfo* info) {
    assert(page%size==0);
    const uint32_t base=size>0x10000?size:0x10000;
    if(page<base || page>=base+size*2)return false;
    *info={base,size*2,size,1,page==base?1u:0u};return true;
  }
};
struct FakeMemory {
  FakeHeap heap;
  FakeHeap* LookupHeap(uint32_t address){
    const uint32_t base=heap.size>0x10000?heap.size:0x10000;
    return address>=base&&address<base+heap.size*2?&heap:nullptr;
  }
} memory;
#define REX_KERNEL_MEMORY() (&::memory)
''' + function(shaders, 'size_t ReadableGuestSpan(') + r'''
int main() {
  GuardedBytes header(36);header.word(4,36);header.word(8,0);
  assert(ShaderContainerByteLength(header.bytes,36)==36);
  assert(ShaderContainerByteLength(header.bytes,35)==0);
  header.word(4,0x400000);assert(!ShaderContainerByteLength(header.bytes,36));
  header.word(4,12);assert(!ShaderContainerByteLength(header.bytes,36));
  header.word(4,36);header.word(8,0xFFFFFFFF);assert(!ShaderContainerByteLength(header.bytes,36));
  hash_calls=0;assert(!HashShaderContainer(header.bytes,36));assert(hash_calls==0);
  GuardedBytes runtime(40);runtime.word(4,36);runtime.word(8,4);
  assert(ShaderContainerByteLength(runtime.bytes,40)==40);
  assert(ShaderContainerByteLength(runtime.bytes,39)==0);
  hash_calls=0;assert(HashShaderContainer(runtime.bytes,40));assert(hash_calls==1);
  runtime.word(36,4);assert(!ShaderContainerByteLength(runtime.bytes,40));
  GuardedBytes archive_bytes(44);archive_bytes.word(4,36);archive_bytes.word(8,4);archive_bytes.word(36,4);
  assert(ShaderContainerByteLength(archive_bytes.bytes,44)==44);
  assert(ShaderContainerByteLength(archive_bytes.bytes,43)==0);
  GuardedBytes prefix(7);hash_calls=0;
  assert(!FindShaderByMicrocode(prefix.bytes,0,7));assert(hash_calls==0);
  GuardedBytes microcode(16);hash_calls=0;
  assert(FindShaderByMicrocode(microcode.bytes,0,16)==&hit);assert(hash_calls==2);
  hash_calls=0;assert(!FindShaderByMicrocode(microcode.bytes,0,15));assert(hash_calls==1);
  hash_calls=0;assert(!FindShaderByMicrocode(microcode.bytes,1,16));assert(hash_calls==1);
  microcode.bytes[12]=1;assert(!FindShaderByMicrocode(microcode.bytes,0,16));
  assert(ReadableGuestSpan(0)==0);
  assert(ReadableGuestSpan(0x10003)==4093);
  assert(ReadableGuestSpan(0x10FFF)==1);
  assert(ReadableGuestSpan(0x11000)==0);
  assert(ReadableGuestSpan(0x12000)==0);
  memory.heap.size=65536;
  assert(ReadableGuestSpan(0x11000)==65536-4096);
  assert(ReadableGuestSpan(0x1FFFF)==1);
  assert(ReadableGuestSpan(0x20000)==0);
  memory.heap.size=16*1024*1024;
  assert(ReadableGuestSpan(16*1024*1024+4096)==4*1024*1024+4);
  assert(ReadableGuestSpan(32*1024*1024-4096)==4096);
  assert(ReadableGuestSpan(32*1024*1024)==0);
}
'''
cpp = a.output / 'shader-bounds.cpp'
cpp.write_text(code)
exe = a.output / 'shader-bounds.exe'
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG',
                '-I' + str(root / 'rexlego/src'), str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe.resolve())], check=True, timeout=10)
print('PASS: production container parsing/hash, placement lookup and readable '
      'guest spans; truncated/colliding shaders cannot cross guard pages')
