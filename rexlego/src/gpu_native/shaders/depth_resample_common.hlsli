Texture2D<float> SourceDepth : register(t0);
Texture2D<uint2> SourceStencil : register(t1);
cbuffer Mapping : register(b0) {
    uint SourceWidth, SourceHeight, DestinationWidth, DestinationHeight, StencilBit;
};
uint2 SourcePixel(float4 position) {
    uint2 pixel=uint2(position.xy);
    // Exact nearest source center, including odd extents and noninteger ratios.
    return min(((pixel*2+1)*uint2(SourceWidth,SourceHeight)) /
        (2*uint2(DestinationWidth,DestinationHeight)),uint2(SourceWidth-1,SourceHeight-1));
}
