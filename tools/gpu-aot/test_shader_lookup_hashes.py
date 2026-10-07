"""Run the production lookup against colliding prefixes and same-call hash reuse."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
src=(root/'rexlego/src/gpu_native/shader_archive.cpp').read_text()
start=src.index('const ShaderCacheEntry* FindShaderByMicrocode(')
end=src.index('\nuint64_t HashShaderContainer(',start)
body=src[start:end].replace('XXH3_64bits(', 'CountHash(')
code=r'''
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu_native/shader_archive.h"
std::vector<ShaderMicrocodeEntry> entries;
namespace {
ShaderMicrocodeEntry* g_shaderMicrocodeEntries;
size_t g_shaderMicrocodeEntryCount;
unsigned calls=0;
uint64_t CountHash(const void* p,size_t n){++calls;return XXH3_64bits(p,n);}
ShaderCacheEntry hit{9,0,0,0,0,0};
const ShaderCacheEntry* FindShader(uint64_t id){return id==9?&hit:nullptr;}
const ShaderCacheEntry* FindPrecompiledByMicrocode(const void*,uint32_t,size_t,uint64_t,
    std::unordered_map<uint32_t,uint64_t>&){return nullptr;}

''' + body + r'''
}
int main(){
 std::array<uint8_t,512> bytes{};
 bytes[20]=23;
 auto prefix=XXH3_64bits(bytes.data(),8);
 for(unsigned i=0;i<3000;++i)
   entries.push_back({prefix,0,100+i,128u<<(i%3),i%2,0});
 entries.push_back({prefix,XXH3_64bits(bytes.data(),512),9,512,0,0});
 g_shaderMicrocodeEntries=entries.data();g_shaderMicrocodeEntryCount=entries.size();
 assert(FindShaderByMicrocode(bytes.data(),0,bytes.size())==&hit);
 assert(calls<=4); // Prefix plus at most three lengths, not 1501 hashes.
 calls=0;
 assert(FindShaderByMicrocode(bytes.data(),1,bytes.size())==nullptr);assert(calls==4);
 bytes[20]^=1;calls=0;
 assert(FindShaderByMicrocode(bytes.data(),0,bytes.size())==nullptr);assert(calls==4);
 bytes[20]^=1;calls=0;
 assert(FindShaderByMicrocode(bytes.data(),0,bytes.size())==&hit);assert(calls<=4);
 calls=0;
 assert(!FindShaderByMicrocode(bytes.data(),0,7));assert(calls==0);
 assert(!FindShaderByMicrocode(bytes.data(),0,127));assert(calls==1);
 assert(!FindShaderByMicrocode(nullptr,0,bytes.size()));
}
'''
code=code.replace('g_shaderMicrocodeEntries','fixtureEntries').replace('g_shaderMicrocodeEntryCount','fixtureCount')
cpp=a.output/'lookup.cpp';cpp.write_text(code);exe=a.output/'lookup.exe'
subprocess.run([a.compiler,'-std=c++20','-I'+str(root/'rexlego/src'),
 '-I'+str(root/'rexglue-sdk/thirdparty/xxHash'),str(cpp),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe.resolve())],check=True,timeout=10)
print('PASS: actual lookup, 3001 colliding prefixes, stage/missing/success and same-address rewrites; <=4 hashes per lookup')
