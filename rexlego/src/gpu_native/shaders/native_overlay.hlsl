cbuffer Transform : register(b0) { float2 scale; float2 translate; uint filterMode; };
Texture2D<float4> image : register(t0);
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
SamplerState linearWrap : register(s2);
SamplerState pointWrap : register(s3);
struct Vertex {float2 pos:POSITION;float2 uv:TEXCOORD;float4 color:COLOR;};
struct Pixel {float4 pos:SV_Position;float2 uv:TEXCOORD;float4 color:COLOR;};
Pixel vs_main(Vertex v) {
  Pixel p;p.pos=float4(v.pos*scale+translate,0,1);p.uv=v.uv;p.color=v.color;return p;
}
float4 ps_main(Pixel p):SV_Target {
  float4 sample;
  if(filterMode==0)sample=image.Sample(linearClamp,p.uv);
  else if(filterMode==1)sample=image.Sample(pointClamp,p.uv);
  else if(filterMode==2)sample=image.Sample(linearWrap,p.uv);
  else sample=image.Sample(pointWrap,p.uv);
  return p.color*sample;
}
