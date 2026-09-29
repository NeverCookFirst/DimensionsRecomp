#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

float4 main(in float4 position : SV_Position,
            in float2 tex_coord : TEXCOORD) : SV_Target {
  Texture2D<float4> texture =
      g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
  return texture.Sample(g_SamplerDescriptorHeap[0], tex_coord) *
         float4(g_PushConstants.Param0.xxx, 1.0);
}
