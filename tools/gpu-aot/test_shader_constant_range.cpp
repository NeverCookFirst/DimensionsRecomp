#include "gpu_native/shader_constant_range.h"
#include <cassert>
#include <vector>

using namespace legodimensions::gpu_native;

void Word(std::vector<uint8_t>& code, size_t at, uint32_t word) {
  for (size_t i = 0; i < 4; ++i) code[at + i] = uint8_t(word >> (24 - i * 8));
}

int main() {
  assert(ShaderFloatConstantCount({}) == 256);
  std::vector<uint8_t> code(36);
  Word(code, 0, 1 | (2 << 12));
  Word(code, 4, 2 << 12);
  Word(code, 20, (73 << 16) | (1u << 30) | (1u << 29));
  Word(code, 32, (6 << 16) | (1u << 30) | (1u << 29));
  assert(ShaderFloatConstantCount(code) == 74);
  Word(code, 16, 1u << 31);
  assert(ShaderFloatConstantCount(code) == 256);
  Word(code, 16, 1u << 30);
  assert(ShaderFloatConstantCount(code) == 256);
  Word(code, 16, 0);
  Word(code, 12, 42u << 26);
  Word(code, 20, 191u | (7u << 29));
  assert(ShaderFloatConstantCount(code) == 192);
  Word(code, 12, 0);
  Word(code, 20, 7u << 29);
  Word(code, 32, 7u << 29);
  assert(ShaderFloatConstantCount(code) == 0);
  Word(code, 0, 1 | (7 << 12));
  assert(ShaderFloatConstantCount(code) == 256);
  Word(code, 0, 3 | (1 << 12));
  assert(ShaderFloatConstantCount(code) == 256);
  std::vector<uint8_t> header(44);
  Word(header, 24, 36);
  Word(header, 36, 0);
  Word(header, 40, 36);
  assert(ShaderInstructionBytes(header, code).size() == 36);
  Word(header, 36, 4);
  assert(ShaderInstructionBytes(header, code).empty());
  Word(header, 24, UINT32_MAX);
  assert(ShaderInstructionBytes(header, code).empty());
}
