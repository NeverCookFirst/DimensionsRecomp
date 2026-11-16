#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include "gpu_native/vertex_upload.h"
using namespace legodimensions::gpu_native;
void Reference(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src,
               const VertexByteOrder& order) {
  size_t offset = 0;
  for (; offset + 4 <= src.size(); offset += 4)
    for (size_t byte = 0; byte < 4; ++byte) dst[offset + byte] = src[offset + 3 - byte];
  for (; offset < src.size(); ++offset) dst[offset] = src[offset];
  assert(ApplyVertexByteOrder(dst.data(), dst.size(), order));
}
int main() {
  std::mt19937 rng(337);
  for (unsigned n = 0; n < 30000; ++n) {
    const unsigned stride = 4 * (1 + rng() % 64), offset = 4 * (rng() % 128);
    const unsigned length = offset + rng() % 4096;
    std::vector<uint32_t> fields, halves;
    for (unsigned word = 0; word < stride / 4; ++word) {
      const unsigned pick = rng() % 6;
      if (pick == 0) fields.push_back(word * 4);
      else if (pick == 1) halves.push_back(word * 4 | kHalfSwapField);
    }
    if (fields.empty() && halves.empty()) fields.push_back(0);
    fields.insert(fields.end(), halves.begin(), halves.end());  // flagged fields sort last
    const VertexByteOrder order{stride, offset, fields};
    std::vector<uint8_t> src(length + 32), expected(length + 32, 0xCC), actual(length + 32, 0xCC);
    for (auto& byte : src) byte = uint8_t(rng());
    std::vector<uint8_t> bounded(src.begin() + 7, src.begin() + 7 + length), reference(length);
    Reference(reference, bounded, order);
    std::copy(reference.begin(), reference.end(), expected.begin() + 3);
    assert(WriteAlignedVertexUpload(actual.data() + 3, src.data() + 7, length, order));
    assert(actual == expected); // Exact bytes and no writes beyond the window.
  }
  std::vector<uint8_t> source(1024, 73), output(1024, 0xCC);
  const std::vector<uint32_t> duplicate{0, 0}, unordered{4, 0}, unaligned{1}, outside{16}, good{0, 4};
  for (auto fields : {duplicate, unordered, unaligned, outside})
    assert(!WriteAlignedVertexUpload(output.data(), source.data(), source.size(), {16, 0, fields}));
  assert(!WriteAlignedVertexUpload(output.data(), source.data(), source.size(), {18, 0, good}));
  assert(!WriteAlignedVertexUpload(output.data(), source.data(), source.size(), {16, 2, good}));
  assert(!WriteAlignedVertexUpload(output.data(), source.data(), source.size(), {16, 1028, good}));
  assert(!WriteAlignedVertexUpload(output.data(), source.data(), source.size(), {260, 0, good}));
  assert(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xCC; }));
  // Guest FLOAT16_4 bytes H0hi H0lo H1hi H1lo H2.. arrive as little-endian H0,H1,H2,H3.
  {
    const std::vector<uint8_t> guest{0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44, 0x00};
    std::vector<uint8_t> host(8);
    const std::vector<uint32_t> half_fields{0 | kHalfSwapField, 4 | kHalfSwapField};
    assert(WriteAlignedVertexUpload(host.data(), guest.data(), 8, {8, 0, half_fields}));
    const std::vector<uint8_t> want{0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44};
    assert(host == want);
    std::vector<uint8_t> slow(8);
    Reference(slow, guest, {8, 0, half_fields});
    assert(slow == want);
  }
  std::cout << "PASS 30000 differential vertex uploads, misaligned pointers, partial records/tails, bounded writes and untouched fallback\n";
}
