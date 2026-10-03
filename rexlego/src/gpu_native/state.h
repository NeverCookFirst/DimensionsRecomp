// CPU-side shadow of the guest draw bindings consumed by the native renderer.
#pragma once

#include <array>
#include <mutex>

#include <rex/types.h>

namespace legodimensions::gpu_native {

// TU23 samplers map directly to Xenos tfetch indices. Captured VS code uses
// slot 16, above the sixteen pixel slots; slots 26..31 are vertex fetches.
constexpr u32 kNativeTextureSlots = 26;
constexpr u32 kNativeVertexStreams = 16;
constexpr u32 kNativeRenderTargets = 4;

struct VertexStreamBinding {
  u32 buffer = 0;
  u32 offset = 0;
  u32 stride = 0;
};

struct DrawBindings {
  std::array<u32, kNativeTextureSlots> textures{};
  std::array<VertexStreamBinding, kNativeVertexStreams> vertex_streams{};
  std::array<u32, kNativeRenderTargets> render_targets{};
  u32 depth_stencil = 0;
  u32 index_buffer = 0;
  u32 vertex_declaration = 0;
  u32 export_resource = 0;
  u32 export_index = 0;
  u32 export_format = 0;
};

void ResetDrawBindings();
void BindTexture(u32 sampler, u32 texture);
void BindVertexStream(u32 stream, u32 buffer, u32 offset, u32 stride);
void BindIndexBuffer(u32 buffer);
void BindVertexDeclaration(u32 declaration);
void BindRenderTarget(u32 index, u32 surface);
void BindDepthStencil(u32 surface);
void BeginExportBinding(u32 index, u32 resource, u32 format);
void EndExportBinding(u32 index, u32 resource, u32 format);
u32 BoundRenderTarget(u32 index);
u32 BoundDepthStencil();
DrawBindings SnapshotDrawBindings();

}  // namespace legodimensions::gpu_native
