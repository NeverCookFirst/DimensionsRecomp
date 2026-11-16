#pragma once
#include <cstdint>
#include <span>
#include <plume_render_interface.h>
#include "gpu_native/vertex_declarations.h"

namespace legodimensions::gpu_native {
// Xenos vertex shaders have float registers only: an integer (non-normalized)
// UBYTE4 fetch yields float(0..255). The native declaration must bind those as
// R8G8B8A8_UINT/SINT, while translated shaders declare most semantics float4,
// a D3D12 signature mismatch that reinterprets the bits (Chima spikes,
// VS 500E11F5871DD2AA). Returns the integer elements other than BLENDINDICES
// (whose shader input is already uint4) as a semantic mask.
uint32_t IntegerInputMask(std::span<const plume::RenderInputElement> inputs);
// A private variant declaring exactly those inputs uint4 and converting them
// with float(), or nullptr (logged once per shader/mask) when none was built.
plume::RenderShader* ResolveIntegerInputShader(uint64_t hash, uint32_t spec, uint32_t mask);
bool HasIntegerInputShader(uint64_t hash, uint32_t spec, uint32_t mask);
// The variant plus the input elements its pipeline must use (signed BYTE4
// elements switched to R8G8B8A8_SINT). Elements stay valid until the next call
// on this thread; GetPipeline consumes them immediately.
struct IntegerInputSelection {
  plume::RenderShader* shader = nullptr;
  const plume::RenderInputElement* elements = nullptr;
};
IntegerInputSelection SelectIntegerInputShader(uint64_t hash, uint32_t spec,
                                               const VertexDeclarationView& declaration);
bool HasIntegerInputShader(uint64_t hash, uint32_t spec, const VertexDeclarationView& declaration);
void ResetIntegerInputShaders();
}  // namespace legodimensions::gpu_native
