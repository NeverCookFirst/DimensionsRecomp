#pragma once
#include <cstring>
#include <span>
#include <plume_render_interface.h>
#include "gpu_native/shader_archive.h"

namespace legodimensions::gpu_native {
// BLENDINDICES is also used by TT terrain for normalized material weights.
// D3D12 must receive a float shader signature for float/normalized formats.
inline bool FloatBlendIndicesInput(std::span<const plume::RenderInputElement> inputs) {
  for (const auto& input : inputs) {
    if (input.semanticIndex || std::strcmp(input.semanticName, "BLENDINDICES")) continue;
    using F = plume::RenderFormat;
    switch (input.format) {
      case F::R8G8B8A8_UNORM: case F::B8G8R8A8_UNORM: case F::R8G8B8A8_SNORM:
      case F::R16G16_UNORM: case F::R16G16_SNORM: case F::R16G16_FLOAT:
      case F::R16G16B16A16_UNORM: case F::R16G16B16A16_SNORM: case F::R16G16B16A16_FLOAT:
      case F::R32_FLOAT: case F::R32G32_FLOAT: case F::R32G32B32_FLOAT:
      case F::R32G32B32A32_FLOAT: return true;
      default: return false;
    }
  }
  return false;
}
ShaderBytecode FindFloatBlendIndicesDxil(uint64_t hash, uint32_t spec);
plume::RenderShader* ResolveFloatBlendIndicesShader(uint64_t hash, uint32_t spec);
ShaderBytecode FindPositionPrecisionDxil(uint64_t hash, uint32_t spec);
plume::RenderShader* ResolvePositionPrecisionShader(uint64_t hash, uint32_t spec);
void ResetFloatBlendIndicesShaders();
} // namespace legodimensions::gpu_native
