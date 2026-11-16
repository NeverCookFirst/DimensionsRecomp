#pragma once

#include <cstdint>
#include <cstring>
#include <rex/ppc/context.h>

namespace legodimensions::native_frontend {

// Native frontend helpers run on the guest frontend thread. Their volatile
// register changes must not escape into the surrounding translated function.
template <typename F>
uint32_t CallGuest(F fn, PPCContext& ctx, uint8_t* base, uint32_t r3,
                   uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0,
                   uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0,
                   uint32_t r10 = 0) {
  struct Restore {
    PPCContext& target;
    PPCContext saved;
    ~Restore() { target = saved; }
  } restore{ctx, ctx};
  // Match the SDK's host-to-guest caller frame (112-byte ABI area plus
  // 64 bytes of padding). Context restoration alone cannot undo callee
  // argument-home stores into the interrupted guest caller's stack.
  const auto previous_stack = ctx.r1.u32;
  ctx.r1.u64 -= 0xB0;
  const uint32_t chain = __builtin_bswap32(previous_stack);
  std::memcpy(base + ctx.r1.u32, &chain, 4);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  fn(ctx, base);
  return ctx.r3.u32;
}

// TU23's existing GenericPage component. These describe the real title API
// (sub_82FBDFF8), rather than an ImGui message window. The caller must already
// own a registered frontend page/controller and keep guest strings alive.
enum class PopupButtons : uint32_t { kAccept = 0, kAcceptCancel = 1, kNone = 3 };
struct PopupRequest {
  uint32_t component = 0;
  uint32_t message = 0;  // Guest UTF-8 string, copied by the title's text helper.
  PopupButtons buttons = PopupButtons::kAccept;
  uint32_t prompt_flags = 3;  // Native Select/Back controller hints.
  uint32_t label_count = 0;
  uint32_t labels = 0;  // Guest array of up to two big-endian string pointers.
  bool centered = false;
};

template <typename F>
bool ConfigurePopup(F show, PPCContext& ctx, uint8_t* base, const PopupRequest& request) {
  if (!request.component || !request.message || request.label_count > 2 ||
      (request.label_count && !request.labels)) return false;
  switch (request.buttons) {
    case PopupButtons::kAccept:
    case PopupButtons::kAcceptCancel:
    case PopupButtons::kNone:
      break;
    default:
      return false;
  }
  CallGuest(show, ctx, base, request.component, request.message,
            static_cast<uint32_t>(request.buttons), request.prompt_flags,
            request.label_count, request.labels, request.centered ? 1 : 0);
  return true;
}

}  // namespace legodimensions::native_frontend
