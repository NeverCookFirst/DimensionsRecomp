// Read-only TU23 portrait identity. Offsets come from 833738B8, 82CC4E50
// and binding functions 82BCBB78 / 82BCE7F8. Object+4 is a reference count.
#pragma once
#include <cstdint>
#include <limits>

namespace legodimensions::gpu_native {
struct PortraitIdentity {
  uint32_t owner = 0;
  uint32_t scene = 0;
  uint32_t material = 0;
  uint32_t texture_object = 0;
  uint32_t texture_backend = 0;
  uint32_t active_texture_index = 0;
  uint32_t texture = 0;
  uint32_t material_flags = 0;
  uint32_t texture_flags = 0;
  uint32_t readable_fields = 0;
};

template<class ReadWord>
PortraitIdentity ReadPortraitIdentity(uint32_t owner, ReadWord&& read) {
  PortraitIdentity result;
  result.owner = owner;
  const auto field = [&](uint32_t object, uint32_t offset, uint32_t& value,
                         uint32_t bit) {
    if (!object || uint64_t(object) + offset + 4 > (uint64_t{1} << 32)) return false;
    if (!read(object + offset, value)) return false;
    result.readable_fields |= bit;
    return true;
  };
  field(owner, 84, result.scene, 1);
  if (!field(owner, 200, result.material, 2) || !result.material) return result;
  field(result.material, 948, result.material_flags, 4);
  field(result.material, 952, result.texture_flags, 8);
  if (!field(result.material, 956, result.texture_object, 16)) return result;
  // 82BCBB78 loads asset+44 into r30, then passes r30 as r6 to
  // 82BCE7F8. The asset+40 member instead supplies sampler parameters.
  if (!field(result.texture_object, 44, result.texture_backend, 32) ||
      !field(result.texture_backend, 116, result.active_texture_index, 64)) return result;
  // 82BCE7F8 passes the selected inline D3D header, NOT the backend pointer,
  // to SetTexture: backend + 120 + index * 52. Reject wrap/unreadable headers.
  const uint64_t header = uint64_t(result.texture_backend) + 120 +
      uint64_t(result.active_texture_index) * 52;
  uint32_t ignored = 0;
  if (header + 4 <= (uint64_t{1} << 32) &&
      field(uint32_t(header), 0, ignored, 128)) result.texture = uint32_t(header);
  return result;
}

bool PortraitProbeEnabled();
void TracePortraitDraw(uint32_t texture, uint32_t slot, uint64_t vertex_shader,
                       uint64_t pixel_shader);
}  // namespace legodimensions::gpu_native
