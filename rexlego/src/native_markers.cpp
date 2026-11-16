// Native engine profiler markers sub_82A3E238/sub_82A3E2B8 that keep the section stack and skip the timing only the profiler reads; LEGO_NATIVE_LIGHT_MARKERS=0 runs the generated ones.
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_82A3E238(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_82A3E2B8(PPCContext& __restrict ctx, u8* base);
extern "C" void sub_82A2A2B8(PPCContext& __restrict ctx, u8* base);
extern "C" void sub_82A20188(PPCContext& __restrict ctx, u8* base);

namespace {
bool LightMarkers() {
  static const bool on = [] {
    const char* value = std::getenv("LEGO_NATIVE_LIGHT_MARKERS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return on;
}

constexpr u32 kDepth = 0x848D6EC8;      // index of the open section, -1 when none
constexpr u32 kStack = 0x848D7168;      // 28-byte entries: id, stopwatch
constexpr u32 kClockReady = 0x848D65EC; // set once the shared clock base is initialised

u32 Load32(u8* base, u32 address) { return __builtin_bswap32(*reinterpret_cast<volatile u32*>(base + address)); }
void Store32(u8* base, u32 address, u32 value) {
  *reinterpret_cast<volatile u32*>(base + address) = __builtin_bswap32(value);
}

// Calls a guest helper from inside a 112-byte frame, as the generated marker does.
template <typename F>
void CallInFrame(PPCContext& ctx, u8* base, u32 return_address, F callee) {
  const u32 frame = ctx.r1.u32 - 112;
  Store32(base, frame, ctx.r1.u32);
  ctx.r1.u64 = frame;
  ctx.lr = return_address;
  callee(ctx, base);
  ctx.r1.u64 = frame + 112;
}

void LightBegin(PPCContext& ctx, u8* base) {
  const u32 depth = Load32(base, kDepth) + 1;
  Store32(base, kDepth, depth);
  const u32 entry = kStack + depth * 28;
  Store32(base, entry, ctx.r3.u32);
  *(base + entry + 24) = 1;
  if (!*(volatile u8*)(base + kClockReady)) CallInFrame(ctx, base, 0x82A3E27C, sub_82A2A2B8);
}

void LightEnd(PPCContext& ctx, u8* base) {
  const int32_t depth = int32_t(Load32(base, kDepth));
  if (depth < 0 || ctx.r3.u32 != Load32(base, kStack + u32(depth) * 28)) {
    ctx.r3.u64 = depth < 0 ? 0x4000 : 0x4001;
    CallInFrame(ctx, base, depth < 0 ? 0x82A3E2DC : 0x82A3E304, sub_82A20188);
    return;
  }
  Store32(base, kDepth, u32(depth - 1));
}
}  // namespace

REX_HOOK_RAW(sub_82A3E238) {
  if (LightMarkers()) { LightBegin(ctx, base); return; }
  __imp__sub_82A3E238(ctx, base);
}

REX_HOOK_RAW(sub_82A3E2B8) {
  if (LightMarkers()) { LightEnd(ctx, base); return; }
  __imp__sub_82A3E2B8(ctx, base);
}
