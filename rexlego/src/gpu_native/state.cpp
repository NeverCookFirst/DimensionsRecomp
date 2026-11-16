#include "gpu_native/state.h"
#include "gpu_native/d3d.h"
#include <cstdlib>

namespace legodimensions::gpu_native {
namespace {

std::mutex g_bindings_mutex;
DrawBindings g_bindings;
thread_local const DrawBindings* t_draw_bindings = nullptr;

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

void BindVertexDeclaration(u32 declaration) {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.vertex_declaration = declaration;
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

void BeginExportBinding(u32 index, u32 resource, u32 format) {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.export_index = index;
  g_bindings.export_resource = resource;
  g_bindings.export_format = format;
}

void EndExportBinding(u32 index, u32 resource, u32 format) {
  std::lock_guard lock(g_bindings_mutex);
  if (g_bindings.export_index == index &&
      g_bindings.export_resource == resource &&
      g_bindings.export_format == format) {
    g_bindings.export_index = 0;
    g_bindings.export_resource = 0;
    g_bindings.export_format = 0;
  }
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
  if (t_draw_bindings) return *t_draw_bindings;
  std::lock_guard lock(g_bindings_mutex);
  return g_bindings;
}

DrawBindingsScope::DrawBindingsScope(const DrawBindings& bindings) : previous_(t_draw_bindings) {
  t_draw_bindings = &bindings;
}
DrawBindingsScope::~DrawBindingsScope() { t_draw_bindings = previous_; }

bool SnapshotStateBindingsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_SNAPSHOT_STATE");
    return value && value[0] == '1';
  }();
  return enabled;
}

DrawBindings ReadDeviceBindings(const D3DDevice* device) {
  DrawBindings bindings;
  if (!device) return bindings;
  for (u32 i = 0; i < bindings.textures.size(); ++i) bindings.textures[i] = device->textures[i];
  for (u32 i = 0; i < bindings.render_targets.size(); ++i) bindings.render_targets[i] = device->render_targets[i];
  bindings.depth_stencil = device->depth_stencil;
  bindings.index_buffer = device->index_buffer;
  bindings.vertex_declaration = device->vertex_declaration;
  return bindings;
}

void PublishDeviceBindings(const DrawBindings& bindings) {
  std::lock_guard lock(g_bindings_mutex);
  g_bindings.textures = bindings.textures;
  g_bindings.render_targets = bindings.render_targets;
  g_bindings.depth_stencil = bindings.depth_stencil;
  g_bindings.index_buffer = bindings.index_buffer;
  g_bindings.vertex_declaration = bindings.vertex_declaration;
}

}  // namespace legodimensions::gpu_native
