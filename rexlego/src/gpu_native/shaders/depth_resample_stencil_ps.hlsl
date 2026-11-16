#include "depth_resample_common.hlsli"
void main(float4 position:SV_Position) {
    if ((SourceStencil.Load(int3(SourcePixel(position),0)).y & StencilBit)==0) discard;
}
