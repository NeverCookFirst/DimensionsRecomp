#include "legodimensions_pch.h"
#include <rex/hook.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_82A2A160(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_82A2A138(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_82A2A0E8(PPCContext& __restrict ctx, u8* base);

namespace {
bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_ATOMICS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

void SetCr0(PPCContext& ctx) {
  ctx.cr0.lt = 0;
  ctx.cr0.gt = 0;
  ctx.cr0.eq = 1;
  ctx.cr0.so = ctx.xer.so;
}

void Add(PPCContext& ctx, u8* base, u32 delta) {
  auto* p = reinterpret_cast<u32*>(REX_RAW_ADDR(ctx.r3.u32));
  std::atomic_ref<u32> word(*p);
  u32 raw = word.load(std::memory_order_relaxed);
  for (;;) {
    const u32 next = __builtin_bswap32(__builtin_bswap32(raw) + delta);
    if (word.compare_exchange_weak(raw, next, std::memory_order_seq_cst,
                                  std::memory_order_relaxed)) {
      ctx.r3.u64 = __builtin_bswap32(next);
      SetCr0(ctx);
      return;
    }
  }
}
static_assert(std::atomic_ref<u32>::is_always_lock_free);
}

REX_HOOK_RAW(sub_82A2A160) {
  if (!Enabled()) { __imp__sub_82A2A160(ctx, base); return; }
  Add(ctx, base, 1);
}

REX_HOOK_RAW(sub_82A2A138) {
  if (!Enabled()) { __imp__sub_82A2A138(ctx, base); return; }
  Add(ctx, base, u32(-1));
}

REX_HOOK_RAW(sub_82A2A0E8) {
  if (!Enabled()) { __imp__sub_82A2A0E8(ctx, base); return; }
  auto* p = reinterpret_cast<u32*>(REX_RAW_ADDR(ctx.r3.u32));
  std::atomic_ref<u32> word(*p);
  u32 raw = __builtin_bswap32(ctx.r4.u32);
  const bool exchanged = word.compare_exchange_strong(
      raw, __builtin_bswap32(ctx.r5.u32), std::memory_order_seq_cst);
  ctx.cr6.compare<int32_t>(int32_t(__builtin_bswap32(raw)), ctx.r4.s32, ctx.xer);
  ctx.r3.u64 = exchanged ? 1 : 0;
  SetCr0(ctx);
}
