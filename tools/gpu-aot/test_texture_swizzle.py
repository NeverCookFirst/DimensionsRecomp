"""Compare native channel composition against the actual SDK table/function.

No SDK files are modified, no game runs, and no game data is required.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
sdk = root/'rexglue-sdk'
ids = {name:int(value) for name,value in re.findall(r'(k_\w+) = (\d+)',
    (sdk/'include/rex/graphics/xenos.h').read_text().split('enum class TextureFormat')[1].split('};')[0])}
table = (sdk/'src/graphics/d3d12/texture_cache.cpp').read_text()
oracle = {}
for name,entry in re.findall(r'// (k_\w+)\s*\n(?:\s*//[^\n]*\n)*\s*\{([^}]+)\}',table):
    match = re.search(r'XE_GPU_TEXTURE_SWIZZLE_([RGBA]{4})\s*$',entry)
    if name in ids and match:
        oracle[ids[name]] = sum('RGBA'.index(c) << (3*i) for i,c in enumerate(match[1]))
formats = [2,8,9,10,13,22,23,25,30,31,36,37,49,58,59,
           6,14,18,19,20,21,26,32,38,54]
assert all(f in oracle for f in formats), [f for f in formats if f not in oracle]
s = (sdk/'src/graphics/pipeline/texture/cache.cpp').read_text()
start = s.index('uint32_t TextureCache::GuestToHostSwizzle(')
end = s.index('\n}\n',start)+3
function = s[start:end].replace('TextureCache::GuestToHostSwizzle','OracleSwizzle')
function = function.replace('xenos::XE_GPU_TEXTURE_SWIZZLE_0','4u')
cases = ','.join(f'{{{f},{oracle[f]}}}' for f in formats)
cpp = a.output/'test.cpp'
cpp.write_text('''#include "texture_swizzle.h"
#include <cassert>
#include <cstdint>
#include <iostream>
using namespace legodimensions::gpu_native;
'''+function+'''
int main(){
 struct Case{uint32_t format,channels;};
 Case cases[]={'''+cases+'''};
 for(auto c:cases){
  assert(TextureFormatChannelSwizzle(c.format)==c.channels);
  for(uint32_t guest=0;guest<4096;++guest)
   assert(NativeTextureSwizzle(c.format,guest)==OracleSwizzle(guest,c.channels));
 }
 assert(NativeTextureSwizzle(2,0x688)==0); // Scalar alpha was previously filled with 1.
 assert(NativeTextureSwizzle(49,0x688)==0x248); // DXN blue/alpha replicate G.
 std::cout<<"PASS: actual SDK composition and format table, all 4096 fetch swizzles per format\\n";
}
''')
exe = a.output/'test.exe'
subprocess.run(['clang++','-std=c++20','-O2','-I'+str(root/'rexlego/src/gpu_native'),
                str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
draw = (root/'rexlego/src/gpu_native/textures.cpp').read_text()
assert 'NativeTextureSwizzle(resource->guest_format, swizzle)' in draw
assert 'swizzles[(host_swizzle >> 9) & 7]' in draw
(a.output/'verification.json').write_text(json.dumps({'passed':True,
    'formats':formats,'composition_cases':len(formats)*4096,
    'oracle':'actual read-only SDK format table and GuestToHostSwizzle body',
    'game_visual_validation':False},indent=2)+'\n')
