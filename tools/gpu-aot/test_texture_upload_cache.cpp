#define XXH_INLINE_ALL
#include "../../rexlego/src/gpu_native/texture_upload_cache.h"

#include <array>
#include <cassert>
#include <iostream>
#include <cstdio>
#include <cstdlib>

#undef assert
#define assert(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "check failed: %s\n", #condition); std::exit(23); } } while (0)

int main() {
  using namespace legodimensions::gpu_native;
  std::array<uint8_t, 6 * 256> base{};  // All cube faces, not only face zero.
  std::array<uint8_t, 512> mips{};
  std::array<uint8_t, 24> fetch{};
  const auto initial = MakeTextureUploadSourceKey(base, mips, fetch);
  assert(initial == MakeTextureUploadSourceKey(base, mips, fetch));
  for (size_t offset : {size_t{0}, size_t{256}, base.size() - 1}) {
    ++base[offset];
    assert(initial != MakeTextureUploadSourceKey(base, mips, fetch));
    --base[offset];
  }
  for (size_t offset : {size_t{0}, mips.size() - 1}) {
    ++mips[offset];
    assert(initial != MakeTextureUploadSourceKey(base, mips, fetch));
    --mips[offset];
  }
  ++fetch[20];  // Changing fetch semantics must invalidate identical bytes.
  assert(initial != MakeTextureUploadSourceKey(base, mips, fetch));
  --fetch[20];
  const auto other_allocation = base;
  assert(initial == MakeTextureUploadSourceKey(other_allocation, mips, fetch));
  const auto base_only = MakeTextureUploadSourceKey(base, {}, fetch);
  assert(initial != base_only);
  assert(base_only == MakeTextureUploadSourceKey(base, {}, fetch));
  assert(initial != MakeTextureUploadSourceKey(
      std::span<const uint8_t>(base).first(base.size() - 1), mips, fetch));
  std::array<uint32_t, 6> header{2, 0xF4000052u, 0x001FE0FFu,
                               0xD10u, 0x200u, 0xF4100A00u};
  const auto header_bytes = [&] {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(header.data()), sizeof(header));
  };
  const auto placed = MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes());
  header[1] += 0xC000;
  header[5] += 0xC000;
  assert(placed == MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes()));
  ++base.back();
  assert(placed != MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes()));
  --base.back();
  ++mips.back();
  assert(placed != MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes()));
  --mips.back();
  header[1] ^= 1;  // Format / endian semantics cannot be masked as addresses.
  assert(placed != MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes()));
  header[1] ^= 1;
  header[5] &= 0xFFF;  // Removing mip storage changes interpretation.
  assert(placed != MakeRelocatableTextureUploadSourceKey(base, mips, header_bytes()));
  std::cout << "Texture upload source-cache regressions passed\n";
}
