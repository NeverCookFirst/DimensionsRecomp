"""Check the actual candidate host layout and bias upload against its HLSL ABI."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('draw', type=Path)
p.add_argument('common', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
s = a.draw.read_text()
start = s.index('struct SharedConstants {')
shared = s[start:s.index('\n};', start)+3]
bias = re.search(r'    const int32_t lod_bias = [^\n]+;\n    shared.fetch_lod_bias\[i\] = [^\n]+;', s)[0]
common = a.common.read_text()
assert 'g_FetchLodBiasArr[8] : packoffset(c39)' in common
assert 'SharedConstants + 624 + (i)*4' in common
assert 'g_AlphaFunction : packoffset(c37.z)' in common
assert 'g_ViewportMode : packoffset(c37.w)' in common
code = '''#include <cstdint>
#include <cstddef>
#include <array>
#include <cassert>
#include <iostream>
using u32=uint32_t;
''' + shared + '''
int main(){
 static_assert(sizeof(SharedConstants)==752);
 static_assert(offsetof(SharedConstants,fetch_lod_bias)==624);
 static_assert(offsetof(SharedConstants,alpha_function)==600);
 static_assert(offsetof(SharedConstants,viewport_mode)==604);
 static_assert(offsetof(SharedConstants,color_output_scale)==608);
 SharedConstants initial;for(float value:initial.fetch_lod_bias)assert(value==0);
 for(uint32_t encoded=0;encoded<1024;++encoded){
  for(uint32_t i=0;i<32;++i){
   SharedConstants shared;std::array<uint32_t,6> fetch{};
   fetch[4]=0xFFC00FFFu|(encoded<<12);
''' + bias + '''
   int signed_value=encoded>=512?int(encoded)-1024:int(encoded);
   assert(shared.fetch_lod_bias[i]==float(signed_value)/32.0f);
   for(uint32_t slot=0;slot<32;++slot)if(slot!=i)assert(shared.fetch_lod_bias[slot]==0);
  }
 }
 std::cout<<"PASS: actual host upload, HLSL ABI752, all 1024 signed biases in 32 slots, old offsets preserved\\n";
}
'''
source = a.output/'lod-host-test.cpp'
source.write_text(code)
exe = a.output/'lod-host-test.exe'
subprocess.run(['clang++', '-std=c++20', str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
