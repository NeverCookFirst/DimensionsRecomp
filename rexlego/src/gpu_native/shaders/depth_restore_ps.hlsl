#include "copy_common.hlsli"
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float main(float4 position : SV_Position) : SV_Depth {
  uint2 pixel = uint2(position.xy);
  // Color/depth EDRAM bank ordering differs by a 40-sample half-tile.
  pixel.x = pixel.x % 80 < 40 ? pixel.x + 40 : pixel.x - 40;
  uint4 bytes = uint4(round(saturate(
      g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
          .Load(int3(pixel, 0))) * 255.0));
  uint depth = bytes.y | (bytes.z << 8) | (bytes.w << 16);
  return float(depth + (depth >> 23)) * (1.0 / 16777216.0);
}
