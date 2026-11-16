#include "gpu_native/temporal_shaders.h"
#include "gpu_native/device.h"
#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <plume_render_interface.h>
namespace legodimensions::gpu_native {
namespace {
struct TemporalShaderEntry {uint64_t hash;uint32_t stage,spec,offset,size,camera;};
#include LEGO_TEMPORAL_SHADER_METADATA
extern "C" const unsigned char g_temporalShaderBlob[];
using Key=std::tuple<uint64_t,uint32_t,uint32_t>;
std::map<Key,std::unique_ptr<plume::RenderShader>>g_shaders;
const TemporalShaderEntry* Find(Key key){
 auto found=std::lower_bound(std::begin(g_temporal_entries),std::end(g_temporal_entries),key,
  [](const auto&entry,const auto&value){return Key{entry.hash,entry.stage,entry.spec}<value;});
 return found!=std::end(g_temporal_entries)&&Key{found->hash,found->stage,found->spec}==key?found:nullptr;
}
Key Identity(uint64_t hash,bool pixel,uint32_t spec){return {hash,uint32_t(pixel),hash?spec&(pixel?3u:1u):0};}
}
bool HasTemporalShader(uint64_t hash,bool pixel,uint32_t spec){return Find(Identity(hash,pixel,spec))!=nullptr;}
namespace {
plume::RenderShader* Resolve(Key key){
 if(auto found=g_shaders.find(key);found!=g_shaders.end())return found->second.get();
 const auto*entry=Find(key);auto*device=HostDevice::Device();if(!entry||!device)return nullptr;
 auto shader=device->createShader(g_temporalShaderBlob+entry->offset,entry->size,"main",plume::RenderShaderFormat::DXIL);
 if(!shader)return nullptr;
 auto*result=shader.get();g_shaders.emplace(key,std::move(shader));return result;
}
}
plume::RenderShader* ResolveTemporalShader(uint64_t hash,bool pixel,uint32_t spec){return Resolve(Identity(hash,pixel,spec));}
bool HasTemporalCoverageShader(uint64_t hash,TemporalCoverage mode,uint32_t spec){
 return mode!=TemporalCoverage::kNone && Find({hash,uint32_t(mode),spec&3u})!=nullptr;
}
plume::RenderShader* ResolveTemporalCoverageShader(uint64_t hash,TemporalCoverage mode,uint32_t spec){
 return mode!=TemporalCoverage::kNone ? Resolve({hash,uint32_t(mode),spec&3u}) : nullptr;
}
bool HasTemporalCameraConstants(uint64_t hash,uint32_t spec){
 const auto* entry=Find(Identity(hash,false,spec));return entry && entry->camera;
}
void ResetTemporalShaders(){g_shaders.clear();}
}
