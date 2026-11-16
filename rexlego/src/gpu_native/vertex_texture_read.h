#pragma once
#include <plume_render_interface.h>

namespace legodimensions::gpu_native {
inline void PrepareVertexTextureShaderRead(plume::RenderCommandList* commands,
                                          plume::RenderTexture* texture) {
  // Plume's GRAPHICS SHADER_READ is pixel-only on D3D12; the combined stages add the non-pixel state vertex fetches need.
  commands->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
      plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
}
}
