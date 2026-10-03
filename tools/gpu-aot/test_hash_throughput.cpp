// Compare the installed SDK library with the same xxHash header implementation.
#include <xxhash.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

static uint64_t LibraryHash(const void* p, size_t n) { return XXH3_64bits(p, n); }
#define XXH_INLINE_ALL
#include <xxhash.h>

int main() {
  std::vector<uint8_t> data(16 * 1024 * 1024);
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 31);
  uint64_t result = 0;
  for (bool inlined : {false, true}) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 32; ++i) {
      data[0] = uint8_t(i);
      const auto a = inlined ? XXH3_64bits(data.data(), data.size())
                             : LibraryHash(data.data(), data.size());
      result ^= a;
    }
    std::cout << (inlined ? "header" : "SDK library") << ": "
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start).count()
              << " ms for 512 MiB, checksum=" << result << '\n';
  }
  // The checksum cancels when all results match.
  return result != 0;
}
