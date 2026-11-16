"""Execute production DXIL lookup: linked shaders must not inflate base banks."""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
source=(root/'rexlego/src/gpu_native/shader_archive.cpp').read_text()
start=source.index('ShaderBytecode FindDxil(');end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
body=source[start:end]
fixture=r'''
#include "gpu_native/shader_archive.h"
#include "gpu_native/linked_shader_cache.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <iostream>
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
ShaderCacheEntry g_shaderCacheEntries[]={{11,1,2,0,0,1},{12,1,2,0,0,0}};
ShaderCacheEntry g_runtimeShaderCacheEntries[]={{21,1,2,0,0,1},{22,1,2,0,0,0}};
const size_t g_shaderCacheEntryCount=2,g_runtimeShaderCacheEntryCount=2;
const uint8_t g_compressedDxilCache[]={1,2,3,4},g_runtimeCompressedDxilCache[]={5,6,7,8};
const size_t g_dxilCacheCompressedSize=4,g_dxilCacheDecompressedSize=4;
const size_t g_runtimeDxilCacheCompressedSize=4,g_runtimeDxilCacheDecompressedSize=4;
LinkedShaderCacheEntry g_linkedShaderCacheEntries[]={{11,0,0,2},{11,1,2,2}};
LinkedShaderCacheEntry g_runtimeLinkedShaderCacheEntries[]={{21,0,0,2},{21,1,2,2}};
const size_t g_linkedShaderCacheEntryCount=2,g_runtimeLinkedShaderCacheEntryCount=2;
const uint8_t g_compressedLinkedDxilCache[]={31,32,33,34},g_runtimeCompressedLinkedDxilCache[]={41,42,43,44};
const size_t g_linkedDxilCacheCompressedSize=4,g_linkedDxilCacheDecompressedSize=4;
const size_t g_runtimeLinkedDxilCacheCompressedSize=4,g_runtimeLinkedDxilCacheDecompressedSize=4;
int base_decodes=0,linked_decodes=0;
size_t ZSTD_decompress(void* dst,size_t cap,const void* src,size_t n){++base_decodes;if(cap!=n)return 999;std::memcpy(dst,src,n);return n;}
bool ZSTD_isError(size_t value){return value==999;}
using mz_ulong=unsigned long;constexpr int MZ_OK=0;
int mz_uncompress(uint8_t* dst,mz_ulong* n,const uint8_t* src,mz_ulong len){++linked_decodes;if(*n!=len)return -1;std::memcpy(dst,src,len);return MZ_OK;}
namespace legodimensions::gpu_native {
struct Bank {std::vector<ShaderCacheEntry> shaders;std::vector<LinkedShaderCacheEntry> variants;std::vector<uint8_t> dxil;};
std::mutex g_pack_mutex;std::map<uint64_t,std::pair<Bank*,size_t>> g_pack_shaders;
const ShaderCacheEntry* FindBuiltinShader(uint64_t hash){
 for(auto& e:g_shaderCacheEntries)if(e.hash==hash)return &e;
 for(auto& e:g_runtimeShaderCacheEntries)if(e.hash==hash)return &e;
 return nullptr;
}
'''+body+r'''
}
using namespace legodimensions::gpu_native;
void check(bool ok){if(!ok)throw std::runtime_error("archive selection or bytes changed");}
int main(int argc,char**){try{
 for(unsigned iteration=0;iteration<100;++iteration){
  for(auto h:{11ull,21ull})for(unsigned spec:{0u,1u,8u,9u}){
   auto code=FindDxil(h,spec);check(code&&code.size==2);
   const uint8_t expected=(h==11?31:41)+2*(spec&1);
   check(code.data[0]==expected&&code.data[1]==expected+1);
  }
 }
 check(base_decodes==0&&linked_decodes==2);
 check(!FindDxil(99,0)&&base_decodes==0);
 auto main=FindDxil(12,0),runtime=FindDxil(22,0);
 check(main&&runtime&&main.size==2&&runtime.size==2);
 check(main.data[0]==2&&main.data[1]==3&&runtime.data[0]==6&&runtime.data[1]==7);
 check(base_decodes==2&&linked_decodes==2);
 check(FindDxil(12,99).data==main.data&&FindDxil(22,99).data==runtime.data);
 check(base_decodes==2&&FindDxil(11,1).data[0]==33);
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 23;}}
'''
for mutant in (False,True):
    code=fixture
    if mutant:
        code=code.replace('  if (shader->specConstantsMask == 0) {','  {',1)
        # Restore the old eager inflation while still returning the correct
        # linked bytes. Only the decoder-call oracle should reject this path.
        code=code.replace('    return {dxil.get() + shader->dxilOffset, shader->dxilSize};',
            '    if (shader->specConstantsMask == 0) return {dxil.get() + shader->dxilOffset, shader->dxilSize};',1)
    stem='legacy-eager' if mutant else 'lazy-archive'
    cpp=a.output/(stem+'.cpp');exe=a.output/(stem+'.exe');cpp.write_text(code)
    subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
    result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=10)
    if result.returncode != (23 if mutant else 0):
        raise RuntimeError(stem+': '+result.stdout+result.stderr)
print('PASS production main/runtime lazy archive selection, 800 linked lookups, exact bytes, single inflation per used bank; eager-path mutation rejected')
