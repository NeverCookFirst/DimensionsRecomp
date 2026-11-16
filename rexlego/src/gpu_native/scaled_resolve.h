#pragma once
#include "gpu_native/resolve_region.h"

namespace legodimensions::gpu_native {
struct ScaledResolveConstants {
  float ratio_x, ratio_y, offset_x, offset_y;
};
inline std::optional<ScaledResolveConstants> ScaledResolveMapping(
    ResolveRegion input, ResolveRegion output) {
  if (!input.width || !input.height || !output.width || !output.height) return {};
  const float x=float(input.width)/output.width,y=float(input.height)/output.height;
  return ScaledResolveConstants{x,y,float(input.left)-float(output.x)*x,
      float(input.top)-float(output.y)*y};
}
}
