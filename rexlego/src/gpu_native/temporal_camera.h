#pragma once
#include <array>
#include <cmath>
#include <optional>
namespace legodimensions::gpu_native {
struct TemporalCamera {
  float near_plane, far_plane, vertical_fov;
  bool inverted;
};
// c0..3 are the captured row-vector world-to-clip matrix. Removing its view
// translation and off-axis Y component recovers perspective depth/FOV terms.
inline std::optional<TemporalCamera> DecodeTemporalCamera(const std::array<float,16>& m) {
  for (float value:m) if (!std::isfinite(value)) return {};
  float ww=0,zw=0,yw=0;
  for (int i=0;i<3;++i) {ww+=m[i*4+3]*m[i*4+3];zw+=m[i*4+2]*m[i*4+3];yw+=m[i*4+1]*m[i*4+3];}
  if (ww < 0.000001f) return {}; // Orthographic / unavailable projection.
  const float a=zw/ww,w=std::sqrt(ww),b=(m[14]-a*m[15])/w;
  float yy=0;
  for (int i=0;i<3;++i) {const float y=m[i*4+1]-yw/ww*m[i*4+3];yy+=y*y;}
  const bool inverted=a<0 && b>0;
  if ((!inverted && !(a>1 && b<0)) || yy<=0) return {};
  const float n=inverted?-b/(a-1):-b/a;
  const float f=inverted?-b/a:-b/(a-1);
  const float fov=2*std::atan(w/std::sqrt(yy));
  if (!std::isfinite(n)||!std::isfinite(f)||!std::isfinite(fov)||n<=0||f<=n||fov<=0||fov>=3.14159265f) return {};
  return TemporalCamera{n,f,fov,inverted};
}
}  // namespace legodimensions::gpu_native
