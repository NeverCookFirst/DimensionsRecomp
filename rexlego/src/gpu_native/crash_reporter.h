#pragma once

#if defined(_WIN32)
#include <array>
#include "crash_memory.h"
#include <cstdio>
#include <windows.h>
#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

namespace legodimensions::gpu_native {
// This callback is installed through the existing accepted runtime API. It
// runs only on a fatal exception, inside the runtime's guarded reporter call.
// ReadProcessMemory keeps the best-effort stack scan out of guest heap locks
// and stops at inaccessible memory rather than faulting the reporter itself.
inline void NativeCrashStack(uint32_t stack_pointer, char* output, size_t capacity) {
  if (!capacity) return;
  output[0] = 0;
  auto* memory = REX_KERNEL_MEMORY();
  if (!stack_pointer || !memory) {
    std::snprintf(output, capacity, "(no stack memory)");
    return;
  }
  const auto* base = memory->virtual_membase();
  uint32_t last = 0; size_t used = 0, found = 0;
  for (uint32_t i = 0; i < 128 && found < 12; ++i) {
    const uint64_t address = uint64_t(stack_pointer) + i * 4;
    if (address + 4 > 0x100000000ull) break;
    uint32_t raw = 0; SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base + address, &raw, sizeof(raw), &copied) || copied != sizeof(raw)) break;
    const uint32_t value = __builtin_bswap32(raw);
    if (value < 0x82000000 || value >= 0x84000000 || value == last) continue;
    last = value; ++found;
    const int n = std::snprintf(output + used, capacity - used, "%s%08X", used ? " <- " : "", value);
    if (n < 0 || size_t(n) >= capacity - used) break;
    used += size_t(n);
  }
  if (!found) std::snprintf(output, capacity, "(no guest code found in readable stack)");
}

inline CrashMemoryWords LogNativeCrashWords(const char* label, uint32_t address,
                                           uint32_t requested = 64) {
  auto* memory = REX_KERNEL_MEMORY();
  const auto snapshot = ReadCrashMemoryWords(memory ? memory->virtual_membase() : nullptr,
                                            address, requested);
  REXLOG_ERROR("[FATAL] Guest memory {}: address={:08X} words={} requested={}",
               label, address, snapshot.count, requested);
  for (uint32_t i = 0; i < snapshot.count; i += 8) {
    std::array<char, 80> line{}; size_t used = 0;
    for (uint32_t j = i; j < snapshot.count && j < i + 8; ++j) {
      const int n = std::snprintf(line.data() + used, line.size() - used,
                                 "%s%08X", j == i ? "" : " ", snapshot.words[j]);
      if (n <= 0 || size_t(n) >= line.size() - used) break;
      used += size_t(n);
    }
    REXLOG_ERROR("[FATAL] Guest memory {}+{:03X}: {}", label, i * 4, line.data());
  }
  return snapshot;
}

inline void NativeCrashReporter() {
  using rex::system::XThread;
  if (!XThread::IsInThread()) {
    REXLOG_ERROR("[FATAL] Crashed on a host thread with no guest context");
    return;
  }
  XThread* thread = XThread::GetCurrentThread();
  auto* state = thread ? thread->thread_state() : nullptr;
  auto* ctx = state ? state->context() : nullptr;
  if (!ctx) {
    REXLOG_ERROR("[FATAL] Crashed on a guest thread with no context to report");
    return;
  }
  REXLOG_ERROR("[FATAL] Crashed on guest tid {:#06x} ({}): lr={:08X} r3={:08X} r1={:08X}",
      thread->thread_id(), thread->name().empty() ? "unnamed" : thread->name(),
      uint32_t(ctx->lr), ctx->r3.u32, ctx->r1.u32);
  REXLOG_ERROR("[FATAL] Guest r0-r7: {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X}",
      ctx->r0.u64, ctx->r1.u64, ctx->r2.u64, ctx->r3.u64, ctx->r4.u64, ctx->r5.u64, ctx->r6.u64, ctx->r7.u64);
  REXLOG_ERROR("[FATAL] Guest r8-r15: {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X}",
      ctx->r8.u64, ctx->r9.u64, ctx->r10.u64, ctx->r11.u64, ctx->r12.u64, ctx->r13.u64, ctx->r14.u64, ctx->r15.u64);
  REXLOG_ERROR("[FATAL] Guest r16-r23: {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X}",
      ctx->r16.u64, ctx->r17.u64, ctx->r18.u64, ctx->r19.u64, ctx->r20.u64, ctx->r21.u64, ctx->r22.u64, ctx->r23.u64);
  REXLOG_ERROR("[FATAL] Guest r24-r31: {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X} {:016X}",
      ctx->r24.u64, ctx->r25.u64, ctx->r26.u64, ctx->r27.u64, ctx->r28.u64, ctx->r29.u64, ctx->r30.u64, ctx->r31.u64);
  // Guest values precede all guest-memory inspection, so an unreadable stack
  // cannot discard the operands needed to understand the original exception.
  std::array<char, 256> callers{};
  NativeCrashStack(ctx->r1.u32, callers.data(), callers.size());
  REXLOG_ERROR("[FATAL] Guest callers (stack candidates): {}", callers.data());
  // Midway's recorded fault is in sub_82EBF268 before its first nested call.
  // Capture both sides of that relationship even if the external dump races
  // process exit. This records state only; no collection or reference is edited.
  if (uint32_t(ctx->lr) == 0x82EBF270) {
    const auto owner = LogNativeCrashWords("relation-owner", ctx->r25.u32);
    const auto target = LogNativeCrashWords("relation-target", ctx->r23.u32);
    if (owner.count > 38 && owner.words[36] && owner.words[38])
      LogNativeCrashWords("owner-entries", owner.words[36]);
    if (target.count > 41 && target.words[39] && target.words[41])
      LogNativeCrashWords("target-backlinks", target.words[39]);
  }

}

inline void InstallNativeCrashReporter() {
  rex::arch::ExceptionHandler::SetCrashReporter(&NativeCrashReporter);
}
} // namespace legodimensions::gpu_native
#else
namespace legodimensions::gpu_native { inline void InstallNativeCrashReporter() {} }
#endif
