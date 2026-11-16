#include "legodimensions_pch.h"
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

// Native save/restore helpers with the generated stores, loads and r12/lr results, resolving the host address once; frames that could reach the 0xE0000000 window or wrap run the generated helper.

#define SAVEREST_SLOTS(X) \
  X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define SAVEREST_DECLARE(n)                                    \
  extern "C" void __imp____savegprlr_##n(PPCContext& __restrict ctx, u8* base); \
  extern "C" void __imp____restgprlr_##n(PPCContext& __restrict ctx, u8* base); \
  extern "C" void __imp____savefpr_##n(PPCContext& __restrict ctx, u8* base);   \
  extern "C" void __imp____restfpr_##n(PPCContext& __restrict ctx, u8* base);
SAVEREST_SLOTS(SAVEREST_DECLARE)

namespace {
static_assert(offsetof(PPCContext, r31) - offsetof(PPCContext, r14) == 17 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, f31) - offsetof(PPCContext, f14) == 17 * sizeof(PPCRegister));

enum class Mode { kOff, kOn, kValidate };

// LEGO_NATIVE_SAVEREST=0 disables the native helpers; LEGO_NATIVE_SAVEREST_VALIDATE=1 runs both and compares.
Mode ReadMode() {
  const char* value = std::getenv("LEGO_NATIVE_SAVEREST");
  if (value && std::strcmp(value, "0") == 0) return Mode::kOff;
  const char* validate = std::getenv("LEGO_NATIVE_SAVEREST_VALIDATE");
  if (validate && std::strcmp(validate, "1") == 0) return Mode::kValidate;
  return Mode::kOn;
}
const Mode g_mode = ReadMode();

PPCRegister* Gpr(PPCContext& ctx, int n) {
  return reinterpret_cast<PPCRegister*>(reinterpret_cast<u8*>(&ctx) + offsetof(PPCContext, r14)) + (n - 14);
}
PPCRegister* Fpr(PPCContext& ctx, int n) {
  return reinterpret_cast<PPCRegister*>(reinterpret_cast<u8*>(&ctx) + offsetof(PPCContext, f14)) + (n - 14);
}

// Everything a restore helper may write: the integer registers through lr, the FP registers, fpscr.
bool SameRestoreState(const PPCContext& a, const PPCContext& b) {
  const auto* x = reinterpret_cast<const u8*>(&a);
  const auto* y = reinterpret_cast<const u8*>(&b);
  const size_t gpr = offsetof(PPCContext, r3), gpr_end = offsetof(PPCContext, lr) + sizeof(u64);
  const size_t fpr = offsetof(PPCContext, f0), fpr_end = offsetof(PPCContext, f31) + sizeof(PPCRegister);
  return std::memcmp(x + gpr, y + gpr, gpr_end - gpr) == 0 && std::memcmp(x + fpr, y + fpr, fpr_end - fpr) == 0 &&
         a.fpscr.csr == b.fpscr.csr;
}

// The lowest slot sits 8 * (33 - n) bytes below the base register, the highest 8 bytes below it.
bool FastFrame(u32 frame) { return frame >= 0x100 && frame <= 0xE0000000u; }

template <int N>
void SaveGprLr(PPCContext& __restrict ctx, u8* base) {
  auto* slot = reinterpret_cast<volatile u64*>(base + ctx.r1.u32 - 8 * (33 - N));
  for (int i = 0; i < 32 - N; ++i) slot[i] = __builtin_bswap64(Gpr(ctx, N + i)->u64);
  *reinterpret_cast<volatile u32*>(base + ctx.r1.u32 - 8) = __builtin_bswap32(ctx.r12.u32);
}

template <int N>
void RestGprLr(PPCContext& __restrict ctx, u8* base) {
  auto* slot = reinterpret_cast<volatile u64*>(base + ctx.r1.u32 - 8 * (33 - N));
  for (int i = 0; i < 32 - N; ++i) Gpr(ctx, N + i)->u64 = __builtin_bswap64(slot[i]);
  ctx.r12.u64 = __builtin_bswap32(*reinterpret_cast<volatile u32*>(base + ctx.r1.u32 - 8));
  ctx.lr = ctx.r12.u64;
}

template <int N>
void SaveFpr(PPCContext& __restrict ctx, u8* base) {
  ctx.fpscr.disableFlushMode();
  auto* slot = reinterpret_cast<volatile u64*>(base + ctx.r12.u32 - 8 * (32 - N));
  for (int i = 0; i < 32 - N; ++i) slot[i] = __builtin_bswap64(Fpr(ctx, N + i)->u64);
}

template <int N>
void RestFpr(PPCContext& __restrict ctx, u8* base) {
  ctx.fpscr.disableFlushMode();
  auto* slot = reinterpret_cast<volatile u64*>(base + ctx.r12.u32 - 8 * (32 - N));
  for (int i = 0; i < 32 - N; ++i) Fpr(ctx, N + i)->u64 = __builtin_bswap64(slot[i]);
}

std::atomic<u64> g_validated{0};
std::atomic<u64> g_mismatches{0};

void Report(const char* name, int n, u32 frame) {
  const u64 count = g_mismatches.fetch_add(1, std::memory_order_relaxed);
  if (count < 32) REXLOG_ERROR("Native save/restore mismatch in {}_{} frame={:08X}", name, n, frame);
}

void NoteValidated() {
  const u64 count = g_validated.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((count & ((u64(1) << 26) - 1)) == 0)
    REXLOG_INFO("Native save/restore validated {} calls, {} mismatches", count,
                g_mismatches.load(std::memory_order_relaxed));
}

// Runs the generated helper, then the native one over the same input, and compares slots and registers.
template <typename Native, typename Generated>
void ValidateSave(PPCContext& ctx, u8* base, u32 frame, u32 bytes, const char* name, int n, Native native,
                  Generated generated) {
  u8* low = base + frame - bytes;
  u8 before[8 * 19], expected[8 * 19];
  std::memcpy(before, low, bytes);
  generated(ctx, base);
  std::memcpy(expected, low, bytes);
  std::memcpy(low, before, bytes);
  native(ctx, base);
  if (std::memcmp(low, expected, bytes) != 0) {
    Report(name, n, frame);
    std::memcpy(low, expected, bytes);
  }
  NoteValidated();
}

template <typename Native, typename Generated>
void ValidateRestore(PPCContext& ctx, u8* base, u32 frame, const char* name, int n, Native native,
                     Generated generated) {
  PPCContext input = ctx;
  generated(ctx, base);
  PPCContext expected = ctx;
  ctx = input;
  native(ctx, base);
  if (!SameRestoreState(ctx, expected)) {
    Report(name, n, frame);
    ctx = expected;
  }
  NoteValidated();
}
}  // namespace

#define SAVEREST_HOOKS(n)                                                                       \
  REX_HOOK_RAW(__savegprlr_##n) {                                                               \
    if (g_mode == Mode::kOff || !FastFrame(ctx.r1.u32)) { __imp____savegprlr_##n(ctx, base); return; } \
    if (g_mode == Mode::kValidate) {                                                            \
      ValidateSave(ctx, base, ctx.r1.u32, 8 * (33 - n), "savegprlr", n, SaveGprLr<n>,           \
                   __imp____savegprlr_##n);                                                     \
      return;                                                                                   \
    }                                                                                           \
    SaveGprLr<n>(ctx, base);                                                                    \
  }                                                                                             \
  REX_HOOK_RAW(__restgprlr_##n) {                                                               \
    if (g_mode == Mode::kOff || !FastFrame(ctx.r1.u32)) { __imp____restgprlr_##n(ctx, base); return; } \
    if (g_mode == Mode::kValidate) {                                                            \
      ValidateRestore(ctx, base, ctx.r1.u32, "restgprlr", n, RestGprLr<n>, __imp____restgprlr_##n); \
      return;                                                                                   \
    }                                                                                           \
    RestGprLr<n>(ctx, base);                                                                    \
  }                                                                                             \
  REX_HOOK_RAW(__savefpr_##n) {                                                                 \
    if (g_mode == Mode::kOff || !FastFrame(ctx.r12.u32)) { __imp____savefpr_##n(ctx, base); return; } \
    if (g_mode == Mode::kValidate) {                                                            \
      ValidateSave(ctx, base, ctx.r12.u32, 8 * (32 - n), "savefpr", n, SaveFpr<n>,              \
                   __imp____savefpr_##n);                                                       \
      return;                                                                                   \
    }                                                                                           \
    SaveFpr<n>(ctx, base);                                                                      \
  }                                                                                             \
  REX_HOOK_RAW(__restfpr_##n) {                                                                 \
    if (g_mode == Mode::kOff || !FastFrame(ctx.r12.u32)) { __imp____restfpr_##n(ctx, base); return; } \
    if (g_mode == Mode::kValidate) {                                                            \
      ValidateRestore(ctx, base, ctx.r12.u32, "restfpr", n, RestFpr<n>, __imp____restfpr_##n);  \
      return;                                                                                   \
    }                                                                                           \
    RestFpr<n>(ctx, base);                                                                      \
  }
SAVEREST_SLOTS(SAVEREST_HOOKS)
