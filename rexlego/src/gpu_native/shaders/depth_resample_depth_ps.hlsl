#include "depth_resample_common.hlsli"
float main(float4 position:SV_Position):SV_Depth {
    return SourceDepth.Load(int3(SourcePixel(position),0));
}
