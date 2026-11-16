// Native input-layout metadata built from Xbox 360 D3D vertex elements.
#pragma once

#include <memory>

#include <rex/types.h>

namespace plume {
struct RenderInputElement;
}

namespace legodimensions::gpu_native {

struct VertexDeclarationView {
  const plume::RenderInputElement* elements = nullptr;
  u32 element_count = 0;
  bool supported = false;
  u32 swapped_texcoords = 0;
  u32 swapped_normals = 0;
  u32 swapped_binormals = 0;
  u32 swapped_tangents = 0;
  u32 swapped_blend_weights = 0;
  u32 swapped_positions = 0;
  u32 sint_texcoords = 0;
  bool has_r11g11b10_normal = false;
  u64 reversed_byte_elements = 0;
  u64 content_hash = 0;
  // Guest D3DDECLTYPE per element, and the signed integer (BYTE4, 0x001A2386)
  // elements, which the declaration still binds as R8G8B8A8_UINT.
  const u32* guest_types = nullptr;
  u64 signed_integer_elements = 0;
  // 16-bit-component elements (8in16) on usages without a shader-side
  // swapFloats() (COLOR, BLENDINDICES, ...): the upload swaps their halves.
  // "Wide" ones span two dwords (16_16_16_16 formats).
  u64 half_swapped_elements = 0;
  u64 half_swapped_wide_elements = 0;
};

u32 CreateVertexDeclarationResource(u32 elements_address);
VertexDeclarationView ResolveVertexDeclaration(u32 guest_address);
bool IsNativeVertexDeclaration(u32 guest_address);
u32 AddRefNativeVertexDeclaration(u32 guest_address);
u32 ReleaseNativeVertexDeclaration(u32 guest_address);
// Registry record for capture-time pinning; null when unknown.
std::shared_ptr<void> FindDeclarationOwner(u32 guest_address);
void ResetVertexDeclarations();

// Moves on every declaration registry insert or erase.
u64 DeclarationRegistryGeneration();

}  // namespace legodimensions::gpu_native
