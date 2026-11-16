#pragma once

#include "native_frontend_guest.h"

#include <array>
#include <cstring>

namespace legodimensions::native_frontend {

// All addresses are guest addresses, including callback thunks allocated by
// FunctionDispatcher. Caller owns mapped, zero-filled component/vtable storage
// of at least 28/20 bytes, and retains storage/name/thunks for the title lifetime.
// This is the native base component ABI, not a copied GenericPage object.
struct ComponentRequest {
  uint32_t component = 0, vtable = 0, name = 0;
  // destructor, event(source,event,payload), result, switch predicate, set result
  std::array<uint32_t, 5> callbacks{};
  uint32_t back_result = 19;
};

namespace component_detail {
inline bool Fits(uint32_t p, uint32_t n) { return p && p <= UINT32_MAX - (n - 1); }
inline uint32_t Load(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
inline void Store(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
}  // namespace component_detail

// ctor is TU23 sub_82EFB478. It initializes the actual list/name/root fields.
// Allocate all thunks first: a failed allocation must not publish a half-vtable.
template <typename Constructor>
bool InitializeComponent(Constructor ctor, PPCContext& ctx, uint8_t* base,
                         const ComponentRequest& r) {
  using namespace component_detail;
  if (!Fits(r.component, 28) || !Fits(r.vtable, 20) || !r.name) return false;
  for (auto callback : r.callbacks) if (!callback) return false;
  // The two owned ranges must not overlap each other.
  if (uint64_t(r.component) < uint64_t(r.vtable) + 20 &&
      uint64_t(r.vtable) < uint64_t(r.component) + 28) return false;
  for (uint32_t i = 0; i < 28; ++i) if (base[r.component + i]) return false;
  for (uint32_t i = 0; i < 20; ++i) if (base[r.vtable + i]) return false;
  CallGuest(ctor, ctx, base, r.component, r.name);
  for (unsigned i = 0; i < r.callbacks.size(); ++i)
    Store(base, r.vtable + i * 4, r.callbacks[i]);
  Store(base, r.component, r.vtable);
  Store(base, r.component + 24, r.back_result);
  return true;
}

// Register and bind only this new page. Calling the title's global bind-all
// routine again would send initialization events to every existing screen.
// add/find are sub_82EFB1E8/sub_82EF8D80. Caller validates live mapped UI/page
// objects and calls from the frontend thread with no concurrent guest mutation.
// The title owns the linked component after success; no detach/free is implied.
template <typename Add, typename Find>
bool AttachComponentPage(Add add, Find find, PPCContext& ctx, uint8_t* base,
                         uint32_t ui, uint32_t component, uint32_t page) {
  using namespace component_detail;
  if (!Fits(ui, 108) || !Fits(component, 28) || !Fits(page, 480)) return false;
  const auto list = Load(base, ui + 104), name = Load(base, component + 16);
  if (!Fits(list, 12) || !name || !Load(base, component) || !Load(base, page) ||
      Load(base, component + 4) || Load(base, component + 8) ||
      Load(base, component + 20) || Load(base, page + 476)) return false;
  if (CallGuest(find, ctx, base, list, name)) return false;
  CallGuest(add, ctx, base, ui, component);
  // Same reciprocal fields as sub_82F053C8; event 1 is deliberately separate
  // so the caller can finish validating all controls before initialization.
  Store(base, page + 476, component);
  Store(base, component + 20, page);
  return true;
}

}  // namespace legodimensions::native_frontend
