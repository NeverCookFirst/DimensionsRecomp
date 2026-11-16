#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace legodimensions::native_frontend {

// TU23 retained these three state IDs but does not populate their slots.
// Check the live arrays before taking ownership: another mod may use them.
// All calls belong to the guest frontend thread, while manager/arrays remain
// mapped. Component/controller allocations must outlive any title-side cached
// pointers, including GenericPage+176 after a slot is released. Release only
// removes array ownership; it does not make those allocations safe to free.
// This does not construct widgets, expose Options rows or run a page.
class SettingsPageBinding {
 public:
  static constexpr std::array<uint32_t, 3> kIds{21, 22, 23};
  enum class Availability { kInvalid, kBusy, kOccupied, kAvailable };

  static Availability Inspect(uint8_t* base, uint32_t manager) {
    if (!Fits(manager, 204)) return Availability::kInvalid;
    const uint32_t pages = Load(base, manager + 140), interfaces = Load(base, manager + 152);
    if (Load(base, manager + 148) <= kIds.back() || Load(base, manager + 160) <= kIds.back() ||
        !Fits(pages, (kIds.back() + 1) * 4) || !Fits(interfaces, (kIds.back() + 1) * 4))
      return Availability::kInvalid;
    for (auto id : kIds)
      if (Load(base, pages + id * 4) || Load(base, interfaces + id * 4)) return Availability::kOccupied;
    return Busy(base, manager) ? Availability::kBusy : Availability::kAvailable;
  }

  bool Install(uint8_t* base, uint32_t manager, uint32_t component,
               uint32_t controller) {
    return Install(base, manager, {component, component, component},
                   {controller, controller, controller});
  }

  // Video/Effects can own independent page components. They need not reuse
  // GenericPage, whose incoming interface is published before outgoing hide.
  bool Install(uint8_t* base, uint32_t manager,
               const std::array<uint32_t, 3>& components,
               const std::array<uint32_t, 3>& controllers) {
    if (manager_ || !Fits(manager, 204)) return false;
    for (unsigned i = 0; i < kIds.size(); ++i)
      if (!components[i] || !controllers[i]) return false;
    if (Inspect(base, manager) != Availability::kAvailable) return false;
    const uint32_t pages = Load(base, manager + 140), interfaces = Load(base, manager + 152);
    // Validate every slot before publishing any of them. There are no guest
    // calls or allocations between these writes on the frontend thread.
    for (unsigned i = 0; i < kIds.size(); ++i) {
      Store(base, pages + kIds[i] * 4, components[i]);
      Store(base, interfaces + kIds[i] * 4, controllers[i]);
    }
    manager_ = manager;
    pages_ = pages;
    interfaces_ = interfaces;
    components_ = components;
    controllers_ = controllers;
    return true;
  }

  bool Owns(uint8_t* base, uint32_t manager) const {
    if (!manager_ || manager != manager_ || Load(base, manager + 140) != pages_ ||
        Load(base, manager + 152) != interfaces_ ||
        Load(base, manager + 148) <= kIds.back() ||
        Load(base, manager + 160) <= kIds.back()) return false;
    for (unsigned i = 0; i < kIds.size(); ++i)
      if (Load(base, pages_ + kIds[i] * 4) != components_[i] ||
          Load(base, interfaces_ + kIds[i] * 4) != controllers_[i]) return false;
    return true;
  }

  // Never clear an active/pending controller or a slot replaced by another
  // owner. Leave the binding intact on failure so the caller can defer exit.
  bool Release(uint8_t* base, uint32_t manager) {
    if (!Owns(base, manager) || Busy(base, manager)) return false;
    for (auto id : kIds) {
      Store(base, pages_ + id * 4, 0);
      Store(base, interfaces_ + id * 4, 0);
    }
    manager_ = pages_ = interfaces_ = 0;
    components_.fill(0);
    controllers_.fill(0);
    return true;
  }

 private:
  static bool Fits(uint32_t address, uint32_t bytes) {
    return address && address <= UINT32_MAX - (bytes - 1);
  }
  static uint32_t Load(uint8_t* base, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
  }
  static void Store(uint8_t* base, uint32_t address, uint32_t value) {
    value = __builtin_bswap32(value);
    std::memcpy(base + address, &value, sizeof(value));
  }
  static bool Busy(uint8_t* base, uint32_t manager) {
    for (auto offset : {164u, 168u, 172u})
      for (auto id : kIds)
        if (Load(base, manager + offset) == id) return true;
    return false;
  }
  uint32_t manager_ = 0, pages_ = 0, interfaces_ = 0;
  std::array<uint32_t, 3> components_{}, controllers_{};
};

}  // namespace legodimensions::native_frontend
