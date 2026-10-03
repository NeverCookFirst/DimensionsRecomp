// Differential oracle is the actual generated TU23 83FB58A8 body, extracted
// by extract_sampler_oracle.py. No hand-written copy of its merge algorithm.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "gpu_native/sampler_state.h"

using namespace legodimensions::gpu_native;

union OracleRegister {
  uint64_t u64 = 0;
  int64_t s64;
  uint32_t u32;
  int32_t s32;
};
struct OracleCondition {
  bool eq = false, gt = false, lt = false;
  template<typename T> void compare(T a, T b, int) { eq = a == b; gt = a > b; lt = a < b; }
};
struct OracleContext {
  OracleRegister r1,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r20,r21,r22,r23,r24,r25,r26,r27,r28,r29,r30,r31;
  uint64_t lr = 0;
  OracleCondition cr0, cr6;
  int xer = 0;
};
uint32_t Read32(const uint8_t* base, uint32_t at) {
  assert(at + 4ull <= 0x40000); uint32_t value;
  std::memcpy(&value, base + at, 4); return __builtin_bswap32(value);
}
void Write32(uint8_t* base, uint32_t at, uint32_t value) {
  assert(at + 4ull <= 0x40000); value = __builtin_bswap32(value);
  std::memcpy(base + at, &value, 4);
}
uint64_t Read64(const uint8_t* base, uint32_t at) {
  return uint64_t(Read32(base, at)) << 32 | Read32(base, at + 4);
}
void Write64(uint8_t* base, uint32_t at, uint64_t value) {
  Write32(base, at, uint32_t(value >> 32)); Write32(base, at + 4, uint32_t(value));
}
#define DEFINE_REX_FUNC(name) void name(OracleContext& ctx, uint8_t* base)
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U32(x) Read32(base, uint32_t(x))
#define REX_STORE_U32(x,v) Write32(base, uint32_t(x), uint32_t(v))
#define REX_LOAD_U64(x) Read64(base, uint32_t(x))
#define REX_STORE_U64(x,v) Write64(base, uint32_t(x), uint64_t(v))
#define REX_LOAD_U8(x) base[uint32_t(x)]
// Save/restore only affects registers outside the compared device bytes.
void __savegprlr_20(OracleContext&, uint8_t*) {}
void __restgprlr_20(OracleContext&, uint8_t*) {}
void sub_83FC7048(OracleContext&, uint8_t*) { assert(false && "Xbox queue path entered"); }
#include "tu23_set_texture_oracle.inc"

int main() {
  std::vector<uint8_t> memory(0x40000);
  std::mt19937 random(0x83FB58A8);
  constexpr uint32_t device = 0x10000, texture = 0x20000;
  for (uint32_t n = 0; n < 100000; ++n) {
    const uint32_t slot = n % 26;
    const uint32_t address = device + 1152 + 24 * slot;
    TextureFetchWords before, header;
    for (uint32_t w = 0; w < 6; ++w) {
      before[w] = random(); header[w] = random();
      Write32(memory.data(), address + 4*w, before[w]);
      Write32(memory.data(), texture + 28 + 4*w, header[w]);
    }
    const uint8_t min = uint8_t(random()), max = uint8_t(random());
    memory[device + 12356 + slot] = min; memory[device + 12382 + slot] = max;
    Write32(memory.data(), device + 12920 + 4*slot, 0);
    const uint64_t dirty_before = uint64_t(random()) << 32 | random();
    Write64(memory.data(), device + 24, dirty_before);
    OracleContext ctx;
    ctx.r1.u32 = 0x3F000; ctx.r3.u32 = device; ctx.r4.u32 = slot;
    ctx.r5.u32 = n & 1 ? texture : 0; ctx.r6.u64 = uint64_t(1) << (31 - slot);
    auto expected = before;
    if (ctx.r5.u32) expected = MergeTextureFetch(before, header, min, max);
    else ClearTextureFetch(expected);
    const uint64_t expected_dirty = ctx.r5.u32 ? dirty_before | ctx.r6.u64 : dirty_before;
    sub_83FB58A8(ctx, memory.data());
    for (uint32_t w = 0; w < 6; ++w) assert(Read32(memory.data(), address + 4*w) == expected[w]);
    assert(Read64(memory.data(), device + 24) == expected_dirty);
    assert(Read32(memory.data(), device + 12920 + 4*slot) == (n & 1 ? texture : 0));
  }

  // Known Xenos modes and edge cases beyond a round-trip of our own bitfields.
  TextureFetchWords f{};
  f[0] = 2 | (1 << 10) | (6 << 13) | (3 << 16);
  f[1] = 0x1000; f[2] = 255 | (127 << 13); f[5] = 0x2000 | (1 << 9) | 1;
  f[3] = (1 << 19) | (0 << 21) | (1 << 23);
  f[4] = (2 << 2) | (7 << 6) | (uint32_t(-48) & 1023) << 12;
  auto d = DecodeTextureSampler(f);
  assert(d.addressU == plume::RenderTextureAddressMode::MIRROR);
  assert(d.addressV == plume::RenderTextureAddressMode::BORDER);
  assert(d.addressW == plume::RenderTextureAddressMode::CLAMP);
  assert(d.minFilter == plume::RenderFilter::NEAREST);
  assert(d.magFilter == plume::RenderFilter::LINEAR);
  assert(d.minLOD == 2 && d.maxLOD == 7 && d.mipLODBias == -1.5f);
  assert(d.borderColor == plume::RenderBorderColor::OPAQUE_WHITE);
  const auto key = TextureSamplerKey(d);
  // Moving an identical texture must not allocate a new sampler descriptor.
  f[1] += 0x100000; f[5] += 0x100000;
  assert(TextureSamplerKey(DecodeTextureSampler(f)) == key);
  f[3] |= 5 << 25;
  d = DecodeTextureSampler(f);
  assert(d.anisotropyEnabled && d.maxAnisotropy == 16);
  assert(d.minFilter == plume::RenderFilter::LINEAR);
  f[5] = (3 << 9); f[4] |= 15 << 6;
  d = DecodeTextureSampler(f);
  assert(d.addressU == plume::RenderTextureAddressMode::CLAMP);
  assert(d.addressV == plume::RenderTextureAddressMode::CLAMP);
  assert(d.addressW == plume::RenderTextureAddressMode::CLAMP);
  assert(d.minLOD == 0 && d.maxLOD == 0); // no mip page
  f[3] = 2 << 23;
  d = DecodeTextureSampler(f);
  assert(d.maxLOD == 0.25f && d.mipmapMode == plume::RenderMipmapMode::NEAREST);
  std::puts("TU23 SetTexture differential: 100000 cases passed; sampler modes passed");
}
