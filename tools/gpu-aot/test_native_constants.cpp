#include "legodimensions_pch.h"
#include "native_constants.h"
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

extern "C" REX_FUNC(sub_82B7ABF0);
extern "C" REX_FUNC(__imp__sub_82B7ABF0);
extern "C" REX_FUNC(__savegprlr_24);
extern "C" REX_FUNC(__restgprlr_24);
extern "C" REX_FUNC(sub_82BCDAD8);
extern "C" REX_FUNC(sub_83FB6480);
extern "C" REX_FUNC(sub_83FB6558);
extern "C" REX_FUNC(oracle_vertex_constants);
extern "C" REX_FUNC(oracle_pixel_constants);

#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) extern "C" REX_FUNC(name)
#include "native_constants_oracles.inc"

#undef assert
#define assert(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(23); } } while (0)

struct Call {
  unsigned id;
  PPCContext context;
  Call(unsigned kind, const PPCContext& source) : id(kind) {
    std::memcpy(&context, &source, sizeof(context));
  }
  Call(const Call& source) : Call(source.id, source.context) {}
  Call(Call&& source) noexcept : Call(source.id, source.context) {}
  Call& operator=(const Call& source) {
    id = source.id;
    std::memcpy(&context, &source.context, sizeof(context));
    return *this;
  }
  Call& operator=(Call&& source) noexcept { return *this = source; }
};
struct Region {
  u32 address;
  u32 bytes;
};
struct Fixture {
  u32 arena, renderer, device, program, alternate, parameters, metadata, manager, data, stack;
  unsigned behavior = 0, calls = 0;
  bool record = true;
  std::vector<Call> trace;
} fixture;
unsigned sequence_id = 0;

void Record(unsigned id, const PPCContext& ctx) {
  if (fixture.record) fixture.trace.push_back({id, ctx});
}

extern "C" REX_FUNC(sub_82BCDAD8) {
  Record(0, ctx);
  assert(ctx.r3.u32 == fixture.manager);
  REX_STORE_U32(fixture.manager + 8, REX_LOAD_U32(fixture.manager + 8) + 1);
  if (fixture.behavior == 1) REX_STORE_U32(fixture.renderer + 21180, fixture.alternate);
  ctx.r3.u64 = 0x8102030405060708ull;
  ctx.r4.u64 ^= 0x123456789ABCDEFull;
  ctx.r5.u64 = fixture.parameters;
  ctx.r6.u64 = 0xFFEEDDCCBBAA0099ull;
  ctx.r7.u64 = 0x1020304050607080ull;
  ctx.r8.u64 ^= ctx.r4.u64;
  ctx.r9.u64 = 0x9887766554433221ull;
  ctx.r10.u64 = 0x8877665544332211ull;
  ctx.r11.u64 = 0x7766554433221100ull;
  ctx.r12.u64 = ctx.lr;
  ctx.ctr.u64 = 7;
  ctx.xer.ca ^= 1;
  ctx.cr0.compare<i32>(-1, 1, ctx.xer);
  ctx.cr6.compare<u32>(ctx.r4.u32, ctx.r5.u32, ctx.xer);
}

void AfterConstants(PPCContext& ctx, u8* base) {
  if (++fixture.calls != 1) return;
  if (fixture.behavior == 2 || fixture.behavior == 3) {
    REX_STORE_U32(fixture.renderer + 21180, fixture.alternate);
    if (fixture.behavior == 3) REX_STORE_U8(fixture.alternate + 139, ctx.r26.u8 + 1);
  }
  if (fixture.behavior == 4) {
    const u32 replacement = fixture.parameters + 32 * 16;
    if (ctx.r29.u32 == 21236) {
      REX_STORE_U32(replacement, replacement + 8);
      REX_STORE_U32(replacement + 8, fixture.data);
    }
    REX_STORE_U32(fixture.renderer + ctx.r29.u32, replacement);
  }
}

extern "C" REX_FUNC(sub_83FB6480) {
  Record(1, ctx);
  oracle_vertex_constants(ctx, base);
  AfterConstants(ctx, base);
}

extern "C" REX_FUNC(sub_83FB6558) {
  Record(2, ctx);
  oracle_pixel_constants(ctx, base);
  AfterConstants(ctx, base);
}

std::vector<Region> Regions(u32 arena) {
  return {{arena, 0x81000}, {0x847C0000, 0x10000}, {0x84950000, 0x20000}};
}

std::vector<std::vector<u8>> Snapshot(u8* base, const std::vector<Region>& regions) {
  std::vector<std::vector<u8>> result;
  for (const auto& region : regions)
    result.emplace_back(base + region.address, base + u64(region.address) + region.bytes);
  return result;
}

void Restore(u8* base, const std::vector<Region>& regions, const std::vector<std::vector<u8>>& bytes) {
  for (size_t i = 0; i < regions.size(); ++i)
    std::memcpy(base + regions[i].address, bytes[i].data(), bytes[i].size());
}

void EqualContext(const PPCContext& a, const PPCContext& b, const char* place) {
  const auto* x = reinterpret_cast<const u8*>(&a);
  const auto* y = reinterpret_cast<const u8*>(&b);
  for (size_t i = 0; i < sizeof(PPCContext); ++i) if (x[i] != y[i]) {
    std::fprintf(stderr, "case %u %s context byte %zu: original=%02X native=%02X\n",
        sequence_id, place, i, x[i], y[i]);
    std::exit(23);
  }
}

PPCContext Prepare(u8* base, u32 arena, unsigned selected, unsigned behavior, std::mt19937& rng) {
  fixture = {arena, arena, arena + 0x10000, arena + 0x18000, arena + 0x19000,
      arena + 0x20000, arena + 0x22000, arena + 0x24000, arena + 0x30000,
      arena + 0x72000, behavior};
  for (const auto& region : Regions(arena)) std::memset(base + region.address, 0xA5, region.bytes);
  REX_STORE_U32(0x849677B8, fixture.manager);
  REX_STORE_U32(0x84950478, fixture.device);
  REX_STORE_U32(fixture.manager + 8, rng());
  REX_STORE_U32(fixture.manager + 1036, sequence_id % 33);
  REX_STORE_U32(fixture.renderer + 21180, fixture.program);
  REX_STORE_U8(fixture.program + 139, selected);
  REX_STORE_U8(fixture.alternate + 139, 99);
  for (unsigned i = 0; i < 99; ++i) {
    REX_STORE_U8(fixture.program + 40 + i, (i + sequence_id) % 32);
    REX_STORE_U8(fixture.alternate + 40 + i, (99 - i + sequence_id) % 32);
  }
  for (unsigned i = 0; i < 32; ++i) {
    const u32 parameter = fixture.parameters + i * 16;
    const u32 metadata = fixture.metadata + i * 64;
    const u32 stage = rng() & 1;
    const u32 count = i == 12 ? (sequence_id % 33) * 3 : rng() % 97;
    const u32 first = (i == 0 && sequence_id % 17 == 0) ? 255 : rng() % (257 - count);
    const u32 bounded_count = std::min(count, 256 - first);
    u32 data = fixture.data + i * 4096 + rng() % 16;
    if (behavior == 5 && i != 12) data = fixture.device + 1920 + (rng() % 128) * 16;
    REX_STORE_U32(0x847CCA90 + i * 4, metadata);
    REX_STORE_U32(metadata + 20, stage);
    REX_STORE_U32(metadata + 24, bounded_count * 16 + (rng() & 15));
    REX_STORE_U32(metadata + 32, first);
    const u32 pointer = i == 12 ? parameter + 8 : data;
    REX_STORE_U32(parameter, pointer);
    REX_STORE_U32(parameter + 8, data);
    REX_STORE_U8(parameter + 4, (rng() % 3) ? 0 : 0x80);
    REX_STORE_U32(fixture.renderer + 21188 + i * 4, rng() % 7 ? parameter : 0);
    REX_STORE_U32(fixture.renderer + 21348 + (stage * 256 + first) * 4,
        rng() % 2 ? pointer : pointer + 16);
    if (behavior != 5 || i == 12) {
      auto* source = REX_RAW_ADDR(data);
      for (unsigned n = 0; n < 1600; ++n) source[n] = u8(rng());
    }
  }
  const u32 replacement = fixture.parameters + 32 * 16;
  REX_STORE_U32(replacement, fixture.data);
  REX_STORE_U8(replacement + 4, 0x80);
  PPCContext ctx{};
  auto* bytes = reinterpret_cast<u8*>(&ctx);
  for (size_t i = 0; i < sizeof(ctx); ++i) bytes[i] = u8(rng());
  ctx.r1.u64 = fixture.stack;
  ctx.r3.u64 = (u64(rng()) << 32) | fixture.renderer;
  ctx.lr = 0x82ABCDEF;
  ctx.xer = {u8(rng() & 1), u8(rng() & 1), u8(rng() & 1)};
  return ctx;
}

void Differential(u8* base, PPCContext initial, unsigned repetitions) {
  const auto regions = Regions(fixture.arena);
  const auto before = Snapshot(base, regions);
  PPCContext expected;
  std::memcpy(&expected, &initial, sizeof(expected));
  fixture.calls = 0; fixture.trace.clear();
  for (unsigned i = 0; i < repetitions; ++i) {
    expected.r3.u64 = initial.r3.u64;
    __imp__sub_82B7ABF0(expected, base);
  }
  const auto expected_memory = Snapshot(base, regions);
  const auto expected_calls = fixture.trace;
  Restore(base, regions, before);
  PPCContext actual;
  std::memcpy(&actual, &initial, sizeof(actual));
  fixture.calls = 0; fixture.trace.clear();
  for (unsigned i = 0; i < repetitions; ++i) {
    actual.r3.u64 = initial.r3.u64;
    sub_82B7ABF0(actual, base);
  }
  EqualContext(expected, actual, "return");
  assert(expected_calls.size() == fixture.trace.size());
  for (size_t i = 0; i < expected_calls.size(); ++i) {
    assert(expected_calls[i].id == fixture.trace[i].id);
    EqualContext(expected_calls[i].context, fixture.trace[i].context, "call");
  }
  for (size_t i = 0; i < regions.size(); ++i) {
    const auto* actual_bytes = base + regions[i].address;
    for (size_t n = 0; n < expected_memory[i].size(); ++n) if (expected_memory[i][n] != actual_bytes[n]) {
      std::fprintf(stderr, "case %u memory %08llX: original=%02X native=%02X\n", sequence_id,
          static_cast<unsigned long long>(regions[i].address) + n, expected_memory[i][n], actual_bytes[n]);
      std::exit(23);
    }
  }
}

void Benchmark(u8* base, std::mt19937& rng) {
  ++sequence_id;
  auto ctx = Prepare(base, 0x100000, 24, 0, rng);
  fixture.record = false;
  ctx.r3.u64 = fixture.renderer;
  __imp__sub_82B7ABF0(ctx, base);
  constexpr unsigned calls = 100000;
  std::array<std::vector<double>, 2> samples;
  for (unsigned round = 0; round < 6; ++round) for (unsigned order = 0; order < 2; ++order) {
    const unsigned native = order ^ (round & 1);
    auto* fn = native ? sub_82B7ABF0 : __imp__sub_82B7ABF0;
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned n = 0; n < calls; ++n) { ctx.r3.u64 = fixture.renderer; fn(ctx, base); }
    samples[native].push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / calls);
  }
  for (auto& values : samples) std::sort(values.begin(), values.end());
  const double legacy = (samples[0][2] + samples[0][3]) * .5;
  const double native = (samples[1][2] + samples[1][3]) * .5;
  std::printf("BENCH clean_24_blocks original_ns=%.3f hook_ns=%.3f ratio=%.6f calls_per_variant=%u\n",
      legacy, native, native / legacy, calls * 6);
}

void LiveControl(u8* base, std::mt19937& rng) {
  using legodimensions::NativeConstantPublisherEnabled;
  using legodimensions::PollNativeConstantPublisher;
  const char* path = std::getenv("LEGO_NATIVE_CONSTANT_PUBLISHER_FILE");
  assert(path && *path);
  const auto write = [path](const char* value) {
    std::ofstream file(path, std::ios::trunc);
    assert(file);
    file << value;
    file.close();
    assert(file);
  };
  const auto poll = [] { Sleep(510); return PollNativeConstantPublisher(); };
  const auto compare = [&] {
    ++sequence_id;
    auto ctx = Prepare(base, 0xE0100000, 31, 4, rng);
    Differential(base, ctx, 2);
  };
  const bool initial = NativeConstantPublisherEnabled();
  write("invalid");
  assert(!PollNativeConstantPublisher() && NativeConstantPublisherEnabled() == initial);
  write("0");
  auto result = poll();
  assert(result && !*result && !NativeConstantPublisherEnabled());
  compare();
  write("1");
  result = poll();
  assert(result && *result && NativeConstantPublisherEnabled());
  compare();
  write("1");
  assert(!poll() && NativeConstantPublisherEnabled());
  write(" 0\r\n ");
  result = poll();
  assert(result && !*result && !NativeConstantPublisherEnabled());
  compare();
  write("0 1");
  assert(!poll() && !NativeConstantPublisherEnabled());
  write("");
  assert(!poll() && !NativeConstantPublisherEnabled());
  write("1");
  result = poll();
  assert(result && *result && NativeConstantPublisherEnabled());
  compare();
  std::puts("PASS live control: invalid/empty ignored, whitespace accepted, duplicate suppressed, OFF/ON/OFF/ON preserved semantics");
}

int main(int argc, char** argv) {
  SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
  auto* base = static_cast<u8*>(VirtualAlloc(nullptr, 0x100002000ull, MEM_RESERVE, PAGE_READWRITE));
  assert(base);
  const u32 arenas[] = {0x100000, 0xDFF00000, 0xE0100000, 0xFFF00000};
  for (u32 arena : arenas) assert(VirtualAlloc(base + arena, 0x81000, MEM_COMMIT, PAGE_READWRITE));
  assert(VirtualAlloc(base + 0x847C0000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
  assert(VirtualAlloc(base + 0x84950000, 0x20000, MEM_COMMIT, PAGE_READWRITE));
  std::mt19937 rng(0xB7ABF0);
  unsigned comparisons = 0;
  for (u32 arena : arenas) for (unsigned n = 0; n < 600; ++n) {
    ++sequence_id;
    const unsigned selected = n < 100 ? n : rng() % 100;
    auto ctx = Prepare(base, arena, selected, n % 6, rng);
    Differential(base, ctx, 1 + (n % 7 == 0));
    ++comparisons;
  }
  std::printf("PASS %u differential cases: full PPC context, call ordering/arguments, guest memory, four address ranges\n", comparisons);
  if (argc > 1 && !std::strcmp(argv[1], "--benchmark")) Benchmark(base, rng);
  LiveControl(base, rng);
  VirtualFree(base, 0, MEM_RELEASE);
}
