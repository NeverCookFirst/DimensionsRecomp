#include "gpu_native/texture_writeback_merge.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#ifdef REFERENCE_MERGE
#include "reference-merge.inc"
#endif
using namespace legodimensions::gpu_native;
int main() {
  std::mt19937 random(0x9d1547);
  uint32_t cases = 0;
  for (uint32_t size = 0; size <= 257; ++size)
    for (uint32_t offset = 0; offset < 32; ++offset)
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        std::vector<uint8_t> snapshot(size + offset + 32, 0xa9), gpu = snapshot;
        for (uint32_t i = 0; i < size; ++i) {
          snapshot[offset+i] = uint8_t(random());
          gpu[offset+i] = uint8_t(random());
        }
        auto destination = snapshot;
        for (uint32_t i = 0; i < size; ++i) {
          if (pattern == 1 || (pattern == 2 && i % 3 == 0)) destination[offset+i] ^= 0x80;
          if (pattern == 3) destination[offset+i] = gpu[offset+i];
        }
        auto expected = destination;
        bool modified = false;
        for (uint32_t i = 0; i < size; ++i) {
          if (destination[offset+i] == snapshot[offset+i]) expected[offset+i] = gpu[offset+i];
          else modified = true;
        }
#ifdef REFERENCE_MERGE
        auto reference = destination;
        const bool reference_modified = MergeMemexportBytes(reference.data()+offset,
            gpu.data()+offset, snapshot.data()+offset, size);
        assert(reference == expected && reference_modified == modified);
#endif
        const auto result = MergeTextureWriteback(
            std::span<uint8_t>(destination).subspan(offset,size),
            std::span<const uint8_t>(gpu).subspan(offset,size),
            std::span<const uint8_t>(snapshot).subspan(offset,size));
        assert(result.valid && result.cpu_modified == modified && destination == expected);
        ++cases;
      }
  std::vector<uint8_t> destination(32, 1), gpu(31, 2), snapshot(32, 1);
  assert(!MergeTextureWriteback(destination,gpu,snapshot).valid);
  assert(destination == snapshot);
  std::printf("PASS %u byte-merge cases: all alignments, SIMD tails, later CPU bytes, padding, invalid spans%s\n",
      cases,
#ifdef REFERENCE_MERGE
      "; exact SDK reference differential"
#else
      ""
#endif
  );
}
