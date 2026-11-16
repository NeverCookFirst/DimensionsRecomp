#include <rex/ppc/context.h>

#ifdef LEGODIMENSIONS_NATIVE_GPU
#include "gpu_native/visual_effects.h"
#include "gpu_native/guest_metadata.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <array>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(native_effects_diagnostics, false, "Graphics",
    "Bounded CPU diagnostics for native bloom and cel shader selection.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_bloom, true, "Graphics",
    "Bloom glow from the game's original composition pass.");
REXCVAR_DEFINE_STRING(native_cel_shading, "original", "Graphics",
    "Cel shading: original per-world selection or everywhere on supported materials. Legacy off safely follows original lighting.")
    .allowed({"original", "everywhere", "off"});

namespace legodimensions::gpu_native {
namespace {
// Reserve evidence for each committed combination, including choices applied
// after startup. Diagnostic sampling never reads a mapped GPU upload buffer.
constexpr size_t kEffectChoices = 6;
constexpr unsigned kSamplesPerChoice = 64;
struct EffectTraceBudget {
  std::array<std::atomic<uint64_t>, kEffectChoices> calls{};
  std::array<std::atomic<unsigned>, kEffectChoices> logs{};
  bool Sample(size_t choice) {
    if (logs[choice].load(std::memory_order_relaxed) >= kSamplesPerChoice) return false;
    const auto call = calls[choice].fetch_add(1, std::memory_order_relaxed);
    return call < 1024 || (call & 255) == 0;
  }
  bool Log(size_t choice) {
    return logs[choice].fetch_add(1, std::memory_order_relaxed) < kSamplesPerChoice;
  }
};
// A mode selected after a world transition needs fresh evidence even if
// logo/loading materials exhausted this choice earlier. Each guest thread
// keeps one bounded window per observer branch. The enabled guard precedes
// all window access; no runtime state or guest memory is changed.
struct CelTraceWindow {
  size_t choice = size_t(-1);
  uint64_t calls = 0;
  unsigned logs = 0;
  unsigned generation = 0;
  bool started = false;
  std::unordered_set<uint64_t> seen;
  void Select(size_t next_choice) {
    if (choice == next_choice) return;
    choice = next_choice;
    calls = 0;
    logs = 0;
    started = false;
    seen.clear();
    ++generation;
  }
  bool Sample() {
    if (logs >= kSamplesPerChoice) return false;
    const auto call = calls++;
    return call < 1024 || (call & 255) == 0;
  }
  bool Log() { return logs++ < kSamplesPerChoice; }
};
size_t EffectTraceChoice() {
  const auto effects = CurrentNativeVisualEffects();
  return std::min(size_t(effects.cel_shading), size_t(2)) * 2 + size_t(effects.bloom);
}
}  // namespace

NativeVisualEffects CurrentNativeVisualEffects() {
  const auto packed = g_native_visual_effects.load(std::memory_order_acquire);
  if (packed & 0x80000000u)
    return {bool(packed & 1u), NativeCelShadingMode((packed >> 1) & 3u)};
  // Legacy F4/F5 use without the native transaction runtime.
  return {REXCVAR_GET(native_bloom),
      ParseNativeCelShadingMode(REXCVAR_GET(native_cel_shading)).value_or(NativeCelShadingMode::kOriginal)};
}
bool NativeEffectsDiagnosticsEnabled() {
  static const bool enabled = REXCVAR_GET(native_effects_diagnostics);
  return enabled;
}
void TraceNativeBloomBinding(uint64_t shader, uint32_t mask, bool bloom,
    uint32_t guest, uint32_t descriptor, uint64_t serial, bool repair) {
  if (!NativeEffectsDiagnosticsEnabled() ||
      (shader != UINT64_C(0x3A47E5DDE66B42C6) &&
       shader != UINT64_C(0x967A1882A7502FB3) &&
       shader != UINT64_C(0x76B999E82C424E40))) return;
  const auto choice = EffectTraceChoice();
  static EffectTraceBudget budget;
  if (!budget.Sample(choice)) return;
  static thread_local std::array<std::unordered_set<uint64_t>, kEffectChoices> seen;
  const uint64_t identity = (uint64_t(guest) << 32) ^ descriptor ^
      (uint64_t(repair) << 61) ^ shader;
  if (seen[choice].size() >= kSamplesPerChoice ||
      !seen[choice].insert(identity).second || !budget.Log(choice)) return;
  REXLOG_INFO("Native bloom binding: PS={:016X} bloom={} packed={:08X} mask={:08X} guest={:08X} descriptor={} serial={} repair={}",
      shader, bloom, g_native_visual_effects.load(), mask, guest, descriptor, serial, repair);
}
}  // namespace legodimensions::gpu_native
#endif

// TU23 mid-assembly hook, after view+331 and after the view flag bit44 test
// in 82BC4340. It changes only a predicate register. Original material variant
// availability (material+0x60), special-pass exclusions and technique binding
// remain game code. No shared guest views/materials are patched or retained.
namespace {
#ifdef LEGODIMENSIONS_NATIVE_GPU
thread_local auto selected_cel_mode = legodimensions::gpu_native::NativeCelShadingMode::kOriginal;
#endif
}
void NativeCelShadingViewGate(PPCRegister& predicate) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
  using namespace legodimensions::gpu_native;
  selected_cel_mode = CurrentNativeVisualEffects().cel_shading;
  predicate.u64 = NativeCelShadingPredicate(selected_cel_mode, predicate.u64);
#endif
}
void NativeCelShadingMaskGate(PPCRegister& predicate) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
  using namespace legodimensions::gpu_native;
  // A recorder may publish while the guest selects a technique. Both gates
  // of one selection use the same mode, already captured by the view gate.
  predicate.u64 = NativeCelShadingPredicate(selected_cel_mode, predicate.u64);
#endif
}

#ifdef LEGODIMENSIONS_NATIVE_GPU
namespace legodimensions::gpu_native {
// Branch 0 observes the candidate. Branches 1 and 2 observe the original
// default/indexed program bindings after material-specific exclusions.
void TraceNativeCelSelection(uint32_t technique, uint32_t view, uint32_t context,
    uint32_t special, uint32_t program, size_t branch) {
  if (!NativeEffectsDiagnosticsEnabled()) return;
  const auto choice = EffectTraceChoice();
  static thread_local std::array<CelTraceWindow, 3> windows;
  auto& window = windows[branch];
  window.Select(choice);
  if (!window.started) {
    window.started = true;
    REXLOG_INFO("Native cel observer active: branch={} packed={:08X} window={}",
        branch, g_native_visual_effects.load(), window.generation);
  }
  if (!window.Sample()) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return;
  auto& memory = *kernel->memory();
  std::array<uint8_t, 336> view_bytes{};
  std::array<uint8_t, 1126> context_bytes{};
  std::array<uint8_t, 100> table_bytes{};
  const auto word = [](const uint8_t* b) {
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
  };
  if (!CopyNativeGuestMetadata(memory, view, view_bytes.data(), view_bytes.size()) ||
      !CopyNativeGuestMetadata(memory, context, context_bytes.data(), context_bytes.size())) return;
  const uint32_t material = word(context_bytes.data() + 40);
  const uint32_t view_type = word(view_bytes.data() + 8);
  const bool readable = material &&
      CopyNativeGuestMetadata(memory, material, table_bytes.data(), table_bytes.size());
  const uint32_t variant = readable ? word(table_bytes.data() + 96) : 0;
  std::array<uint8_t, 112> program_bytes{};
  const bool program_readable = program &&
      CopyNativeGuestMetadata(memory, program, program_bytes.data(), program_bytes.size());
  const uint32_t vertex_shader = program_readable ? word(program_bytes.data() + 104) : 0;
  const uint32_t pixel_shader = program_readable ? word(program_bytes.data() + 108) : 0;
  const uint64_t flags = uint64_t(word(view_bytes.data() + 96)) << 32 |
      word(view_bytes.data() + 100);
  auto& seen = window.seen;
  const uint64_t identity = (uint64_t(view_type) << 48) ^
      (uint64_t(context_bytes[1120]) << 40) ^ (uint64_t(technique) << 32) ^
      (uint64_t(bool(variant)) << 23) ^ (uint64_t(bool(special)) << 22) ^
      (uint64_t(view_bytes[331]) << 8) ^ context_bytes[1125] ^ (uint64_t(program) << 1);
  if (seen.size() >= kSamplesPerChoice ||
      !seen.insert(identity).second || !window.Log()) return;
  REXLOG_INFO("Native cel selection: branch={} packed={:08X} view={:08X} type={} original331={} flags={:016X} context={:08X} material={:08X} material_type={} compile_flags={:02X} variant24={:08X} candidate={} special={} program={:08X} metadata={} vertex_shader={:08X} pixel_shader={:08X} program_metadata={} window={}",
      branch, g_native_visual_effects.load(), view, view_type, view_bytes[331], flags,
      context, material, context_bytes[1120], context_bytes[1125], variant,
      technique, special, program, readable, vertex_shader, pixel_shader, program_readable, window.generation);
}
}  // namespace legodimensions::gpu_native
#endif

// Observe-only hooks: no register, guest material or render state is changed.
void NativeCelShadingTrace(PPCRegister& technique, PPCRegister& view,
    PPCRegister& context, PPCRegister& special) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
  legodimensions::gpu_native::TraceNativeCelSelection(
      technique.u32, view.u32, context.u32, special.u32, 0, 0);
#endif
}
void NativeCelShadingDefaultBinding(PPCRegister& program, PPCRegister& index,
    PPCRegister& view, PPCRegister& context) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
  legodimensions::gpu_native::TraceNativeCelSelection(
      index.u32 / 4, view.u32, context.u32, 0, program.u32, 1);
#endif
}
void NativeCelShadingIndexedBinding(PPCRegister& program, PPCRegister& index,
    PPCRegister& view, PPCRegister& context) {
#ifdef LEGODIMENSIONS_NATIVE_GPU
  legodimensions::gpu_native::TraceNativeCelSelection(
      index.u32 / 4, view.u32, context.u32, 0, program.u32, 2);
#endif
}
