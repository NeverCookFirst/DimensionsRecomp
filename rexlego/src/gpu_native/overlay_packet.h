#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
namespace legodimensions::gpu_native {
struct OverlayTexture {
  uint32_t width=0,height=0;
  bool linear=true,repeat=false;
  std::vector<uint8_t> rgba;
};
struct OverlayVertex {float x,y,u,v;uint32_t color;};
struct OverlayDraw {
  std::shared_ptr<const OverlayTexture> texture;
  uint32_t count=0,first=0,base=0;
  bool lines=false,scissor=false;
  std::array<float,4> clip{};
};
struct OverlayPacket {
  float logical_width=0,logical_height=0;
  uint32_t width=0,height=0;
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
};
inline std::array<int32_t,4> OverlayScissor(const OverlayDraw& draw,
    const OverlayPacket& packet,uint32_t w,uint32_t h) {
  if(!w||!h||w>16384||h>16384||!std::isfinite(packet.logical_width)||
      !std::isfinite(packet.logical_height)||packet.logical_width<=0||packet.logical_height<=0)return {};
  if(!draw.scissor)return {0,0,int32_t(w),int32_t(h)};
  for(float v:draw.clip)if(!std::isfinite(v))return {};
  const float sx=float(w)/packet.logical_width,sy=float(h)/packet.logical_height;
  return {int32_t(std::floor(std::clamp(draw.clip[0]*sx,0.0f,float(w)))),
      int32_t(std::floor(std::clamp(draw.clip[1]*sy,0.0f,float(h)))),
      int32_t(std::ceil(std::clamp(draw.clip[2]*sx,0.0f,float(w)))),
      int32_t(std::ceil(std::clamp(draw.clip[3]*sy,0.0f,float(h))))};
}
inline bool OverlayDrawRange(const OverlayPacket& p,const OverlayDraw& d) {
  if(!d.count||d.base>=p.vertices.size()||d.first>p.indices.size()||
      d.count>p.indices.size()-d.first)return false;
  for(uint32_t i=0;i<d.count;++i)
    if(uint64_t(d.base)+p.indices[d.first+i]>=p.vertices.size())return false;
  return true;
}
} // namespace legodimensions::gpu_native
