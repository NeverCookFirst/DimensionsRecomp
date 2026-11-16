#include "gpu_native/overlay_packet.h"
#include <cassert>
#include <cstddef>
#include <limits>
#include <random>
using namespace legodimensions::gpu_native;
static_assert(sizeof(OverlayVertex)==20&&offsetof(OverlayVertex,color)==16);
int main(){
  OverlayPacket p;p.logical_width=640;p.logical_height=360;
  OverlayDraw d;d.scissor=true;d.clip={-10,20,350,400};
  assert((OverlayScissor(d,p,1280,720)==std::array<int32_t,4>{0,40,700,720}));
  d.clip={.25f,.75f,1.75f,2.25f};
  assert((OverlayScissor(d,p,1280,720)==std::array<int32_t,4>{0,1,4,5}));
  d.clip[0]=std::numeric_limits<float>::quiet_NaN();assert((OverlayScissor(d,p,1280,720)==std::array<int32_t,4>{}));
  d.scissor=false;assert((OverlayScissor(d,p,1280,720)==std::array<int32_t,4>{0,0,1280,720}));
  p.logical_width=0;assert((OverlayScissor(d,p,1280,720)==std::array<int32_t,4>{}));
  p.vertices.resize(4);p.indices={0,1,2,0,2,3};d={};d.count=6;
  assert(OverlayDrawRange(p,d));d.base=1;assert(!OverlayDrawRange(p,d));
  d.base=0;d.first=UINT32_MAX;assert(!OverlayDrawRange(p,d));
  d.first=0;d.count=UINT32_MAX;assert(!OverlayDrawRange(p,d));
  std::mt19937 random(199603);p.logical_width=640;p.logical_height=360;d.scissor=true;
  for(unsigned i=0;i<100000;++i){
    for(auto& x:d.clip)x=float(int32_t(random()%10000)-5000)/7;
    auto a=OverlayScissor(d,p,1920,1080);
    assert(a[0]>=0&&a[2]>=0&&a[0]<=1920&&a[2]<=1920&&a[1]>=0&&a[3]>=0&&a[1]<=1080&&a[3]<=1080);
  }
}
