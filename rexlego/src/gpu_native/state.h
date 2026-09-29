// CPU-side shadow of the guest draw bindings consumed by the native renderer.
#pragma once

#include <array>
#include <mutex>

#include <rex/types.h>

namespace legodimensions::gpu_native {

constexpr u32 kNativeTextureSlots = 16;
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
};

void ResetDrawBindings();
void BindTexture(u32 sampler, u32 texture);
void BindVertexStream(u32 stream, u32 buffer, u32 offset, u32 stride);
void BindIndexBuffer(u32 buffer);
void BindRenderTarget(u32 index, u32 surface);
void BindDepthStencil(u32 surface);
u32 BoundRenderTarget(u32 index);
u32 BoundDepthStencil();
DrawBindings SnapshotDrawBindings();

}  // namespace legodimensions::gpu_native
