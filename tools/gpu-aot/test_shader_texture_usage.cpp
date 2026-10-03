#include <array>
#include <cassert>
#include <vector>
#include "../../rexlego/src/gpu_native/shader_texture_usage.h"
using namespace legodimensions::gpu_native;
void write(std::vector<uint8_t>& code, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) code[at + i] = uint8_t(value >> (24 - i * 8));
}
void cf(std::vector<uint8_t>& code, size_t pair, uint32_t a, uint32_t ah,
        uint32_t b = 0, uint32_t bh = 0) {
  write(code, pair * 12, a);
  write(code, pair * 12 + 4, ah | (b << 16));
  write(code, pair * 12 + 8, (b >> 16) | (bh << 16));
}
int main() {
  constexpr uint32_t all = ~uint32_t{0};
  assert(ShaderTextureUsage({}) == all);
  std::vector<uint8_t> code(60);
  // A conditional END must not hide the second exec clause. Include slot 31.
  cf(code, 0, 2 | (1 << 12) | (1 << 16), 4 << 12,
              3 | (2 << 12) | (5 << 16), 2 << 12);
  write(code, 24, 1 | (7 << 20));
  write(code, 36, 19 | (31u << 20));
  write(code, 48, 0); // vertex fetch does not consume a texture slot
  assert(ShaderTextureUsage(code) == ((1u << 7) | (1u << 31)));
  write(code, 24, 3); // unknown fetch opcode: conservative fallback
  assert(ShaderTextureUsage(code) == all);
  cf(code, 0, 9 | (1 << 12) | (1 << 16), 1 << 12);
  assert(ShaderTextureUsage(code) == all);
  code.resize(59);
  assert(ShaderTextureUsage(code) == all);
  code.assign(36, 0);
  cf(code, 0, 2 | (1 << 12), 2 << 12); // ALU-only program
  assert(ShaderTextureUsage(code) == 0);
}
