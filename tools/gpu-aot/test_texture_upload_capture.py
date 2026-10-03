"""Execute native DDS capture with padded BC1/BC3/RGBA rows, no GPU or game."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
s=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
start=s.index('void DumpTextureUpload(')
end=s.index('\nbool EnsureMirror',start)
function=s[start:end]
cpp=a.output/'test.cpp'
cpp.write_text(r'''
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
namespace plume {enum class RenderFormat {BC1_UNORM,BC3_UNORM,R8G8B8A8_UNORM};}
enum class ShaderStage {kVertex,kPixel};
u64 vs=0,ps=0;
u64 BoundShaderHash(ShaderStage stage){return stage==ShaderStage::kVertex?vs:ps;}
struct Fetch {u32 base_address=0,mip_address=0,swizzle=0x688,endianness=0,pitch=1,tiled=1,packed_mips=0;};
struct TextureResource {plume::RenderFormat format;u32 guest_address,width=5,height=7,guest_format=20;Fetch guest_fetch;};
u32 RowPitch(plume::RenderFormat,u32){return 256;}
u32 RowCount(plume::RenderFormat f,u32 h){return f==plume::RenderFormat::R8G8B8A8_UNORM?h:(h+3)/4;}
#define REXLOG_INFO(...) ((void)0)
'''+function+r'''
int main(int argc,char**argv){
 _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS",argv[1]);
 _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS_LOGOS_ONLY","1");
 std::vector<u8> data(256*7);
 for(size_t i=0;i<data.size();++i)data[i]=u8(i/256*31+i%256);
 TextureResource r{plume::RenderFormat::BC3_UNORM,999};
 DumpTextureUpload(r,data.data()); // Unrelated shader must not consume quota.
 vs=0x91007ACB3E640E3D;ps=0xEEE573BE160E037D;
 r.guest_address=1;DumpTextureUpload(r,data.data());
 r.format=plume::RenderFormat::BC1_UNORM;r.guest_address=2;DumpTextureUpload(r,data.data());
 vs=0xC3958E2D1B795ED9;ps=0x0C1840BF35E84F2F;
 r.format=plume::RenderFormat::R8G8B8A8_UNORM;r.guest_address=3;DumpTextureUpload(r,data.data());
}
''')
exe=a.output/'test.exe'
subprocess.run(['clang++','-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True)
data=a.output/'dds'
subprocess.run([str(exe.resolve()),str(data.resolve())],check=True)
assert not (data/'999.dds').exists()
for name,code,row_bytes,rows in [('1',b'DXT5',32,2),('2',b'DXT1',16,2),('3',b'\0'*4,20,7)]:
    b=(data/(name+'.dds')).read_bytes()
    assert b[:4]==b'DDS ' and b[84:88]==code
    assert struct.unpack_from('<II',b,12)==(7,5)
    assert len(b)==128+row_bytes*rows
    expected=bytes((row*31+column)&255 for row in range(rows) for column in range(row_bytes))
    assert b[128:]==expected
(a.output/'verification.json').write_text(json.dumps({'passed':True,'formats':['BC3/DXT5','BC1/DXT1','RGBA8'],
    'checks':['odd dimensions','256-byte row padding stripped','exact payload preserved','both TT shader variants','unrelated shader filtered'],
    'game_launched':False},indent=2)+'\n')
print('PASS: DXT5/DXT1/RGBA capture preserves exact rows and filters unrelated shaders')
