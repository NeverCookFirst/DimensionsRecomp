#include <cassert>
#include "gpu_native/pixel_shader_filter.h"
using namespace legodimensions::gpu_native;
int main() {
  assert(ParsePixelShaderFilter("").empty());
  auto hashes = ParsePixelShaderFilter(" CFEAC7ADB912F8A9, 0x12,0X12, abcd ,bad-hash,,FFFFFFFFFFFFFFFFF");
  assert(hashes.size() == 3 && hashes.contains(0xCFEAC7ADB912F8A9ull) &&
         hashes.contains(0x12) && hashes.contains(0xABCD));
  assert(ParsePixelShaderFilter("\t,0x,wat\n").empty());
}
