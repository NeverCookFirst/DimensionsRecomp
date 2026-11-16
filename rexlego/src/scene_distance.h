#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <limits>

namespace legodimensions {
inline double SceneDistanceScale(double value) {
  return std::isfinite(value) ? std::clamp(value, 0.5, 2.0) : 1.0;
}
inline uint32_t ReadSceneWord(std::span<const uint8_t> data, size_t at) {
  return (uint32_t(data[at])<<24)|(uint32_t(data[at+1])<<16)|
      (uint32_t(data[at+2])<<8)|data[at+3];
}
inline void WriteSceneWord(std::span<uint8_t> data,size_t at,uint32_t value) {
  for(size_t i=0;i<4;++i)data[at+i]=uint8_t(value>>(24-8*i));
}
// TU23 NuSceneInstance serializer 82C6F428: clip float at12, four fade
// distances at16. Nonpositive values are engine sentinels, not distances.
inline bool ScaleSceneInstanceDistances(std::span<uint8_t> data,double scale) {
  scale=SceneDistanceScale(scale);
  if(data.size()<32||scale==1.0)return false;
  bool changed=false;
  for(size_t at=12;at<32;at+=4){
    const uint32_t bits=ReadSceneWord(data,at);
    const float value=std::bit_cast<float>(bits);
    if(!std::isfinite(value)||value<=0)continue;
    const double next=double(value)*scale;
    if(next>double(std::numeric_limits<float>::max()))continue;
    const uint32_t replacement=std::bit_cast<uint32_t>(float(next));
    WriteSceneWord(data,at,replacement);changed|=replacement!=bits;
  }
  return changed;
}
// The level's farclip_360 setter clamps the unsigned16 field8 to10..9000.
// Keep its inherited/zero sentinel and near-clip field4 unchanged.
inline bool ScaleLevelFarClip(std::span<uint8_t> data,double scale) {
  scale=SceneDistanceScale(scale);
  if(data.size()<10||scale==1.0)return false;
  const uint32_t value=(uint32_t(data[8])<<8)|data[9];
  if(!value)return false;
  const uint32_t next=uint32_t(std::clamp(std::round(value*scale),10.0,9000.0));
  data[8]=uint8_t(next>>8);data[9]=uint8_t(next);return next!=value;
}
} // namespace legodimensions
