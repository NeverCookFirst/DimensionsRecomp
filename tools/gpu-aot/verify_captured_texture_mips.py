"""Reconstruct captured BC1/BC3 mip bytes with unchanged SDK layout functions.

No game launch. The DDS writer's compact bytes are checked against the saved
raw guest allocations, including real packed tails. This checks upload bytes,
not shader coordinates, sampler selection, blending or GPU-visible lifetime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def block(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    sdk = root / 'rexglue-sdk'
    util = (sdk / 'src/graphics/pipeline/texture/util.cpp').read_text()
    header = (sdk / 'include/rex/graphics/pipeline/texture/util.h').read_text()
    signatures = ['int32_t GetTiledOffset2D(', 'int32_t GetTiledOffset3D(',
                  'uint32_t GetTiledAddressUpperBound2D(', 'uint32_t GetTiledAddressUpperBound3D(',
                  'bool GetPackedMipOffset(', 'TextureGuestLayout GetGuestTextureLayout(']
    bodies = '\n'.join(block(util, s) for s in signatures)
    layout = block(header, 'struct TextureGuestLayout {') + ';'
    packed_level = block(header, 'inline uint32_t GetPackedMipLevel(')
    code = r'''
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
using u32=uint32_t;
#define assert_true(x) assert(x)
#define assert_false(x) assert(!(x))
namespace rex {
u32 align(u32 n,u32 a){return (n+a-1)&~(a-1);}
u32 log2_floor(u32 n){assert(n);return std::bit_width(n)-1;}
u32 log2_ceil(u32 n){assert(n);return std::bit_width(n-1);}
u32 next_pow2(u32 n){return std::bit_ceil(n);}
template<class T,size_t N>constexpr size_t countof(T(&)[N]){return N;}
namespace graphics {
namespace xenos {
enum class DataDimension {k1D=0,k2DOrStacked=1,k3D=2,kCube=3};
enum class TextureFormat {k_DXT1=18,k_DXT4_5=20};
constexpr u32 kTextureMaxMips=14,kTextureTileWidthHeight=32,kTextureTileDepth=4;
constexpr u32 kTextureSubresourceAlignmentBytes=4096,kTextureLinearRowAlignmentBytes=256;
}
// Only the block geometry is supplied; layout/address calculations below are
// extracted byte-for-byte from the SDK. Reject all other formats.
struct FormatInfo {
u32 block_width=4,block_height=4,bits_per_pixel=0;
u32 bytes_per_block()const{return block_width*block_height*bits_per_pixel/8;}
static const FormatInfo* Get(xenos::TextureFormat f){
 static const FormatInfo bc1{4,4,4},bc3{4,4,8};
 assert(f==xenos::TextureFormat::k_DXT1||f==xenos::TextureFormat::k_DXT4_5);
 return f==xenos::TextureFormat::k_DXT1?&bc1:&bc3;
}
};
namespace texture_util {
'''+layout+'\n'+packed_level+'\n'+bodies+r'''
} } }
std::vector<uint8_t> read(const char* p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
u32 word(const std::vector<uint8_t>& d,u32 p){assert(p+4<=d.size());u32 v;std::memcpy(&v,d.data()+p,4);return v;}
int main(int argc,char**argv){
 assert(argc==10);
 const auto dds=read(argv[1]),base=read(argv[2]),mips=read(argv[3]);
 const u32 w=std::stoul(argv[4]),h=std::stoul(argv[5]),pitch=std::stoul(argv[6]);
 const bool tiled=std::stoul(argv[7]),packed=std::stoul(argv[8]);
 const u32 endian=std::stoul(argv[9]);assert(endian<4);
 assert(word(dds,0)==0x20534444&&word(dds,12)==h&&word(dds,16)==w);
 const u32 fourcc=word(dds,84);assert(fourcc==0x31545844||fourcc==0x35545844);
 const u32 bytes=fourcc==0x31545844?8:16,levels=std::max(1u,word(dds,28));
 const auto fmt=bytes==8?rex::graphics::xenos::TextureFormat::k_DXT1:rex::graphics::xenos::TextureFormat::k_DXT4_5;
 using namespace rex::graphics::texture_util;
 const auto layout=GetGuestTextureLayout(rex::graphics::xenos::DataDimension::k2DOrStacked,pitch/32,w,h,1,tiled,fmt,packed,true,levels-1);
 const u32 xorv=endian==1?1:endian==2?3:endian==3?2:0;
 size_t at=128,checked=0;
 for(u32 level=0;level<levels;++level){
  u32 xoff=0,yoff=0,zoff=0;
  if(packed)GetPackedMipOffset(w,h,1,fmt,level,xoff,yoff,zoff);
  const auto storage_level=std::min(level,layout.packed_level);
  const auto& desc=level==0?layout.base:layout.mips[storage_level];
  const auto& raw=level==0?base:mips;
  const u32 origin=level==0?0:layout.mip_offsets_bytes[storage_level];
  const u32 bw=(std::max(1u,w>>level)+3)/4,bh=(std::max(1u,h>>level)+3)/4;
  size_t different=0;
  for(u32 y=0;y<bh;++y)for(u32 x=0;x<bw;++x){
   const int64_t offset=tiled?GetTiledOffset2D(x+xoff,y+yoff,desc.row_pitch_bytes/bytes,std::bit_width(bytes)-1)
       :int64_t(y+yoff)*desc.row_pitch_bytes+int64_t(x+xoff)*bytes;
   assert(offset>=0);
   for(u32 b=0;b<bytes;++b){
    const size_t source=origin+((size_t(offset)+b)^xorv);
    assert(source<raw.size()&&at<dds.size());
    different+=raw[source]!=dds[at++];++checked;
   }
  }
  std::cout<<"mip="<<level<<" bytes="<<bw*bh*bytes<<" differing="<<different<<"\n";
  if(different)return 2;
 }
 assert(at==dds.size());std::cout<<"PASS levels="<<levels<<" bytes="<<checked<<"\n";
}
'''
    cpp = args.output / 'replay.cpp'
    cpp.write_text(code)
    exe = args.output / 'replay.exe'
    subprocess.run([args.compiler, '-std=c++20', '-O2', str(cpp), '-o', str(exe)], check=True)
    records = []
    negative_control = None
    for capture in args.captures:
        for metadata in sorted(capture.rglob('*.txt')):
            if not metadata.with_suffix('.dds').exists():
                continue
            values = dict(re.findall(r'(\w+)=([^\s]+)', metadata.read_text()))
            if not {'pitch', 'tiled', 'packed', 'width', 'height', 'endian'} <= values.keys():
                continue
            dds, base, mips = [metadata.with_suffix(ext) for ext in ('.dds', '.guest-base.bin', '.guest-mips.bin')]
            # Single-level atlases have no mip allocation. Still verify their
            # base bytes rather than silently excluding large lightmaps.
            if not mips.exists() and values.get('levels') == '1':
                mips = args.output / 'empty-mips.bin'
                mips.write_bytes(b'')
            if not base.exists() or not mips.exists():
                records.append(dict(metadata=str(metadata), skipped='missing saved allocations'))
                continue
            if dds.read_bytes()[84:88] not in (b'DXT1', b'DXT5'):
                records.append(dict(metadata=str(metadata), skipped='not BC1/BC3'))
                continue
            command = [str(exe.resolve()), str(dds.resolve()), str(base.resolve()), str(mips.resolve())] + [values[k] for k in ('width', 'height', 'pitch', 'tiled', 'packed', 'endian')]
            result = subprocess.run(command,
                capture_output=True, text=True)
            records.append(dict(metadata=str(metadata.resolve()), passed=result.returncode == 0,
                exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr,
                hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (metadata,dds,base,mips)}))
            if negative_control is None and result.returncode == 0:
                changed = bytearray(dds.read_bytes())
                changed[128] ^= 1
                negative = args.output / 'negative-control.dds'
                negative.write_bytes(changed)
                bad = subprocess.run([command[0], str(negative.resolve())] + command[2:], capture_output=True, text=True)
                negative_control = dict(expected_exit=2, actual_exit=bad.returncode, stdout=bad.stdout, changed_dds_payload_bytes=1)
    report = dict(game_launched=False, gpu_sampling_proven=False, records=records,
        sdk_layout_sha256=hashlib.sha256(util.encode()).hexdigest(),
        sdk_layout_header_sha256=hashlib.sha256(header.encode()).hexdigest(),
        negative_control=negative_control)
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    tested = [r for r in records if 'passed' in r]
    print(f"{sum(r['passed'] for r in tested)}/{len(tested)} captured BC mip chains match saved raw bytes; {len(records)-len(tested)} skipped")
    if not tested or any(not r['passed'] for r in tested) or negative_control['actual_exit'] != 2:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
