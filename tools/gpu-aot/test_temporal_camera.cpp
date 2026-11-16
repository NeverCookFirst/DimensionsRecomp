#include <cassert>
#include "gpu_native/temporal_camera.h"
using namespace legodimensions::gpu_native;
int main(){
 for(bool reversed:{false,true})for(float n:{0.1f,1.f,5.f})for(float f:{100.f,1000.f}){
  const float a=reversed?n/(n-f):f/(f-n),b=reversed?n*f/(f-n):-n*f/(f-n);
  // Rotated/translated camera, with off-axis perspective Y and homogeneous scaling.
  std::array<float,16>m{2,0,0,0,0,0.4f, a,1,0,-1.8f,0,0,12,17,a*31+b,31};
  for(float&v:m)v*=3;
  auto c=DecodeTemporalCamera(m);assert(c && c->inverted==reversed);
  assert(std::abs(c->near_plane-n)<n*0.001f);
  assert(std::abs(c->far_plane-f)<f*0.003f);
  assert(std::abs(c->vertical_fov-2*std::atan(1/1.8f))<0.00001f);
 }
 assert(!DecodeTemporalCamera({}));
}
