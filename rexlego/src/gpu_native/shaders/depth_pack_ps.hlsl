#include "copy_common.hlsli"
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(float4 position : SV_Position) : SV_Target {
  float depth = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
      .Load(int3(int2(position.xy), 0)).x;
  uint packed = uint(round(saturate(depth) * 16777215.0)) << 8;
  // Raw k_24_8 memory viewed as k_8_8_8_8. Both views have the same endian.
  // The current depth sampling mirror has no stencil plane; preserve depth.
  return float4(packed & 255, (packed >> 8) & 255,
      (packed >> 16) & 255, packed >> 24) / 255.0;
}
