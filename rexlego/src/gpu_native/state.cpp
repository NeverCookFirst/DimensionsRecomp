#include "gpu_native/state.h"

namespace legodimensions::gpu_native {
namespace {

std::mutex g_bindings_mutex;
DrawBindings g_bindings;

}  // namespace

void ResetDrawBindings() {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings = {};
}

void BindTexture(u32 sampler, u32 texture) {
  if (sampler >= kNativeTextureSlots) {
    return;
  }
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.textures[sampler] = texture;
}

void BindVertexStream(u32 stream, u32 buffer, u32 offset, u32 stride) {
  if (stream >= kNativeVertexStreams) {
    return;
  }
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.vertex_streams[stream] = {buffer, offset, stride};
}

void BindIndexBuffer(u32 buffer) {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.index_buffer = buffer;
}

void BindRenderTarget(u32 index, u32 surface) {
  if (index >= kNativeRenderTargets) {
    return;
  }
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.render_targets[index] = surface;
}

void BindDepthStencil(u32 surface) {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.depth_stencil = surface;
}

u32 BoundRenderTarget(u32 index) {
  if (index >= kNativeRenderTargets) {
    return 0;
  }
  std::lock_guard lock(g_bindings_mutex);
  return g_bindings.render_targets[index];
}

u32 BoundDepthStencil() {
  std::lock_guard lock(g_bindings_mutex);
  return g_bindings.depth_stencil;
}

DrawBindings SnapshotDrawBindings() {
  std::lock_guard lock(g_bindings_mutex);
  return g_bindings;
}

}  // namespace legodimensions::gpu_native
