// Native input-layout metadata built from Xbox 360 D3D vertex elements.
#pragma once

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
};

u32 CreateVertexDeclarationResource(u32 elements_address);
VertexDeclarationView ResolveVertexDeclaration(u32 guest_address);
bool IsNativeVertexDeclaration(u32 guest_address);
u32 AddRefNativeVertexDeclaration(u32 guest_address);
u32 ReleaseNativeVertexDeclaration(u32 guest_address);
void ResetVertexDeclarations();

}  // namespace legodimensions::gpu_native
