#include "gpu_native/d3d.h"
#include <cassert>

using namespace legodimensions::gpu_native;

int main() {
  D3DDevice device{};
  auto* bytes = reinterpret_cast<u8*>(&device);
  const auto write = [&](u32 at, u32 word) {
    for (u32 i = 0; i < 4; ++i) bytes[at + i] = u8(word >> (24 - 8 * i));
  };
  for (u32 i = 0; i < 26; ++i) write(0x3278 + 4 * i, 0xAA000000 + i * 64);
  for (u32 i = 0; i < 4; ++i) write(12816 + 4 * i, 0xBB000000 + i * 64);
  write(12832, 0xCC000000);
  write(0x320C, 0xDD000000);
  write(0x2FD0, 0xEE000000);
  const auto first = ReadDeviceBindings(&device);
  write(12816, 0xBB000100);
  write(0x3278, 0xAA000100);
  const auto second = ReadDeviceBindings(&device);
  BindVertexStream(3, 0xFF001000, 37, 48);
  BeginExportBinding(4, 0xFF002000, 5);
  PublishDeviceBindings(first);
  auto bound = SnapshotDrawBindings();
  assert(bound.textures[0] == 0xAA000000 && bound.textures[25] == 0xAA000640);
  assert(bound.render_targets[0] == 0xBB000000 && BoundRenderTarget(3) == 0xBB0000C0);
  assert(BoundDepthStencil() == 0xCC000000);
  assert(bound.index_buffer == 0xDD000000 && bound.vertex_declaration == 0xEE000000);
  assert(bound.vertex_streams[3].offset == 37 && bound.vertex_streams[3].stride == 48);
  assert(bound.export_resource == 0xFF002000);
  PublishDeviceBindings(second);
  bound = SnapshotDrawBindings();
  assert(bound.textures[0] == 0xAA000100 && bound.render_targets[0] == 0xBB000100);
  assert(bound.vertex_streams[3].buffer == 0xFF001000 && bound.export_index == 4);
  assert(first.render_targets[0] == 0xBB000000);
  ResetDrawBindings();
  assert(!BoundDepthStencil() && !SnapshotDrawBindings().export_resource);
}
