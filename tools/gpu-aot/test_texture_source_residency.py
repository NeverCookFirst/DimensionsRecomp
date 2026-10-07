"""Execute the production texture-upload residency guard with synthetic heaps."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root / "rexlego/src/gpu_native/textures.cpp").read_text()
    begin = text.index("    const bool source_key_valid =", text.index("bool UploadTextureResource("))
    end = text.index("    if (source_key_valid) {", begin)
    guard = text[begin:end]
    harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
using u32 = uint32_t;
bool base_readable = true, mip_readable = true;
u32 residency_queries = 0;
bool ReadableUploadSpan(u32 address, u32 size) {
  ++residency_queries;
  assert(size != 0);
  if (address == 0x1000) return base_readable;
  if (address == 0x2000) return mip_readable;
  return false;
}
bool LongProbeEnabled() { return false; }
bool LongProbeOnce(uint64_t) { return false; }
template <class... Args> void LongProbeEvent(Args&&...) {}
bool SourceResident(bool volume, bool alpha4, bool capture_pending,
                    u32 mip_address, u32 mip_size) {
  struct { u32 base_address, mip_address; } fetch{1, mip_address};
  struct { struct { u32 level_data_extent_bytes = 64; } base; } layout;
  const u32 guest_address = 0x3000;
'''
    harness += guard + r'''
  return true;
}
int main() {
  // 2D/cube uploads have no special-path flags. These must reject unreadable
  // memory even when capture is off, just like volumes and DXT3A.
  for (auto flags : std::array<std::array<bool, 3>, 4>{{
           {false, false, false}, {true, false, false},
           {false, true, false}, {false, false, true}}}) {
    base_readable = false;
    mip_readable = true;
    residency_queries = 0;
    assert(!SourceResident(flags[0], flags[1], flags[2], 2, 64));
    assert(residency_queries == 1);
    base_readable = true;
    mip_readable = false;
    assert(!SourceResident(flags[0], flags[1], flags[2], 2, 64));
    assert(!SourceResident(flags[0], flags[1], flags[2], 0, 64));
    // Base-only uploads do not require mip memory.
    residency_queries = 0;
    assert(SourceResident(flags[0], flags[1], flags[2], 0, 0));
    assert(residency_queries == 1);
    mip_readable = true;
    residency_queries = 0;
    assert(SourceResident(flags[0], flags[1], flags[2], 2, 64));
    assert(residency_queries == 2);
  }
  std::cout << "Texture-upload production residency guard regressions passed\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "test.cpp"
    binary = args.output.resolve() / "test.exe"
    source.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(source), "-o", str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
