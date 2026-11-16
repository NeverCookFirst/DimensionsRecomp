#include "copy_common.hlsli"
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
cbuffer ScaledResolve : register(b4, space4) {
  float2 g_ResolveRatio;
  float2 g_ResolveOffset;
};

float4 main(float4 position : SV_Position) : SV_Target {
  int2 pixel = int2(position.xy * g_ResolveRatio + g_ResolveOffset);
  return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
      .Load(int3(pixel, 0));
}
