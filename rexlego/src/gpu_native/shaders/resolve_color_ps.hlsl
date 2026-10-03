#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(float4 position : SV_Position) : SV_Target {
  // Exact texel resolve, not filtered presentation. Render-target conversion
  // performs the destination UNORM clamping/quantization.
  return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
      .Load(int3(int2(position.xy) + int2(
          int(g_PushConstants.ResourceDescriptorIndex2 << 16) >> 16,
          int(g_PushConstants.ResourceDescriptorIndex2) >> 16), 0)) * g_PushConstants.Param0;
}
