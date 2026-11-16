#include "copy_common.hlsli"
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float main(float4 position : SV_Position) : SV_Depth {
  uint2 pixel = uint2(position.xy);
  // The guest's 40-sample half-tile scales with the host surface width.
  float halfTile = g_PushConstants.Param0;
  uint width, height;
  g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex].GetDimensions(width, height);
  // Integer classification keeps exact tile boundaries out of the driver's
  // approximate reciprocal used for floating division (notably odd extents).
  uint halfIndex = ((pixel.x * 2 + 1) * g_PushConstants.ResourceDescriptorIndex2) / (width * 2);
  float sourceX = position.x + ((halfIndex & 1) == 0 ? halfTile : -halfTile);
  pixel.x = uint(sourceX);
  uint4 bytes = uint4(round(saturate(
      g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
          .Load(int3(pixel, 0))) * 255.0));
  uint depth = bytes.y | (bytes.z << 8) | (bytes.w << 16);
  return float(depth + (depth >> 23)) * (1.0 / 16777216.0);
}
