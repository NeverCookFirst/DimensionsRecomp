#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace legodimensions::gpu_native {
// TU23 SetGammaRamp converts the title's 16-bit RGB ramp for the current
// display and stores it at device+15408. 83FCC520 then packs it into the
// DC_LUT_30_COLOR registers. Keep that conversion; do not apply a second
// guessed power/brightness curve in the native presenter.
struct DisplayGammaTable {
  std::array<uint32_t, 256> words{};
  friend bool operator==(const DisplayGammaTable&, const DisplayGammaTable&) = default;
};
inline bool DecodeDisplayGamma(std::span<const uint8_t> converted, DisplayGammaTable& output) {
  if (converted.size() != 1536) return false;
  const auto word = [&](uint32_t at) {
    return (uint32_t(converted[at]) << 8) | converted[at+1];
  };
  for (uint32_t i=0;i<256;++i) {
    const uint32_t red=word(i*2)>>6, green=word(512+i*2)>>6, blue=word(1024+i*2)>>6;
    output.words[i]=(red<<20)|(green<<10)|blue;
  }
  return true;
}
} // namespace legodimensions::gpu_native
