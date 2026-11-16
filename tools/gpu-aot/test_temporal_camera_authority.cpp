#include "gpu_native/temporal_camera_authority.h"
#include <cassert>
#include <limits>
using legodimensions::gpu_native::TemporalCameraAuthority;
int main() {
  TemporalCameraAuthority authority;
  TemporalCameraAuthority::Matrix scene{1,0,0,0,0,1,0,0,0,0,1.001f,1,0,0,-.05f,0};
  auto ui = scene;
  // Identical projection parameters can still describe a different view.
  ui[12] = 10;
  assert(!authority.Matches(1,scene));
  authority.Observe(1,scene);
  assert(authority.Matches(1,scene));
  assert(!authority.Matches(1,ui));
  assert(!authority.Matches(2,scene));
  authority.Observe(1,ui);
  assert(!authority.Matches(1,scene) && !authority.Matches(1,ui));
  authority.Observe(1,scene);
  assert(!authority.Matches(1,scene)); // Ambiguity stays rejected this frame.
  authority.Observe(2,ui);
  assert(authority.Matches(2,ui));
  ui[0] = std::numeric_limits<float>::infinity();
  authority.Observe(2,ui);
  assert(!authority.Matches(2,ui));
  authority.Observe(3,scene);
  assert(authority.Matches(3,scene));
  authority.Reset();
  assert(!authority.Matches(3,scene));
}
