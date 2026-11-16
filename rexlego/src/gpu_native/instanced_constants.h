#pragma once
#include <cstdint>
#include <plume_render_interface.h>
#include "gpu_native/shader_archive.h"

namespace legodimensions::gpu_native {
// Variants read LOAD_C(n) from StructuredBuffer<float4> t0 space5 at SV_InstanceID * 256 + n.
ShaderBytecode FindInstancedConstantsDxil(uint64_t hash, uint32_t spec);
plume::RenderShader* ResolveInstancedConstantsShader(uint64_t hash, uint32_t spec);
void ResetInstancedConstantsShaders();
} // namespace legodimensions::gpu_native
