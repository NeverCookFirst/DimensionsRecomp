#include "gpu_native/texture_writeback.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
using namespace legodimensions::gpu_native;
int main(){
 std::mt19937 random(0x9347);uint32_t cases=0;
 for(auto wh:{std::pair{1u,1u},std::pair{67u,33u},std::pair{128u,72u}}){
  auto [w,h]=wh;TextureWritebackCoverage coverage;std::vector<bool> expected(w*h);
  for(uint32_t n=0;n<1000;++n){
   const uint32_t x=random()%w,y=random()%h,rw=1+random()%(w-x),rh=1+random()%(h-y);
   coverage.Record(w,h,x,y,rw,rh);
   for(uint32_t row=y;row<y+rh;++row)for(uint32_t col=x;col<x+rw;++col)expected[row*w+col]=true;
   assert(coverage.Matches(w,h));
   for(uint32_t row=0;row<h;++row)for(uint32_t col=0;col<w;++col)
    assert(coverage.Contains(col,row)==expected[row*w+col]);
   ++cases;
  }
  coverage.Reset();assert(!coverage.Matches(w,h)&&!coverage.Contains(0,0));
  coverage.Record(w,h,0,0,w,h);assert(coverage.Matches(w,h)&&coverage.Contains(w-1,h-1));
  coverage.Record(w,h,w,0,1,1);assert(!coverage.Matches(w,h)&&!coverage.Contains(0,0));
 }
 TextureWritebackCoverage coverage;coverage.Record(4096,4096,4095,4095,1,1);
 assert(coverage.Contains(4095,4095)&&!coverage.Contains(0,0));
 coverage.Record(4097,4096,0,0,4097,4096);assert(!coverage.Matches(4097,4096));
 coverage.Reset();coverage.Record(67,33,UINT32_MAX,0,1,1);assert(!coverage.Matches(67,33));
 assert(TextureWritebackScaleSupported(1280,720,1920,1080));
 assert(TextureWritebackScaleSupported(1280,720,2560,1440));
 assert(!TextureWritebackScaleSupported(1280,720,1920,720));
 assert(!TextureWritebackScaleSupported(67,33,100,49));
 assert(!TextureWritebackScaleSupported(1280,720,640,360));
 assert(!TextureWritebackScaleSupported(0,0,0,0));
 // Integer scaling selects exactly ReXGlue's half-pixel offset in each block.
 for(uint32_t scale=1;scale<=4;++scale)for(uint32_t i=0;i<1280;++i)
  assert(TextureWritebackSample(i,1280*scale,1280)==i*scale+scale/2);
 std::printf("PASS %u coverage unions, partial/full/reset/bounds and reference center sampling\n",cases);
}
