#include <cassert>
#include <cmath>
#include "gpu_native/resolve_region.h"
#include "gpu_native/scaled_resolve.h"
using namespace legodimensions::gpu_native;
int main() {
  const ResolveRegion source{17,23,1129,635,0,0},destination{0,0,320,180,7,11};
  const auto mapping=ScaledResolveMapping(source,destination);
  assert(mapping && mapping->ratio_x != mapping->ratio_y);
  assert(std::abs((7.5f*mapping->ratio_x+mapping->offset_x)-(17+.5f*1129/320))<.00001f);
  assert(std::abs((190.5f*mapping->ratio_y+mapping->offset_y)-(23+179.5f*635/180))<.0001f);
  assert(!ScaledResolveMapping({0,0,0,635,0,0},destination));
  auto padded = CheckedResolveRegion(640, 368, 640, 360);
  assert(padded && padded->width == 640 && padded->height == 360);
  ResolveRect tile{0,0,960,960};
  for (int i = 0; i < 4; ++i) {
    ResolvePoint dst{0,960*i};
    auto r = CheckedResolveRegion(960,960,960,3840,&tile,&dst);
    assert(r && r->y == 960*i && r->height == 960);
  }
  ResolvePoint overflow{0,2881}, negative{-1,0};
  assert(!CheckedResolveRegion(960,960,960,3840,&tile,&overflow));
  assert(!CheckedResolveRegion(960,960,960,3840,&tile,&negative));
  ResolveRect outside{0,0,961,960}, inverted{5,0,4,960};
  assert(!CheckedResolveRegion(960,960,960,3840,&outside));
  assert(!CheckedResolveRegion(960,960,960,3840,&inverted));
  ResolveRect crop{16,32,80,96}; ResolvePoint offset{128,256};
  auto r = CheckedResolveRegion(960,960,960,3840,&crop,&offset);
  assert(r && r->left == 16 && r->top == 32 && r->width == 64 &&
         r->height == 64 && r->x == 128 && r->y == 256);
}
