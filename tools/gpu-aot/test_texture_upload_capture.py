"""Execute native DDS capture with padded BC1/BC3/RGBA rows, no GPU or game."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
s=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
start=s.index('struct TextureCaptureState {')
end=s.index('\nbool EnsureMirror',start)
function=s[start:end]
cpp=a.output/'test.cpp'
cpp.write_text(r'''
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#ifndef _WIN32
int _putenv_s(const char* name,const char* value){return setenv(name,value,1);}
int _dupenv_s(char** value,size_t* size,const char* name){
 const char* input=std::getenv(name);*value=nullptr;*size=0;
 if(input){*size=std::strlen(input)+1;*value=static_cast<char*>(std::malloc(*size));std::memcpy(*value,input,*size);}
 return 0;
}
#endif
std::atomic<u32> g_probe_frame{0};
namespace plume {enum class RenderFormat {BC1_UNORM,BC3_UNORM,R8G8B8A8_UNORM};}
enum class ShaderStage {kVertex,kPixel};
u64 vs=0,ps=0;
u64 BoundShaderHash(ShaderStage stage){return stage==ShaderStage::kVertex?vs:ps;}
struct Fetch {u32 base_address=0,mip_address=0,swizzle=0x688,endianness=0,pitch=1,tiled=1,packed_mips=0,dimension=1;};
struct TextureResource {plume::RenderFormat format;u32 guest_address,width=5,height=7,guest_format=20,levels=1;Fetch guest_fetch;};
u32 RowPitch(plume::RenderFormat,u32){return 256;}
u32 RowCount(plume::RenderFormat f,u32 h){return f==plume::RenderFormat::R8G8B8A8_UNORM?h:(h+3)/4;}
#define REXLOG_INFO(...) ((void)0)
int warnings=0;
#define REXLOG_WARN(...) (++warnings)
'''+function+r'''
int main(int argc,char**argv){
 _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS",argv[1]);
 if(argc>4){
  _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS_PIXEL_SHADER",argv[3]);
  TextureResource r{plume::RenderFormat::BC3_UNORM,42};
  std::vector<u8> data(256*7,0xAB);
  if(argv[4][0]=='0'){
   for(int i=0;i<3;++i)DumpTextureUpload(r,data.data());
   if(warnings!=1 || !TextureCapture().dumped.empty() || TextureCapture().written_bytes ||
      std::filesystem::exists(argv[1]))return 20;
   return 0;
  }
  const u64 hash=std::strtoull(argv[3],nullptr,16);
  ps=hash^1;
  DumpTextureUpload(r,data.data());
  if(!TextureCapture().dumped.empty() || TextureCapture().written_bytes ||
     std::filesystem::exists(argv[1]))return 21;
  ps=hash;
  if(!TextureUploadCapturePending(r))return 22;
  DumpTextureUpload(r,data.data());
  if(warnings || TextureCapture().dumped.size()!=1 || !TextureCapture().written_bytes ||
     !std::filesystem::is_regular_file(std::filesystem::path(argv[1])/"42.dds"))return 23;
  return 0;
 }
 if(argc>2){
  _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS_ALL_LEVELS","1");
  _putenv_s("LEGO_DUMP_TEXTURE_UPLOADS_TRIGGER",argv[2]);
  TextureResource r{plume::RenderFormat::BC1_UNORM,42,64,16,18,7};
  std::vector<u8> data(8192,0xCD),base(4096,0xAB),mips(2048,0xEF);
  u32 offset=0;
  for(u32 level=0;level<7;++level){
   offset=(offset+511)&~511u;
   u32 w=std::max(1u,64u>>level),h=std::max(1u,16u>>level);
   u32 rows=RowCount(r.format,h),bytes=((w+3)/4)*8;
   for(u32 row=0;row<rows;++row)for(u32 b=0;b<bytes;++b)data[offset+row*256+b]=u8(level*23+row*7+b);
   offset+=256*rows;
  }
  if(TextureUploadCapturePending(r))return 1;
  DumpTextureUpload(r,data.data(),base,mips);
  if(std::filesystem::exists(std::filesystem::path(argv[1])/"42.dds"))return 2;
  std::ofstream(argv[2])<<"armed";
  ++g_probe_frame;
  if(!TextureUploadCapturePending(r))return 3;
  DumpTextureUpload(r,data.data(),base,mips);
  if(TextureUploadCapturePending(r))return 4;
  r.guest_address=43;r.guest_fetch.dimension=2;
  if(TextureUploadCapturePending(r))return 5;
  r.guest_fetch.dimension=1;
  r.guest_address=42;
  for(u32 i=0;i<32;++i)TextureCapture().dumped.insert(i+100);
  std::ofstream(argv[2])<<"lotr";++g_probe_frame;
  if(!TextureUploadCapturePending(r))return 7;
  const auto first_budget=TextureCapture().written_bytes;
  DumpTextureUpload(r,data.data(),base,mips);
  if(TextureCapture().written_bytes<=first_budget)return 8;
  std::ofstream(argv[2])<<"../invalid";++g_probe_frame;
  if(TextureUploadCapturePending(r))return 9;
  std::ofstream(argv[2])<<"armed";++g_probe_frame;
  if(TextureUploadCapturePending(r))return 10; // Refuse overwriting prior scene.
  for(const char* stage:{"third","fourth"}){
    std::ofstream(argv[2])<<stage;++g_probe_frame;
    if(!TextureUploadCapturePending(r))return 11;
    DumpTextureUpload(r,data.data(),base,mips);
  }
  std::ofstream(argv[2])<<"fifth";++g_probe_frame;
  if(TextureUploadCapturePending(r))return 12;
  TextureCapture().written_bytes=128ull*1024*1024;
  if(TextureUploadCapturePending(r))return 6;
  return 0;
 }
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
subprocess.run([a.compiler,'-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True,timeout=45)
data=a.output/'dds'
subprocess.run([str(exe.resolve()),str(data.resolve())],check=True,timeout=10)
assert not (data/'999.dds').exists()
for name,code,row_bytes,rows in [('1',b'DXT5',32,2),('2',b'DXT1',16,2),('3',b'\0'*4,20,7)]:
    b=(data/(name+'.dds')).read_bytes()
    assert b[:4]==b'DDS ' and b[84:88]==code
    assert struct.unpack_from('<II',b,12)==(7,5)
    assert len(b)==128+row_bytes*rows
    expected=bytes((row*31+column)&255 for row in range(rows) for column in range(row_bytes))
    assert b[128:]==expected
all_data=a.output/'all-mips'
trigger=a.output/'arm-capture'
trigger.unlink(missing_ok=True)
subprocess.run([str(exe.resolve()),str(all_data.resolve()),str(trigger.resolve())],check=True,timeout=10)
b=(all_data/'armed/42.dds').read_bytes()
assert struct.unpack_from('<I',b,28)[0]==7
expected=bytearray()
for level in range(7):
    w,h=max(1,64>>level),max(1,16>>level)
    for row in range((h+3)//4):
        expected.extend((level*23+row*7+column)&255 for column in range(((w+3)//4)*8))
assert b[128:]==expected
assert (all_data/'armed/42.guest-base.bin').read_bytes()==bytes([0xAB])*4096
assert (all_data/'armed/42.guest-mips.bin').read_bytes()==bytes([0xEF])*2048
assert 'levels=7' in (all_data/'armed/42.txt').read_text()
assert not (all_data/'armed/43.dds').exists()
for stage in ('lotr','third','fourth'):
    assert (all_data/stage/'42.dds').read_bytes()==b
assert not (all_data/'fifth').exists()
for index,(value,valid) in enumerate([
    ('D93BF7D40B24167D',True),('d93bf7d40b24167d',True),('0000000000000000',True),
    ('',False),('D93BF7D40B24167',False),('0D93BF7D40B24167D',False),
    ('0x3BF7D40B24167D',False),(' D93BF7D40B24167',False),
    ('D93BF7D40B24167G',False),('D93BF7D40B24167\n',False)]):
    filter_output=a.output/f'filter-{index}'
    if filter_output.exists():
        shutil.rmtree(filter_output)
    subprocess.run([str(exe.resolve()),str(filter_output.resolve()),
                    'filter',value,'1' if valid else '0'],check=True,timeout=10)
(a.output/'verification.json').write_text(json.dumps({'passed':True,'formats':['BC3/DXT5','BC1/DXT1','RGBA8'],
    'checks':['odd dimensions','256-byte row padding stripped','exact payload preserved','both TT shader variants','unrelated shader filtered',
              'late trigger','cached-upload capture request','all seven packed-tail mip sizes','512-byte mip gaps stripped',
              'exact raw guest backing','volume refused','128MiB budget',
              'rearm after full scene quota','scene files preserved','no reused scene overwrite',
              'four scene limit','invalid scene path refused','global byte budget retained',
              'exact pixel shader match','uppercase and lowercase hash','zero hash',
              'nonmatching draw consumes no capture quota','malformed hash disables capture and warns once'],
    'game_launched':False},indent=2)+'\n')
print('PASS: DXT5/DXT1/RGBA capture preserves exact rows and filters unrelated shaders')
