#pragma once

#include "gpu_native/draw_work_pool.h"
#include <cstring>
#include <type_traits>

namespace legodimensions::gpu_native {
template<class Device, size_t Bytes, size_t Count = 16>
class DeviceSnapshotPool {
  static_assert(Bytes <= sizeof(Device) && std::is_trivially_copyable_v<Device>);
  struct alignas(64) Snapshot {
    Device device{};
    void Reset() {}
  };
  DrawWorkPool<Snapshot, Count> pool_;
 public:
  std::shared_ptr<Device> Capture(const Device* source) {
    auto snapshot = pool_.Acquire();
    std::memcpy(&snapshot->device, source, Bytes);
    auto* device = &snapshot->device;
    return std::shared_ptr<Device>(std::move(snapshot), device);
  }
};
}
