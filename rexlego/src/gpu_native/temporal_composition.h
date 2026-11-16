#pragma once
#include <cstdint>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#include "gpu_native/render_extent.h"
namespace legodimensions::gpu_native {
inline bool NativePrecompositionEnabled() {
  static const bool enabled=[] {
    const char* value=std::getenv("LEGO_NATIVE_PRECOMPOSITION");
    return !value || std::strcmp(value,"0")!=0;
  }();
  return enabled;
}
// Exact captured final scene-image warp pair. Its color samples are in the
// pre-warp scene domain; temporal reconstruction must precede this draw.
constexpr bool IsTemporalCompositionShader(uint64_t vertex,uint64_t pixel) {
  return (vertex==0x22C61B6515A75A6Aull || vertex==0x99EA1E96047332E2ull) &&
      (pixel==0x663E3BD3A64A708Eull || pixel==0x69BC5914F33872EAull);
}
struct TemporalCompositionDraw {
  uint64_t vertex=0,pixel=0;
  NativeRenderExtent source{},target{},depth{};
  uint32_t count=0,start=0,vertex_textures=0,pixel_textures=0,color_mask=0;
  bool indexed=false,extra_targets=false,depth_test=false,stencil_test=false;
  bool blend=false,alpha_test=false,full_viewport=false,full_scissor=false,same_resource=false;
};
inline bool CanComposeTemporalScene(const TemporalCompositionDraw& draw) {
  if(!IsTemporalCompositionShader(draw.vertex,draw.pixel) || draw.indexed ||
      draw.count!=1536 || draw.start || draw.vertex_textures || draw.pixel_textures!=1 ||
      draw.extra_targets || draw.depth_test || draw.stencil_test || draw.blend ||
      draw.alpha_test || draw.color_mask!=15 || !draw.full_viewport ||
      !draw.full_scissor || draw.same_resource) return false;
  for(auto extent:{draw.source,draw.target,draw.depth})
    if(extent.logical_width!=1280 || extent.logical_height!=720 ||
        extent.width!=draw.source.width || extent.height!=draw.source.height ||
        !extent.width || !extent.height) return false;
  return true;
}
}
