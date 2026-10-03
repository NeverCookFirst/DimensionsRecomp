#define XXH_INLINE_ALL
#include "../../rexlego/src/gpu_native/texture_upload_cache.h"

#include <array>
#include <cassert>
#include <iostream>

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
  std::cout << "Texture upload source-cache regressions passed\n";
}
