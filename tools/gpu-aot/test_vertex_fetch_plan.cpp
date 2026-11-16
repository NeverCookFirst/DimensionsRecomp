// Planner bodies come verbatim from draw.cpp; resource functions are oracles whose calls, failure paths and partial work must match.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "gpu_native/state.h"
#include "gpu_native/draw_preparation.h"
#include "gpu_native/vertex_fetch_plan.h"
#include "gpu_native/buffer_window.h"
#include "gpu_native/index_draw.h"
#include "gpu_native/index_snapshot_cache.h"
#include "gpu_native/instanced_fetch.h"
#include "gpu_native/alpha_test.h"
#include "gpu_native/shader_input_usage.h"

#define REXCVAR_GET(x) true
#define REXLOG_WARN(...) ((void)0)
using namespace legodimensions::gpu_native;
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t; using i32 = int32_t;
static unsigned g_case = 0;
static unsigned g_accepted = 0, g_stamp_checks = 0, g_window_resolves = 0, g_cached_resolves = 0;
[[noreturn]] void Fail(const char* expression, int line) {
  std::fprintf(stderr, "FAIL case %u line %d: %s\n", g_case, line, expression);
  std::exit(23);
}
#define CHECK(x) do { if (!(x)) Fail(#x, __LINE__); } while (0)

namespace plume {
struct RenderInputElement {
  const char* semanticName = "POSITION";
  u32 semanticIndex = 0, slotIndex = 0, alignedByteOffset = 0, format = 12;
};
u32 RenderFormatSize(u32 format) { return format; }
struct RenderBuffer {};
}
struct Declaration {
  const plume::RenderInputElement* elements = nullptr;
  u32 element_count = 0;
  bool supported = true, has_r11g11b10_normal = false;
  u64 content_hash = 1, reversed_byte_elements = 0, half_swapped_elements = 0,
      half_swapped_wide_elements = 0;
};
enum class ShaderStage { kVertex, kPixel };
struct PipelineKey { u64 vertex_shader_hash = 0, pixel_shader_hash = 0; u32 spec_constants = 0; };
struct Bytecode { const u8* data = nullptr; u32 size = 0; };
static std::vector<u8> g_shader;
static u64 g_shader_hash = 100, g_generation = 0;
static bool g_shader_available = true, g_float_input = false, g_float_variant = false,
    g_position_variant = false, g_fsr = false, g_resolve_fail = false;
u64 BoundShaderHash(ShaderStage stage) { return g_shader_hash + (stage == ShaderStage::kPixel); }
Bytecode FindDxil(u64, u32) { return g_shader_available ? Bytecode{g_shader.data(), u32(g_shader.size())} : Bytecode{}; }
bool FloatBlendIndicesInput(std::span<const plume::RenderInputElement>) { return g_float_input; }
bool FindFloatBlendIndicesDxil(u64, u32) { return g_float_variant; }
bool FindPositionPrecisionDxil(u64, u32) { return g_position_variant; }
bool NativeFsr3Enabled() { return g_fsr; }
u64 PrecompiledShaderPackGeneration() { return g_generation; }

struct BufferResourceView {
  plume::RenderBuffer* buffer = nullptr;
  u32 length = 0, guest_format = 0, mirror_address = 0;
  u64 content_hash = 0, byte_order_hash = 0;
  bool operator==(const BufferResourceView&) const = default;
};
struct BufferPreparationSnapshot {
  std::shared_ptr<void> owner;
  BufferResourceView metadata;
  bool cached = true;
};
struct PreparedBufferVersion {
  std::shared_ptr<void> owner;
  std::shared_ptr<plume::RenderBuffer> storage;
  BufferResourceView view;
};
enum class BufferKind { kVertex, kIndex };
struct Call {
  u32 kind = 0, slot = 0, address = 0, offset = 0, length = 0, stride = 0, phase = 0;
  std::vector<u32> reversal;
  bool operator==(const Call&) const = default;
};
static std::vector<Call> g_calls;
static bool g_record = true;
void Record(Call call) { if (g_record) g_calls.push_back(std::move(call)); }
bool legodimensions::gpu_native::CpuMemoryUnchanged(const CpuMemoryStamp& stamp) {
  Record({0, 0, u32(stamp.pages.empty() ? 0 : stamp.pages[0].second)});
  return !stamp.pages.empty() && stamp.pages[0].second == 1;
}
static const auto g_buffer = std::make_shared<plume::RenderBuffer>();
BufferResourceView Resolved(u32 kind, u32 slot, u32 address, u32 offset,
    u32 length, const VertexByteOrder& order) {
  Record({kind, slot, address, offset, length, order.stride, order.stream_offset,
      {order.reversed_elements.begin(), order.reversed_elements.end()}});
  if (g_resolve_fail) return {};
  return {g_buffer.get(), length, kind, address + offset, address ^ length,
      (u64(order.stride) << 32) | order.stream_offset};
}
BufferResourceView ResolveBufferStreamWindow(u32 slot, u32 address,
    u32 offset, u32 length, const VertexByteOrder& order) {
  return Resolved(1, slot, address, offset, length, order);
}
BufferResourceView ResolveBufferIndexWindow(u32 address, u32 offset, u32 length) {
  return Resolved(2, 16, address, offset, length, {});
}
BufferResourceView ResolveBufferResourceView(u32 address, BufferKind kind,
    const VertexByteOrder& order = {}) {
  return Resolved(3, kind == BufferKind::kIndex ? 16 : 0, address, 0, 0, order);
}
PreparedBufferVersion PrepareCachedBuffer(const BufferPreparationSnapshot& input,
    u32 offset, u32 length, bool window, const VertexByteOrder& order = {}) {
  auto view = Resolved(4, window, input.metadata.mirror_address, offset, length, order);
  if (!input.cached || !view.buffer) return {};
  view.length = window ? length : input.metadata.length;
  return {input.owner, g_buffer, view};
}
struct ParallelDrawWork {
  DrawBindings bindings;
  Declaration declaration;
  bool indexed = true, geometry_ready = false, use_windows = false;
  u32 start = 0, count = 3, min_index = 0;
  i32 base_vertex = 0;
  std::shared_ptr<std::array<u8, 13000>> device = std::make_shared<std::array<u8, 13000>>();
  std::array<BufferPreparationSnapshot, 16> vertex_inputs;
  BufferPreparationSnapshot index_input;
  IndexDrawWindow index_window;
  std::shared_ptr<const IndexBoundsSnapshot> index_bounds;
  std::shared_ptr<const VertexLayoutPlan> layout;
  std::array<bool, 16> active_streams{};
  std::array<VertexBufferWindow, 16> windows{};
  std::array<PreparedBufferVersion, 16> vertices;
  PreparedBufferVersion indices;
};

#include "vertex_fetch_plan_production.inc"

void CheckLayout(const VertexLayoutPlan& left, const VertexLayoutPlan& right) {
  CHECK(left.valid == right.valid);
  for (unsigned i = 0; i < 16; ++i) {
    const auto& a = left.streams[i]; const auto& b = right.streams[i];
    CHECK(a.active == b.active && a.widths_valid == b.widths_valid &&
        a.minimum_stride == b.minimum_stride && a.reversal_count == b.reversal_count);
    CHECK(std::equal(a.Reversal().begin(), a.Reversal().end(), b.Reversal().begin(), b.Reversal().end()));
  }
}
void CheckVersion(const PreparedBufferVersion& a, const PreparedBufferVersion& b) {
  CHECK(a.owner == b.owner && a.storage == b.storage && a.view == b.view);
}
void Compare(const ParallelDrawWork& prototype, bool resolve_misses) {
  ++g_case;
  auto expected = prototype;
  g_calls.clear();
  const bool legacy = PrepareDrawGeometryLegacy(expected, resolve_misses);
  const auto calls = g_calls;
  auto actual = prototype;
  g_calls.clear();
  const bool native = PrepareDrawGeometry(actual, resolve_misses);
  CHECK(legacy == native);
  CHECK(g_calls == calls);
  g_accepted += legacy;
  for (const auto& call : calls) {
    g_stamp_checks += call.kind == 0;
    g_window_resolves += call.kind == 1 || call.kind == 2;
    g_cached_resolves += call.kind == 4;
  }
  CHECK(expected.geometry_ready == actual.geometry_ready && expected.use_windows == actual.use_windows &&
      expected.min_index == actual.min_index && expected.active_streams == actual.active_streams);
  CHECK(bool(expected.layout) == bool(actual.layout));
  if (expected.layout) CheckLayout(*expected.layout, *actual.layout);
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(expected.windows[i].offset == actual.windows[i].offset && expected.windows[i].length == actual.windows[i].length);
    CheckVersion(expected.vertices[i], actual.vertices[i]);
  }
  CheckVersion(expected.indices, actual.indices);
}

void Shader(u64 mask, bool malformed = false) {
  // A real DXBC ISGN table exercises the same signature reader as production.
  constexpr u32 records = 64, signature = 36, data = signature + 8;
  constexpr u32 name = 8 + records * 24, length = name + 9;
  g_shader.assign(data + length, 0);
  const auto word = [](u32 offset, u32 value) { std::memcpy(g_shader.data() + offset, &value, 4); };
  std::memcpy(g_shader.data(), "DXBC", 4); word(28, 1); word(32, signature);
  std::memcpy(g_shader.data() + signature, "ISGN", 4); word(signature + 4, length);
  word(data, records); std::memcpy(g_shader.data() + data + name, "POSITION", 9);
  for (u32 i = 0; i < records; ++i) {
    const u32 offset = data + 8 + i * 24;
    word(offset, name); word(offset + 4, i); g_shader[offset + 21] = (mask >> i) & 1;
  }
  if (malformed) g_shader.resize(20);
}

ParallelDrawWork Prototype(const std::vector<plume::RenderInputElement>& elements, u64 hash) {
  ParallelDrawWork work;
  work.declaration.elements = elements.data(); work.declaration.element_count = u32(elements.size());
  work.declaration.content_hash = hash;
  const auto owner = std::make_shared<unsigned>(123);
  for (u32 i = 0; i < 16; ++i) {
    work.bindings.vertex_streams[i] = {1000 + i, 0, 32};
    work.vertex_inputs[i] = {owner, {nullptr, 4096, 0, 10000 + i}};
  }
  work.bindings.index_buffer = 999;
  work.index_input = {owner, {nullptr, 4096, 1, 20000}};
  work.index_window = DrawIndexWindow(work.start, work.count, 2, 4096);
  auto bounds = std::make_shared<IndexBoundsSnapshot>();
  bounds->stamp.pages.emplace_back(0, 1); bounds->bounds = {2, 4};
  work.index_bounds = bounds;
  return work;
}

void PureRangeDifferential() {
  std::mt19937_64 random(0x82B726C8);
  for (unsigned trial = 0; trial < 30000; ++trial) {
    std::array<plume::RenderInputElement, 12> elements;
    for (unsigned i = 0; i < elements.size(); ++i) {
      elements[i] = {"POSITION", i, u32(random() % 4), u32(random() % 10) * 4, u32(random() % 5) * 4};
      if (trial % 11 == 0) elements[i].alignedByteOffset = UINT32_MAX - u32(random() % 8);
    }
    Declaration declaration{elements.data(), u32(elements.size())};
    const u64 required = random();
    const auto plan = PrepareVertexFetchPlan(declaration, required, true, [](u32 format) { return format; });
    u32 minimum = u32(random() % 20), maximum = minimum + u32(random() % 8);
    if (trial % 17 == 0) minimum = UINT32_MAX;
    const i32 base = i32(random() % 17) - 8;
    const u32 stride = u32(random() % 16) * 4, offset = u32(random() % 17), bytes = u32(random() % 2000);
    for (u32 slot = 0; slot < 4; ++slot) {
      bool legacy = true;
      for (u32 e = 0; e < elements.size(); ++e)
        if ((required & (u64{1} << e)) && elements[e].slotIndex == slot)
          legacy &= VertexFetchRangeFits(minimum, maximum, base, stride, offset,
              elements[e].alignedByteOffset, elements[e].format, bytes);
      CHECK(legacy == VertexFetchStreamFits(plan.reads[slot], minimum, maximum, base, stride, offset, bytes));
    }
  }
  std::puts("PASS 120000 per-stream comparisons against every original attribute check");
}

void CacheAndLifetime() {
  VertexFetchPlanCache cache(2);
  VertexFetchPlanCache::Key key{1, 2, 0, 0, 0, 0, 0, 3, true};
  unsigned preparations = 0;
  auto prepare = [&]() { ++preparations; auto plan = std::make_shared<VertexFetchPlan>();
    plan->layout.streams[0].minimum_stride = 123; return plan; };
  const auto first = cache.Resolve(key, prepare);
  std::shared_ptr<const VertexLayoutPlan> retained(first, &first->layout);
  CHECK(cache.Resolve(key, prepare) == first && preparations == 1);
  auto changed = key; changed.reduce_unused = false; cache.Resolve(changed, prepare);
  changed = key; changed.shader_generation = 1; cache.Resolve(changed, prepare);
  CHECK(preparations == 3 && retained->streams[0].minimum_stride == 123);
  const auto after_eviction = cache.Resolve(key, prepare);
  CHECK(after_eviction != first && preparations == 4 && retained->streams[0].minimum_stride == 123);
  for (unsigned field = 0; field < 9; ++field) {
    changed = key;
    switch (field) {
      case 0: ++changed.shader; break; case 1: ++changed.declaration; break;
      case 2: ++changed.reversed; break; case 3: ++changed.half_swapped; break;
      case 4: ++changed.half_swapped_wide; break; case 5: ++changed.shader_generation; break;
      case 6: ++changed.specialization; break; case 7: ++changed.elements; break;
      case 8: changed.reduce_unused = false; break;
    }
    const unsigned before = preparations; cache.Resolve(changed, prepare);
    CHECK(preparations == before + 1);
  }
  VertexFetchPlanCache missing;
  unsigned attempts = 0;
  auto absent = [&]() -> std::shared_ptr<const VertexFetchPlan> { ++attempts; return {}; };
  CHECK(!missing.Resolve(key, absent)); CHECK(!missing.Resolve(key, absent)); CHECK(attempts == 2);
  CHECK(missing.Resolve(key, prepare));
  std::puts("PASS complete plan keys, missing shader retry, bounded eviction and queued layout lifetime");
}

void CacheEvictionDifferential() {
  using Key = VertexFetchPlanCache::Key;
  const auto value = [](const Key& key) {
    auto plan = std::make_shared<VertexFetchPlan>();
    const u64 words[]{key.shader, key.declaration, key.reversed, key.half_swapped,
        key.half_swapped_wide, key.shader_generation, key.specialization, key.elements,
        u64(key.reduce_unused)};
    for (u32 i = 0; i < std::size(words); ++i) plan->reads[i].attribute_end = words[i];
    plan->layout.streams[0].minimum_stride = u32(key.shader);
    plan->layout.streams[0].reversal[0] = u32(key.declaration);
    plan->layout.streams[0].reversal[1] = u32(key.shader_generation);
    plan->layout.streams[0].reversal_count = 2;
    return plan;
  };
  const auto equal = [](const VertexFetchPlan& a, const VertexFetchPlan& b) {
    CheckLayout(a.layout, b.layout);
    CHECK(a.upload_streams == b.upload_streams);
    for (u32 i = 0; i < a.reads.size(); ++i) {
      CHECK(a.reads[i].attribute_end == b.reads[i].attribute_end);
      CHECK(a.reads[i].required == b.reads[i].required && a.reads[i].widths_valid == b.reads[i].widths_valid);
    }
  };
  // Six hot pairs plus two new pairs per round overflow the eight-entry cache: one-entry eviction keeps the hot pairs, clear-all rebuilds them.
  VertexFetchPlanCache old(8), recent(8);
  u32 old_builds = 0, recent_builds = 0, calls = 0;
  for (u32 round = 0; round < 100; ++round) {
    for (u32 i = 0; i < 8; ++i) {
      const Key key{i < 6 ? 1 + i : 1000 + round * 2 + i - 6, 2, 0, 0, 0, 0, 0, 3, true};
      auto a = old.Resolve(key, [&] { ++old_builds; return value(key); }, false);
      auto b = recent.Resolve(key, [&] { ++recent_builds; return value(key); }, true);
      equal(*a, *b); CHECK(old.Size() <= 8 && recent.Size() <= 8); ++calls;
    }
  }
  CHECK(recent_builds == 206 && old_builds == 794);
  const auto old_stats = old.TakeStats(), recent_stats = recent.TakeStats();
  CHECK(old_stats.misses == old_builds && recent_stats.misses == recent_builds);
  CHECK(old_stats.hits + old_stats.misses == calls && recent_stats.hits + recent_stats.misses == calls);
  CHECK(recent_stats.evicted == recent_builds - 8 && old_stats.evicted == 792);
  CHECK(!old_stats.uncached && !recent_stats.uncached);
  CHECK(!recent.TakeStats().hits && !recent.TakeStats().misses);
  std::printf("POLICY FIXTURE 800 requests: clear-all constructions=%u LRU constructions=%u; no gameplay claim\n",
      old_builds, recent_builds);

  // Retained work owns its plan independently of either cache and policy.
  std::vector<std::pair<std::shared_ptr<const VertexLayoutPlan>, u32>> retained;
  std::mt19937_64 random(0x82B73D48);
  {
    VertexFetchPlanCache baseline(7), toggled(7);
    for (u32 i = 0; i < 20000; ++i) {
      Key key{1 + random() % 23, 1 + random() % 5, random() % 2, random() % 2,
          random() % 2, random() % 3, u32(random() % 4), 3, bool(random() & 1)};
      auto a = baseline.Resolve(key, [&] { return value(key); }, false);
      auto b = toggled.Resolve(key, [&] { return value(key); }, bool((i / 17) & 1));
      const auto expected = value(key); equal(*a, *expected); equal(*b, *expected);
      CHECK(baseline.Size() <= 7 && toggled.Size() <= 7);
      if (i % 127 == 0) retained.emplace_back(
          std::shared_ptr<const VertexLayoutPlan>(b, &b->layout), u32(key.shader));
    }
  }
  for (const auto& [layout, stride] : retained) CHECK(layout->streams[0].minimum_stride == stride);
  // An unavailable/invalid key must not evict any retained usable entry.
  const Key valid{91, 92, 0, 0, 0, 0, 0, 1, true};
  for (const bool lru : {false, true}) {
    VertexFetchPlanCache cache(1);
    const auto held = cache.Resolve(valid, [&] { return value(valid); }, lru);
    CHECK(!cache.Resolve(Key{93, 92}, []() -> std::shared_ptr<const VertexFetchPlan> { return {}; }, lru));
    auto invalid = valid; invalid.shader = 0;
    CHECK(cache.Resolve(invalid, [&] { return value(invalid); }, lru));
    CHECK(cache.Resolve(valid, [&] { Fail("hot entry lost on non-cacheable miss", __LINE__); return value(valid); }, lru) == held);
    CHECK(cache.Size() == 1);
    const auto stats = cache.TakeStats();
    CHECK(stats.hits == 1 && stats.misses == 3 && stats.uncached == 2 && !stats.evicted);
  }
  std::puts("PASS 20800 old/new cache semantic comparisons, policy changes, exact bounds, counters and post-eviction/post-destruction ownership");
}

void ProductionDifferential() {
  std::mt19937_64 random(0x84959EDC);
  for (unsigned trial = 0; trial < 2400; ++trial) {
    std::vector<plume::RenderInputElement> elements(1 + random() % 14);
    for (u32 i = 0; i < elements.size(); ++i)
      elements[i] = {"POSITION", i, u32(random() % 4), u32(random() % 5) * 4, 4u * (1 + u32(random() % 4))};
    const u32 selected = u32(random() % elements.size());
    switch (trial % 12) {
      case 0: elements[selected].format = 0; break;
      case 1: elements[selected].alignedByteOffset = UINT32_MAX; break;
      case 2: elements[selected].slotIndex = 16; break;
      default: break;
    }
    g_shader_hash = 100000 + trial * 16;
    g_shader_available = trial % 19 != 0;
    g_float_input = trial % 3 == 0; g_float_variant = trial % 5 == 0;
    g_position_variant = trial % 7 == 0; g_fsr = trial % 11 == 0;
    g_resolve_fail = trial % 23 == 0;
    Shader(random(), trial % 29 == 0);
    auto work = Prototype(elements, 200000 + trial);
    work.declaration.reversed_byte_elements = random();
    work.declaration.half_swapped_elements = random();
    work.declaration.half_swapped_wide_elements = random();
    work.declaration.has_r11g11b10_normal = trial & 1;
    *reinterpret_cast<be_u32*>(work.device->data() + 10556) = trial & 2 ? 8u : 0u;
    if (trial % 31 == 0) work.declaration.supported = false;
    if (trial % 37 == 0) work.declaration.content_hash = 0;
    if (trial % 41 == 0) g_shader_hash = 0x500E11F5871DD2AAull;
    work.indexed = trial % 3 != 0;
    work.start = u32(random() % 20); work.count = 1 + u32(random() % 15);
    work.base_vertex = i32(random() % 13) - 6;
    if (trial % 43 == 0) { work.start = UINT32_MAX; work.count = 2; }
    if (trial % 47 == 0) work.count = 0;
    for (u32 i = 0; i < 16; ++i) {
      auto& binding = work.bindings.vertex_streams[i];
      binding.buffer = random() % 5 ? binding.buffer : 0;
      binding.offset = u32(random() % 13);
      binding.stride = random() % 7 ? 32 : 0;
      work.vertex_inputs[i].cached = random() % 13 != 0;
      work.vertex_inputs[i].metadata.length = random() % 7 ? 4096 : 56;
    }
    work.index_input.cached = trial % 17 != 0;
    const auto snapshot = std::const_pointer_cast<IndexBoundsSnapshot>(work.index_bounds);
    if (trial % 7 == 0) snapshot->stamp.pages[0].second = 2;
    if (trial % 53 == 0) snapshot->stamp.pages.clear();
    if (trial % 59 == 0) snapshot->bounds = {UINT32_MAX - 1, UINT32_MAX};
    if (trial % 61 == 0) snapshot->bounds = {10, 4};
    if (trial % 67 == 0) work.index_window = {};
    if (trial % 71 == 0) work.index_bounds.reset();
    for (const bool resolve : {false, true}) {
      Compare(work, resolve);              // cold or cross-mode plan
      Compare(work, resolve);              // warm plan, same dynamic checks
      auto changed = work;
      changed.vertex_inputs[0].metadata.length = 1;
      changed.bindings.vertex_streams[1].offset = UINT32_MAX;
      changed.base_vertex = INT32_MIN;
      Compare(changed, resolve);           // warm plan cannot retain dynamic results
      g_fsr = !g_fsr;
      Compare(work, resolve);              // runtime FSR state changes upload selection
      g_fsr = !g_fsr;
    }
  }
  // A shader absent on the first attempt may appear with the same key later.
  std::vector<plume::RenderInputElement> elements{{"POSITION", 0, 0, 0, 12}, {"POSITION", 1, 1, 0, 8}};
  g_shader_hash = 999999; g_resolve_fail = g_float_variant = g_position_variant = g_fsr = false;
  Shader(1); auto work = Prototype(elements, 999999);
  g_shader_available = false; Compare(work, true);
  g_shader_available = true; Compare(work, true); Compare(work, true);
  // Captured secondary stream indexed to 254 of its 102 records; the whole-buffer fallback must not authorize the fetch.
  elements[1].alignedByteOffset = 24; elements[1].format = 4;
  work = Prototype(elements, 999998); Shader(3); g_shader_hash = 999998;
  work.bindings.vertex_streams[0].stride = 24; work.vertex_inputs[0].metadata.length = 8192;
  work.bindings.vertex_streams[1] = {1001, 396, 36};
  std::const_pointer_cast<IndexBoundsSnapshot>(work.index_bounds)->bounds = {0, 254};
  Compare(work, true); Compare(work, false);
  // A final record ending before its stride padding stays valid and takes a whole-buffer upload when the window cannot fit.
  elements.resize(1); work = Prototype(elements, 999997); Shader(1); g_shader_hash = 999997;
  work.bindings.vertex_streams[0].stride = 24; work.vertex_inputs[0].metadata.length = 60;
  work.base_vertex = -3;
  std::const_pointer_cast<IndexBoundsSnapshot>(work.index_bounds)->bounds = {3, 5};
  Compare(work, true); Compare(work, false);
  work.bindings.vertex_streams[0].stride = 0; Compare(work, true); Compare(work, false);
  CHECK(g_accepted > 100 && g_stamp_checks > 100 && g_cached_resolves > 100);
  std::printf("PASS %u production planner differential cases: results, work fields, layout, ordered dynamic calls and arguments\n", g_case);
  std::printf("COVERAGE accepted=%u rejected=%u index_stamp_checks=%u window_resolves=%u cached_resolves=%u\n",
      g_accepted, g_case - g_accepted, g_stamp_checks, g_window_resolves, g_cached_resolves);
}

void LiveControl() {
  const char* path = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_FILE");
  CHECK(path && *path);
  auto write = [&](const char* text) {
    { std::ofstream output(path); output << text; }
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    return PollVertexFetchPlan();
  };
  VertexFetchPlanEnabledState().store(true);
  CHECK(!write("2")); CHECK(VertexFetchPlanEnabled());
  CHECK(!write("")); CHECK(VertexFetchPlanEnabled());
  CHECK(!write("1 0")); CHECK(VertexFetchPlanEnabled());
  const auto off = write(" 0 \n"); CHECK(off && !*off && !VertexFetchPlanEnabled());
  CHECK(!write("0"));
  std::vector<plume::RenderInputElement> elements{{"POSITION", 0, 0, 0, 12}};
  auto work = Prototype(elements, 888888); g_shader_hash = 888888; Shader(1);
  for (const bool enabled : {false, true, false, true}) {
    const auto before = VertexFetchPlanEnabled();
    const auto acknowledged = write(enabled ? "1" : "0");
    CHECK(VertexFetchPlanEnabled() == enabled);
    CHECK(bool(acknowledged) == (before != enabled));
    Compare(work, true);
  }
  std::puts("PASS live control: strict parsing, whitespace, duplicate suppression, OFF/ON/OFF/ON planner semantics");
}

void LiveCacheControl() {
  const char* path = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU_FILE");
  CHECK(path && *path);
  auto write = [&](const char* text) {
    { std::ofstream output(path); output << text; }
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    return PollVertexFetchPlanCacheLru();
  };
  VertexFetchPlanCacheLruState().store(true);
  CHECK(!write("2")); CHECK(VertexFetchPlanCacheLruEnabled());
  CHECK(!write("")); CHECK(VertexFetchPlanCacheLruEnabled());
  CHECK(!write("1 0")); CHECK(VertexFetchPlanCacheLruEnabled());
  const auto off = write(" 0 \n"); CHECK(off && !*off && !VertexFetchPlanCacheLruEnabled());
  CHECK(!write("0"));
  VertexFetchPlanCache cache(1);
  const VertexFetchPlanCache::Key key{77, 78, 0, 0, 0, 0, 0, 1, true};
  const auto plan = cache.Resolve(key, [] { return std::make_shared<VertexFetchPlan>(); });
  for (const bool enabled : {false, true, false, true}) {
    const auto before = VertexFetchPlanCacheLruEnabled();
    const auto acknowledged = write(enabled ? "1" : "0");
    CHECK(VertexFetchPlanCacheLruEnabled() == enabled);
    CHECK(bool(acknowledged) == (before != enabled));
    CHECK(cache.Resolve(key, [] { Fail("policy toggle rebuilt a retained plan", __LINE__); return std::make_shared<VertexFetchPlan>(); }) == plan);
  }
  std::puts("PASS live cache control: strict parsing, whitespace, duplicate suppression, OFF/ON/OFF/ON with retained plan");
}

int main() {
  const char* initial = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN");
  CHECK(VertexFetchPlanEnabled() == (!initial || std::strcmp(initial, "0") != 0));
  const char* cache_initial = std::getenv("LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU");
  CHECK(VertexFetchPlanCacheLruEnabled() == (!cache_initial || std::strcmp(cache_initial, "0") != 0));
  VertexFetchPlanEnabledState().store(true);
  PureRangeDifferential(); CacheAndLifetime(); CacheEvictionDifferential();
  ProductionDifferential(); LiveControl(); LiveCacheControl();
}
