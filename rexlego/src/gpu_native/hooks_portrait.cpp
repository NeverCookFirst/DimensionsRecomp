#include "gpu_native/renderer_route.h"
#include "gpu_native/portrait_probe.h"
#include "gpu_native/long_probe.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>

extern "C" void __imp__sub_833738B8(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_83373850(PPCContext& __restrict ctx, u8* base);

namespace legodimensions::gpu_native {
namespace {
std::mutex g_portrait_mutex;
// Diagnostics only: bounded owner set, removed by the actual destructor.
std::array<u32, 32> g_portrait_owners{};

bool ReadPortraitWord(u32 address, u32& value) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !address || u64(address) + 4 > 0x100000000ull) return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap || !heap->IsRangeCommittedReadable(address, 4)) return false;
  u32 word;
  std::memcpy(&word, memory->TranslateVirtual<const u8*>(address), sizeof(word));
  value = __builtin_bswap32(word);
  return true;
}

void TracePortraitSetup(u32 owner) {
  if (!PortraitProbeEnabled()) return;
  const auto identity = ReadPortraitIdentity(owner, ReadPortraitWord);
  {
    std::lock_guard lock(g_portrait_mutex);
    if (std::find(g_portrait_owners.begin(), g_portrait_owners.end(), owner) ==
        g_portrait_owners.end())
      for (auto& entry : g_portrait_owners)
        if (!entry) { entry = owner; break; }
  }
  LongProbeEvent("portrait_setup", false, "owner=", owner, "scene=", identity.scene,
      "material=", identity.material, "texture_object=", identity.texture_object,
      "texture_backend=", identity.texture_backend, "active_index=", identity.active_texture_index,
      "texture=", identity.texture, "material_flags=", identity.material_flags,
      "texture_flags=", identity.texture_flags, "readable_fields=", identity.readable_fields);
}

void ForgetPortrait(u32 owner) {
  if (!PortraitProbeEnabled()) return;
  std::lock_guard lock(g_portrait_mutex);
  for (auto& entry : g_portrait_owners) if (entry == owner) entry = 0;
  LongProbeEvent("portrait_destroy", false, "owner=", owner);
}
}  // namespace

bool PortraitProbeEnabled() {
  static const bool requested = [] {
    const auto* value = std::getenv("LEGO_NATIVE_PORTRAIT_TRACE");
    return value && std::strcmp(value, "1") == 0;
  }();
  return requested && LongProbeEnabled();
}

void TracePortraitDraw(u32 texture, u32 slot, u64 vertex_shader, u64 pixel_shader) {
  if (!PortraitProbeEnabled() || !texture) return;
  std::lock_guard lock(g_portrait_mutex);
  static u32 previous_frame = ~u32{0}, reports = 0;
  const auto frame = g_probe_frame.load();
  if (frame != previous_frame) { previous_frame = frame; reports = 0; }
  if (reports >= 64) return;
  for (auto owner : g_portrait_owners) {
    if (!owner) continue;
    // Read current identity: setup pointers or the NuTexture's D3D resource
    // may have changed since loading. Never retain a host resource here.
    const auto identity = ReadPortraitIdentity(owner, ReadPortraitWord);
    if (identity.texture != texture || !(identity.readable_fields & 128)) continue;
    ++reports;
    LongProbeEvent("portrait_bound_draw", false, "owner=", owner,
        "material=", identity.material, "texture_object=", identity.texture_object,
        "texture_backend=", identity.texture_backend, "active_index=", identity.active_texture_index,
        "texture=", texture, "slot=", slot, "VS=", vertex_shader, "PS=", pixel_shader,
        "material_flags=", identity.material_flags, "texture_flags=", identity.texture_flags);
    break;
  }
}
}  // namespace legodimensions::gpu_native

REX_HOOK_RAW(sub_833738B8) {
  const u32 owner = ctx.r3.u32;
  __imp__sub_833738B8(ctx, base);
  legodimensions::gpu_native::TracePortraitSetup(owner);
}

REX_HOOK_RAW(sub_83373850) {
  legodimensions::gpu_native::ForgetPortrait(ctx.r3.u32);
  __imp__sub_83373850(ctx, base);
}
