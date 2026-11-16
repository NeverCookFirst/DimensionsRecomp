#include <cassert>
#include "gpu_native/render_extent.h"
using namespace legodimensions::gpu_native;
int main() {
  // Scene selection now belongs to fsr3_settings. Exercise edge/region math
  // with explicit host extents rather than the removed NativeSceneExtent API.
  const NativeRenderExtent s{1280,720,1920,1080}, unscaled{1280,720,1280,720};
  assert(s.width==1920 && s.height==1080 && s.ScaleX()==1.5f && s.ScaleY()==1.5f);
  auto a=NativePhysicalResolve({0,0,1280,512,0,0},s,s);
  auto b=NativePhysicalResolve({0,512,1280,208,0,512},s,s);
  assert(a && a->width==1920 && a->height==768 && b && b->top==768 && b->y==768 && b->height==312);
  assert(a->height+b->height==1080);
  assert(!NativePhysicalResolve({0,0,1280,720,0,0},s,unscaled));
  assert(!NativePhysicalResolve({0,0,1,1,0,0},{},s));
  ResolveRegion source_area{};
  auto minimap=NativePhysicalResolve({0,0,512,512,0,0},s,{512,512,512,512},&source_area);
  assert(minimap && minimap->width==512 && minimap->height==512);
  assert(source_area.width==768 && source_area.height==768);
  for (unsigned level=1;level<4;++level) {
    unsigned edge=512>>level;
    auto mip=NativePhysicalResolve({0,0,edge,edge,0,0},{320,256,320,256},
        {edge,edge,edge,edge});
    assert(mip && mip->width==edge && mip->height==edge);
  }
  const NativeRenderExtent four_k{1280,720,3840,2160};
  assert(four_k.ScaleX()==3.0f && four_k.ScaleY()==3.0f);
  auto full4k=NativePhysicalResolve({0,0,1280,720,0,0},four_k,four_k);
  assert(full4k && full4k->width==3840 && full4k->height==2160);
  for(int edge=0;edge<=1280;++edge) assert(NativeRenderEdge(edge,1280,3840)==edge*3);
  // Shared edge rounding gives adjacent tiles no overlaps or gaps, including odd bounds.
  for(int edge=0;edge<=1280;++edge) assert(NativeRenderEdge(edge,1280,1920)==edge*3/2);
  auto tile=NativePhysicalResolve({0,0,960,960,0,960},{960,960,960,960},{960,3840,960,3840});
  assert(tile && tile->width==960 && tile->height==960 && tile->y==960);
}
