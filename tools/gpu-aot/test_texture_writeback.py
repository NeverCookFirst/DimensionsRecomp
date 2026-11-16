"""Round-trip production RGBA writeback through the production upload decoder."""
import argparse
from pathlib import Path
import subprocess

def body(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
tile=body((root/'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(),'int32_t GetTiledOffset2D(')
fixture=r'''
#include "gpu_native/texture_writeback.h"
#include "gpu_native/texture_volume_upload.h"
#include <algorithm>
#include <cstdio>
#include <vector>
#include <stdexcept>
namespace rex {constexpr uint32_t align(uint32_t a,uint32_t n){return (a+n-1)&~(n-1);}}
namespace xenos {constexpr uint32_t kTextureTileWidthHeight=32;}
'''+tile+r'''
using namespace legodimensions::gpu_native;
void check(bool ok){if(!ok)throw std::runtime_error("writeback byte oracle");}
int main(){try{
 unsigned cases=0;
 for(auto wh:{std::pair{1u,1u},std::pair{17u,13u},std::pair{256u,256u},std::pair{67u,33u}})
 for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian)for(bool swap:{false,true}){
  auto [w,h]=wh;const unsigned pitch=((w+31)&~31u)*4,rgba_pitch=w*4+12;
  std::vector<uint8_t> rgba(rgba_pitch*h,0xee);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c)
   rgba[y*rgba_pitch+x*4+c]=uint8_t(x*17+y*29+c*61);
  auto offset=[&](unsigned x,unsigned y)->int64_t{return tiled?GetTiledOffset2D(x,y,pitch/4,2):y*pitch+x*4;};
  unsigned extent=pitch*((h+31)&~31u)*4;
  std::vector<uint8_t> backing(extent+32,0x5a),decoded(w*h*4);
  check(WritebackTextureRgba8(rgba,w,h,rgba_pitch,{backing.data()+16,extent},endian,swap,offset));
  check(CopyTextureVolumeBlocks({backing.data()+16,extent},decoded,w,h,1,4,w*4,endian,
   [&](unsigned x,unsigned y,unsigned)->int64_t{return offset(x,y);}));
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c)
   check(decoded[(y*w+x)*4+c]==rgba[y*rgba_pitch+x*4+(swap&&(c==0||c==2)?2-c:c)]);
  for(unsigned i=0;i<16;++i)check(backing[i]==0x5a&&backing[extent+16+i]==0x5a);
  auto original=backing;
  check(!WritebackTextureRgba8(rgba,w,h,rgba_pitch,{backing.data()+16,extent},endian,swap,
    [&](unsigned x,unsigned y)->int64_t{return x==w-1&&y==h-1?-1:offset(x,y);}));
  check(backing==original);
  ++cases;
 }
 std::vector<uint8_t> rgba{1,2,3,4},guest(4,0);
 check(WritebackTextureRgba8(rgba,1,1,4,guest,3,true,[](auto,auto){return 0;}));
 check((guest==std::vector<uint8_t>{4,1,2,3})); // independent endian/BGRA oracle
 auto original=guest;
 check(!WritebackTextureRgba8(rgba,1,1,4,guest,4,false,[](auto,auto){return 0;}));
 check(!WritebackTextureRgba8(rgba,1,1,3,guest,0,false,[](auto,auto){return 0;}));
 check(!WritebackTextureRgba8(rgba,1,1,4,guest,0,false,[](auto,auto){return 3;}));
 check(guest==original);
 printf("PASS %u linear/tiled/unaligned extent/endian/channel cases, exact upload inverse and atomic failures\n",cases);
 return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 23;}}
'''
for mutant in (False,True):
    header=root/'rexlego/src/gpu_native/texture_writeback.h'
    code=fixture
    if mutant:
        changed=header.read_text().replace('guest[size_t((at+c)^endian_xor)]','guest[size_t(at+c)]')
        (a.output/'mutant.h').write_text(changed)
        code=code.replace('#include "gpu_native/texture_writeback.h"','#include "mutant.h"')
    name='no-endian-mutant' if mutant else 'writeback'
    cpp=a.output/(name+'.cpp');exe=a.output/(name+'.exe');cpp.write_text(code)
    subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
    r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15)
    if r.returncode!=(23 if mutant else 0):raise RuntimeError(r.stdout+r.stderr)
    if not mutant:print(r.stdout.strip())
print('Omitted-endian mutation rejected')
