#include "legodimensions_pch.h"
#include "native_constants.h"
#include <rex/hook.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

extern "C" REX_FUNC(__imp__sub_82B7ABF0);
extern "C" REX_FUNC(__savegprlr_24);
extern "C" REX_FUNC(__restgprlr_24);
extern "C" REX_FUNC(sub_82BCDAD8);
extern "C" REX_FUNC(sub_83FB6480);
extern "C" REX_FUNC(sub_83FB6558);

namespace {
std::atomic<bool>& EnabledState() {
  static std::atomic<bool> enabled{[] {
    const char* value = std::getenv("LEGO_NATIVE_CONSTANT_PUBLISHER");
    return value && std::strcmp(value, "0") != 0;
  }()};
  return enabled;
}
}

namespace legodimensions {
bool NativeConstantPublisherEnabled() {
  return EnabledState().load(std::memory_order_relaxed);
}

std::optional<bool> PollNativeConstantPublisher() {
  static const char* path = std::getenv("LEGO_NATIVE_CONSTANT_PUBLISHER_FILE");
  if (!path || !*path) return {};
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next;
  static bool acknowledged = false;
  const auto now = Clock::now();
  if (now < next) return {};
  next = now + std::chrono::milliseconds(500);
  std::ifstream input(path);
  char value = 0, extra = 0;
  if (!(input >> value) || (value != '0' && value != '1') || (input >> extra)) return {};
  const bool enabled = value == '1';
  const bool previous = EnabledState().exchange(enabled, std::memory_order_relaxed);
  if (acknowledged && previous == enabled) return {};
  acknowledged = true;
  return enabled;
}
}

namespace {
struct Registers {
  u64 r3, r4, r5, r6, r7, r8, r9, r10, r11;
  void Load(const PPCContext& ctx) {
    r3 = ctx.r3.u64; r4 = ctx.r4.u64; r5 = ctx.r5.u64;
    r6 = ctx.r6.u64; r7 = ctx.r7.u64; r8 = ctx.r8.u64;
    r9 = ctx.r9.u64; r10 = ctx.r10.u64; r11 = ctx.r11.u64;
  }
  void Store(PPCContext& ctx) const {
    ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
    ctx.r6.u64 = r6; ctx.r7.u64 = r7; ctx.r8.u64 = r8;
    ctx.r9.u64 = r9; ctx.r10.u64 = r10; ctx.r11.u64 = r11;
  }
};

void Publish(PPCContext& ctx, u8* base) {
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x82B7ABF8;
  __savegprlr_24(ctx, base);
  const u32 frame = ctx.r1.u32 - 160;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;
  ctx.r25.s64 = -2070544384;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r3.u64 = REX_LOAD_U32(0x849677B8);
  ctx.lr = 0x82B7AC0C;
  sub_82BCDAD8(ctx, base);

  Registers r;
  r.Load(ctx);
  r.r11 = REX_LOAD_U32(ctx.r31.u32 + 21180);
  ctx.r24.u64 = ctx.r26.u64 = 0;
  r.r10 = REX_LOAD_U8(u32(r.r11) + 139);
  ctx.cr6.compare<u32>(u32(r.r10), 0, ctx.xer);
  if (r.r10) {
    r.r10 = r.r11;
    r.r9 = 1;
    r.r11 = u64(i64(-2072182784));
    ctx.r30.u64 = u64{1} << 63;
    ctx.r27.s64 = -2070609920;
    ctx.r28.s64 = -2072196464;
    u64 index = 0;
    do {
      r.r9 = REX_LOAD_U8(u32(r.r10) + 40);
      r.r11 = r.r9 + 5297;
      const u32 slot = u32(r.r11) << 2;
      r.r7 = REX_LOAD_U32(ctx.r31.u32 + slot);
      ctx.cr6.compare<u32>(u32(r.r7), 0, ctx.xer);
      if (r.r7) {
        r.r8 = u32(r.r9) << 2;
        r.r11 = REX_LOAD_U32(u32(r.r7));
        r.r10 = REX_LOAD_U32(ctx.r28.u32 + u32(r.r8));
        r.r3 = REX_LOAD_U32(u32(r.r10) + 20);
        r.r4 = REX_LOAD_U32(u32(r.r10) + 32);
        r.r10 = (u32(r.r3) << 8) + r.r4;
        r.r6 = r.r10 + 5337;
        r.r10 = u32(r.r6) << 2;
        r.r5 = REX_LOAD_U32(ctx.r31.u32 + u32(r.r10));
        ctx.cr6.compare<u32>(u32(r.r11), u32(r.r5), ctx.xer);
        bool changed = !ctx.cr6.eq;
        if (!changed) {
          r.r7 = REX_LOAD_U8(u32(r.r7) + 4);
          ctx.cr6.compare<u32>(u32(r.r7), 0, ctx.xer);
          changed = !ctx.cr6.eq;
        }
        if (changed) {
          REX_STORE_U32(ctx.r31.u32 + u32(r.r10), u32(r.r11));
          ctx.cr6.compare<u32>(u32(r.r9), 12, ctx.xer);
          if (ctx.cr6.eq) {
            // Slot 12 contains an indirect bone array with three vectors per bone.
            r.r10 = REX_LOAD_U32(ctx.r31.u32 + 21236);
            r.r11 = REX_LOAD_U32(0x849677B8);
            r.r9 = REX_LOAD_U32(u32(r.r10));
            r.r11 = REX_LOAD_U32(u32(r.r11) + 1036);
            r.r10 = u32(r.r11) << 1;
            r.r5 = REX_LOAD_U32(u32(r.r9));
            r.r6 = r.r11 + r.r10;
          } else {
            r.r11 = REX_LOAD_U32(ctx.r28.u32 + u32(r.r8));
            r.r10 = REX_LOAD_U32(ctx.r31.u32 + slot);
            r.r9 = REX_LOAD_U32(u32(r.r11) + 24);
            r.r5 = REX_LOAD_U32(u32(r.r10));
            r.r6 = u32(r.r9) >> 4;
          }
          r.r11 = r.r6 + r.r4;
          r.r10 = u32(r.r4) >> 2;
          r.r9 = r.r11 - 1;
          ctx.cr6.compare<i32>(i32(r.r3), 0, ctx.xer);
          r.r3 = REX_LOAD_U32(0x84950478);
          r.r8 = u32(r.r9) >> 2;
          r.r7 = r.r8 - r.r10;
          r.r11 = u32(r.r7);
          const u32 shift = (r.r11 & 127) > 63 ? 63 : u32(r.r11 & 127);
          r.r9 = ~u64{0} << (63 - shift);
          ctx.xer.ca = 0;
          r.r7 = (r.r10 & 64) ? 0 : r.r9 >> (r.r10 & 127);
          ctx.r26.u64 = index;
          ctx.r29.u64 = slot;
          r.Store(ctx);
          if (ctx.cr6.eq) {
            ctx.lr = 0x82B7AD00;
            sub_83FB6480(ctx, base);
          } else {
            ctx.lr = 0x82B7AD08;
            sub_83FB6558(ctx, base);
          }
          r.Load(ctx);
          index = ctx.r26.u64;
          r.r11 = REX_LOAD_U32(ctx.r31.u32 + ctx.r29.u32);
          REX_STORE_U8(u32(r.r11) + 4, ctx.r24.u8);
        }
      }
      r.r11 = REX_LOAD_U32(ctx.r31.u32 + 21180);
      ++index;
      r.r10 = r.r11 + index;
      r.r11 = REX_LOAD_U8(u32(r.r11) + 139);
      ctx.cr6.compare<i32>(i32(index), i32(r.r11), ctx.xer);
    } while (!ctx.cr6.eq);
  }
  r.Store(ctx);
  ctx.r1.s64 += 160;
  __restgprlr_24(ctx, base);
}
}

REX_HOOK_RAW(sub_82B7ABF0) {
  if (!legodimensions::NativeConstantPublisherEnabled()) { __imp__sub_82B7ABF0(ctx, base); return; }
  Publish(ctx, base);
}
