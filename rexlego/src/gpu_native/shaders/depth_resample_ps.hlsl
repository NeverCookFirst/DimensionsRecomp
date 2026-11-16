#include "depth_resample_common.hlsli"
void main(float4 position:SV_Position,out float depth:SV_Depth,out uint stencil:SV_StencilRef) {
    uint2 pixel=SourcePixel(position);
    depth=SourceDepth.Load(int3(pixel,0));
    stencil=SourceStencil.Load(int3(pixel,0)).y;
}
