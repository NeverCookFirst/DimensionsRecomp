#include "gpu_native/index_bounds.h"
#include <cassert>
#include <random>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace legodimensions::gpu_native;
int main() {
  std::mt19937 random(0x108060);
  for (unsigned width : {2u, 4u}) {
    for (unsigned count = 0; count < 1025; ++count) {
      // Exercise every unaligned pointer phase and scalar/vector tail.
      for (unsigned phase = 0; phase < 32; ++phase) {
        std::vector<uint8_t> data(phase + count * width + 16, 0xFF);
        IndexBounds expected;
        for (unsigned i = 0; i < count; ++i) {
          uint32_t value = random();
          if (width == 2) value &= 65535;
          if (i == 1) value = 0;
          if (i == 2) value = width == 2 ? 65535 : UINT32_MAX;
          expected.min = std::min(expected.min, value);
          expected.max = std::max(expected.max, value);
          for (unsigned j = 0; j < width; ++j)
            data[phase + i * width + j] = uint8_t(value >> ((width - 1 - j) * 8));
        }
        auto range = std::span<const uint8_t>(data.data() + phase, count * width);
        auto result = ScanGuestIndexBounds(range, width);
        assert(result.min == expected.min && result.max == expected.max);
        if (count) {
          // No cached range can hide a direct write between two draws.
          std::fill_n(data.data() + phase, count * width, 0);
          result = ScanGuestIndexBounds(range, width);
          assert(result.min == 0 && result.max == 0);
        }
      }
    }
  }
  const uint8_t malformed[] = {0, 1, 2};
  assert(ScanGuestIndexBounds(malformed, 2).min == UINT32_MAX);
  assert(ScanGuestIndexBounds(malformed, 3).min == UINT32_MAX);
  assert(ScanGuestIndexBounds(malformed, 0).min == UINT32_MAX);
  for (unsigned width : {2u, 4u}) {
    for (unsigned position = 0; position < 65; ++position) {
      std::vector<uint8_t> data(65 * width, 0x7F);
      std::fill_n(data.data() + position * width, width, 0);
      auto result = ScanGuestIndexBounds(data, width);
      assert(result.min == 0);
      assert(result.max == (width == 2 ? 0x7F7Fu : 0x7F7F7F7Fu));
      std::fill_n(data.data() + position * width, width, 0xFF);
      result = ScanGuestIndexBounds(data, width);
      assert(result.min == (width == 2 ? 0x7F7Fu : 0x7F7F7F7Fu));
      assert(result.max == (width == 2 ? 0xFFFFu : UINT32_MAX));
    }
  }
#ifdef _WIN32
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * info.dwPageSize,
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  assert(pages);
  DWORD previous = 0;
  assert(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous));
  for (unsigned width : {2u, 4u}) {
    for (unsigned count = 1; count <= 65; ++count) {
      auto* first = pages + info.dwPageSize - count * width;
      std::fill_n(first, count * width, 0x7F);
      const auto result = ScanGuestIndexBounds({first, count * width}, width);
      const auto value = width == 2 ? 0x7F7Fu : 0x7F7F7F7Fu;
      assert(result.min == value && result.max == value);
    }
  }
  assert(VirtualFree(pages, 0, MEM_RELEASE));
#endif
}
