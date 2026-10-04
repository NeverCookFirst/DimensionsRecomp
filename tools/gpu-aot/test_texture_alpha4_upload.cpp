#include "../../rexlego/src/gpu_native/texture_alpha4_upload.h"
#include <cassert>
#include <iostream>
#include <vector>
using legodimensions::gpu_native::CopyTextureAlpha4Blocks;

int main() {
  uint32_t cases = 0;
  for (uint32_t width : {1u,2u,3u,4u,5u,16u,31u,32u})
  for (uint32_t height : {1u,3u,4u,5u,16u,32u})
  for (uint32_t depth : {1u,3u})
  for (uint32_t endian = 0; endian < 4; ++endian)
  for (uint32_t phase = 0; phase < 16; ++phase) {
    constexpr uint32_t pitch = 256;
    const uint32_t blocks_x = (width+3)/4, blocks_y = (height+3)/4;
    // Include a packed-mip start, row padding and Z padding independently of
    // host row/slice pitch. Permute adjacent blocks to exercise offset use.
    auto offset = [=](uint32_t x,uint32_t y,uint32_t z)->int64_t {
      return 128 + uint64_t(z)*2048 + uint64_t(y)*128 + (x^1u)*8;
    };
    std::vector<uint8_t> source(size_t(offset(blocks_x-1,blocks_y-1,depth-1))+32,0xCD);
    // x^1 may address one block beyond the final logical X.
    source.resize(128+depth*2048,0xCD);
    std::vector<uint8_t> result(pitch*height*depth,0xFE);
    for(uint32_t z=0;z<depth;++z)
    for(uint32_t y=0;y<blocks_y;++y)
    for(uint32_t x=0;x<blocks_x;++x)
    for(uint32_t t=0;t<16;++t){
      const uint8_t nibble = uint8_t((phase+t+x*3+y*5+z*7)&15);
      const size_t at=(size_t(offset(x,y,z))+t/2)^endian;
      const uint32_t shift=(t&1)*4;
      source[at]=uint8_t((source[at]&~(15<<shift))|(nibble<<shift));
    }
    assert(CopyTextureAlpha4Blocks(source,result,width,height,depth,pitch,endian,offset));
    for(uint32_t z=0;z<depth;++z)
    for(uint32_t y=0;y<height;++y){
      for(uint32_t x=0;x<width;++x){
        const uint32_t t=(y%4)*4+x%4;
        const uint8_t expected=uint8_t(((phase+t+(x/4)*3+(y/4)*5+z*7)&15)*17);
        assert(result[(z*height+y)*pitch+x]==expected);
      }
      assert(result[(z*height+y)*pitch+width]==0xFE);
    }
    ++cases;
  }
  std::vector<uint8_t> block(8), output(16,0xFE);
  auto linear=[](auto,auto,auto){return int64_t{0};};
  assert(!CopyTextureAlpha4Blocks(std::span<const uint8_t>(block).first(7),output,4,4,1,4,0,linear));
  assert(!CopyTextureAlpha4Blocks(block,std::span<uint8_t>(output).first(15),4,4,1,4,0,linear));
  assert(!CopyTextureAlpha4Blocks(block,output,4,4,1,3,0,linear));
  assert(!CopyTextureAlpha4Blocks(block,output,4,4,1,4,4,linear));
  assert(!CopyTextureAlpha4Blocks(block,output,4,4,1,4,0,[](auto,auto,auto){return int64_t{-1};}));
  assert(!CopyTextureAlpha4Blocks(block,output,4,4,1,4,0,[](auto,auto,auto){return INT64_MAX;}));
  assert(!CopyTextureAlpha4Blocks(block,output,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,0,linear));
  std::cout<<"PASS: "<<cases<<" alpha4 cases, endian modes, packed offsets, odd edges, padded slices and bounded reads\n";
}
