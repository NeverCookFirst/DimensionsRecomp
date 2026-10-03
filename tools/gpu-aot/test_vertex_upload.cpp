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
    std::vector<uint32_t> fields;
    for (unsigned word = 0; word < stride / 4; ++word) if (rng() % 4 == 0) fields.push_back(word * 4);
    if (fields.empty()) fields.push_back(0);
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
  std::cout << "PASS 30000 differential vertex uploads, misaligned pointers, partial records/tails, bounded writes and untouched fallback\n";
}
