"""Compare inline AVX2 XXH3 with SSE2 library hashes and benchmark both."""
import argparse
import json
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--sdk-library',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
include=root/'rexglue-sdk/thirdparty/xxHash'
c=a.output/'reference.c';c.write_text('''#include <xxhash.h>
unsigned long long ReferenceHash(const void* p,size_t n){return XXH3_64bits(p,n);}
unsigned long long ReferenceSeed(const void* p,size_t n,unsigned long long s){return XXH3_64bits_withSeed(p,n,s);}
''')
h=a.output/'inline-hash.cpp';h.write_text(r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
extern "C" unsigned long long ReferenceHash(const void*,size_t);
extern "C" unsigned long long ReferenceSeed(const void*,size_t,unsigned long long);
int main(){
 std::mt19937 random(991);std::vector<unsigned char> bytes(8*1024*1024+64);
 for(auto& b:bytes)b=random();
 for(size_t i=0;i<5000;++i){
  size_t size=i<300?i:random()%(8*1024*1024);size_t offset=random()%64;auto seed=(uint64_t(random())<<32)|random();
  assert(XXH3_64bits(bytes.data()+offset,size)==ReferenceHash(bytes.data()+offset,size));
  assert(XXH3_64bits_withSeed(bytes.data()+offset,size,seed)==ReferenceSeed(bytes.data()+offset,size,seed));
 }
 volatile uint64_t sink=0;
 auto bench=[&](bool ref){auto start=std::chrono::steady_clock::now();
  for(size_t i=0;i<2048;++i){const auto* p=bytes.data()+(i%8)*1024*1024;
   sink=ref?ReferenceHash(p,1024*1024):XXH3_64bits(p,1024*1024);}
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
 bench(false);bench(true);
 double ref=bench(true),inl=bench(false);
 printf("{\"cases\":5000,\"hashed_bytes_per_benchmark\":2147483648,\"sse2_ms\":%.3f,\"avx2_ms\":%.3f,\"ratio\":%.3f,\"identical\":true}\n",ref,inl,ref/inl);
}
''')
obj=a.output/'reference.obj'
subprocess.run(['clang','-O2','-DXXH_VECTOR=XXH_SSE2','-I'+str(include),'-c',str(c),'-o',str(obj)],check=True)
hashobj=a.sdk_library or a.output/'xxhash.obj'
if not a.sdk_library:
 subprocess.run(['clang','-O2','-DXXH_VECTOR=XXH_SSE2','-I'+str(include),'-c',str(include/'xxhash.c'),'-o',str(hashobj)],check=True)
exe=a.output/'inline-hash.exe'
subprocess.run(['clang++','-std=c++20','-O2','-march=x86-64-v3','-I'+str(include),str(h),str(obj),str(hashobj),'-o',str(exe)],check=True)
result=subprocess.run([str(exe.resolve())],check=True,text=True,capture_output=True)
proof=json.loads(result.stdout);proof['reference_library']=str(hashobj.resolve())
if a.sdk_library:proof['sdk_reference_ms']=proof.pop('sse2_ms')
(a.output/'verification.json').write_text(json.dumps(proof,indent=2));print(json.dumps(proof,indent=2))
