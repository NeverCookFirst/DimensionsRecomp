#include "legodimensions_pch.h"
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

// Native CRT memset (sub_83E46040) and memcpy (sub_83E45090); overlapping, MMIO, window-crossing, wrapping or oversized ranges run the generated routine.

extern "C" void __imp__sub_83E46040(PPCContext& __restrict ctx, u8* base);
extern "C" void __imp__sub_83E45090(PPCContext& __restrict ctx, u8* base);

namespace {
enum class Mode { kOff, kOn, kValidate };

// LEGO_NATIVE_MEMOPS=0 disables the native forms; LEGO_NATIVE_MEMOPS_VALIDATE=1 runs both and compares.
Mode ReadMode() {
  const char* value = std::getenv("LEGO_NATIVE_MEMOPS");
  if (value && std::strcmp(value, "0") == 0) return Mode::kOff;
  const char* validate = std::getenv("LEGO_NATIVE_MEMOPS_VALIDATE");
  if (validate && std::strcmp(validate, "1") == 0) return Mode::kValidate;
  return Mode::kOn;
}
const Mode g_mode = ReadMode();

constexpr u32 kPhysicalWindow = 0xE0000000u;

u8* Host(u8* base, u32 address) { return base + address + (address >= kPhysicalWindow ? 0x1000u : 0u); }

// One host mapping for the whole range, no wrap, and clear of the MMIO aperture.
bool PlainRange(u32 address, u32 size) {
  const u64 end = u64(address) + size;
  if (end > 0x100000000ull) return false;
  if (address < kPhysicalWindow && end > kPhysicalWindow) return false;
  if (address < 0x80000000u && end > 0x7F000000u) return false;
  return true;
}

// Physical views at 0xA0000000, 0xC0000000 and 0xE0000000 alias one another, so overlap is judged on
// the physical address; a physical range must stay inside one view.
bool PhysicalKey(u32 address, u32 size, u64& key) {
  if (address < 0xA0000000u) {
    key = address;
    return true;
  }
  const u32 view = address < 0xC0000000u ? 0xA0000000u : address < kPhysicalWindow ? 0xC0000000u : kPhysicalWindow;
  const u64 view_end = view == kPhysicalWindow ? 0x100000000ull : u64(view) + 0x20000000u;
  if (u64(address) + size > view_end) return false;
  key = (u64(1) << 32) | ((address - view) + (view == kPhysicalWindow ? 0x1000u : 0u));
  return true;
}

bool Disjoint(u32 dst, u32 src, u32 size) {
  u64 d = 0, s = 0;
  if (!PhysicalKey(dst, size, d) || !PhysicalKey(src, size, s)) return false;
  return d + size <= s || s + size <= d;
}

void NativeMemset(PPCContext& __restrict ctx, u8* base) {
  const u64 dst = ctx.r3.u64;
  const u32 n = ctx.r5.u32;
  const u32 byte = ctx.r4.u8;
  std::memset(Host(base, u32(dst)), int(byte), n);
  // Registers exactly as the generated routine leaves them: prefix bytes to word alignment, 16-byte
  // blocks, words, then the tail.
  const u32 prefix = std::min<u32>(n, (4u - (u32(dst) & 3u)) & 3u);
  const u32 rest = n - prefix;
  const u32 tail = rest & 3u;
  ctx.r6.u64 = dst + prefix + 16ull * (rest >> 4) + 4ull * ((rest >> 2) & 3u);
  ctx.r5.u64 -= prefix;
  ctx.r4.u64 = (ctx.r4.u64 & 0xFFFFFFFF00000000ull) | (byte * 0x01010101u);
  ctx.r0.u64 = tail;
  ctx.ctr.u64 = tail == 3 ? 1 : 0;
  ctx.cr0.compare<int32_t>(int32_t(tail), 0, ctx.xer);
}

bool MemsetFast(const PPCContext& ctx) {
  return ctx.r5.u64 < 0x80000000ull && PlainRange(ctx.r3.u32, ctx.r5.u32);
}

// Memory, the r1-8 spill and r3 match the generated routine; only ABI-volatile scratch registers differ.
void NativeMemcpy(PPCContext& __restrict ctx, u8* base) {
  REX_STORE_U64(ctx.r1.u32 + -8, ctx.r3.u64);
  const u32 n = ctx.r5.u32;
  if (n) std::memcpy(Host(base, ctx.r3.u32), Host(base, ctx.r4.u32), n);
}

bool MemcpyFast(const PPCContext& ctx) {
  const u32 n = ctx.r5.u32;
  return ctx.r5.u64 < 0x80000000ull && ctx.r1.u32 >= 0x100 && ctx.r1.u32 <= kPhysicalWindow &&
         PlainRange(ctx.r3.u32, n) && PlainRange(ctx.r4.u32, n) && (n == 0 || Disjoint(ctx.r3.u32, ctx.r4.u32, n));
}

std::atomic<u64> g_validated{0};
std::atomic<u64> g_mismatches{0};

void Report(const char* name, u32 dst, u32 src, u32 n) {
  const u64 count = g_mismatches.fetch_add(1, std::memory_order_relaxed);
  if (count < 32) REXLOG_ERROR("Native {} mismatch dst={:08X} src={:08X} n={}", name, dst, src, n);
}

void NoteValidated() {
  const u64 count = g_validated.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((count & ((u64(1) << 20) - 1)) == 0)
    REXLOG_INFO("Native memops validated {} calls, {} mismatches", count, g_mismatches.load(std::memory_order_relaxed));
}

// The destination plus up to 16 guard bytes on each side, clipped to the pages the destination touches.
struct Window {
  u8* low;
  size_t size;
};

Window DestinationWindow(u8* base, u32 dst, u32 n) {
  u8* host = Host(base, dst);
  const uintptr_t first_page = reinterpret_cast<uintptr_t>(host) & ~uintptr_t(0xFFF);
  const uintptr_t last_page_end = ((reinterpret_cast<uintptr_t>(host) + n - 1) | 0xFFF) + 1;
  const uintptr_t low = std::max(first_page, reinterpret_cast<uintptr_t>(host) - 16);
  const uintptr_t high = std::min(last_page_end, reinterpret_cast<uintptr_t>(host) + n + 16);
  return {reinterpret_cast<u8*>(low), size_t(high - low)};
}

bool SameMemsetState(const PPCContext& a, const PPCContext& b) {
  return a.r0.u64 == b.r0.u64 && a.r3.u64 == b.r3.u64 && a.r4.u64 == b.r4.u64 && a.r5.u64 == b.r5.u64 &&
         a.r6.u64 == b.r6.u64 && a.ctr.u64 == b.ctr.u64 && a.cr0.lt == b.cr0.lt && a.cr0.gt == b.cr0.gt &&
         a.cr0.eq == b.cr0.eq && a.cr0.so == b.cr0.so;
}

// Runs the generated routine, rewinds the destination, runs the native one, and compares; the guard
// bytes are only read, so a generated write outside the destination shows up as a mismatch.
template <typename Native, typename Generated, typename SameState>
void Validate(PPCContext& ctx, u8* base, const char* name, Native native, Generated generated, SameState same) {
  const u32 dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0 || n > (1u << 20)) {
    native(ctx, base);
    return;
  }
  thread_local std::vector<u8> before, expected;
  u8* host = Host(base, dst);
  const Window window = DestinationWindow(base, dst, n);
  const size_t offset = size_t(host - window.low);
  before.assign(window.low, window.low + window.size);
  const PPCContext input = ctx;
  generated(ctx, base);
  const PPCContext result = ctx;
  expected.assign(window.low, window.low + window.size);
  const bool guards_same = std::memcmp(before.data(), expected.data(), offset) == 0 &&
                           std::memcmp(before.data() + offset + n, expected.data() + offset + n,
                                       window.size - offset - n) == 0;
  std::memcpy(host, before.data() + offset, n);
  ctx = input;
  native(ctx, base);
  const bool memory_same = std::memcmp(host, expected.data() + offset, n) == 0;
  if (!guards_same || !memory_same || !same(ctx, result)) {
    Report(name, dst, src, n);
    std::memcpy(host, expected.data() + offset, n);
    ctx = result;
  }
  NoteValidated();
}
}  // namespace

REX_HOOK_RAW(sub_83E46040) {
  if (g_mode == Mode::kOff || !MemsetFast(ctx)) { __imp__sub_83E46040(ctx, base); return; }
  if (g_mode == Mode::kValidate) {
    Validate(ctx, base, "memset", NativeMemset, __imp__sub_83E46040, SameMemsetState);
    return;
  }
  NativeMemset(ctx, base);
}

REX_HOOK_RAW(sub_83E45090) {
  if (g_mode == Mode::kOff || !MemcpyFast(ctx)) { __imp__sub_83E45090(ctx, base); return; }
  if (g_mode == Mode::kValidate) {
    Validate(ctx, base, "memcpy", NativeMemcpy, __imp__sub_83E45090,
             [base](const PPCContext& a, const PPCContext& b) {
               return a.r3.u64 == b.r3.u64 && REX_LOAD_U64(a.r1.u32 + -8) == REX_LOAD_U64(b.r1.u32 + -8);
             });
    return;
  }
  NativeMemcpy(ctx, base);
}
