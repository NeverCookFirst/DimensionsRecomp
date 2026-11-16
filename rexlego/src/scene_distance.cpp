#include "scene_distance.h"
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc.h>
#include <rex/types.h>

REXCVAR_DEFINE_DOUBLE(native_scene_draw_distance_scale,1.0,"Graphics",
    "Scene object clip and LOD fade distance multiplier (0.5 to 2). "
    "Experimental; restart the game after changing. 1 preserves level settings.")
    .range(0.5,2.0).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(native_level_render_distance_scale,1.0,"Graphics",
    "Level camera far-clip multiplier (0.5 to 2), capped at the game's 9000. "
    "Experimental; restart the game after changing. 1 preserves level settings.")
    .range(0.5,2.0).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" void __imp__sub_82C6F428(PPCContext& __restrict ctx,u8* base);
extern "C" void __imp__sub_83A11F50(PPCContext& __restrict ctx,u8* base);

namespace {
struct AppliedDistance {
  std::array<u8,32> baseline{},scaled{};
  size_t size=0;
};
std::mutex distance_mutex;
std::unordered_map<u32,AppliedDistance> instance_distances,level_distances;
// Restore our own previous result before serialization. A reader then loads
// fresh asset values; a writer sees the original asset distances. This also
// prevents repeated script loads from multiplying the same far clip again.
void Restore(std::unordered_map<u32,AppliedDistance>& records,u32 address,u8* base){
  if(!address)return;
  auto it=records.find(address);
  if(it!=records.end()&&!std::memcmp(base+address,it->second.scaled.data(),it->second.size))
    std::memcpy(base+address,it->second.baseline.data(),it->second.size);
  records.erase(address);
}
void Remember(std::unordered_map<u32,AppliedDistance>& records,u32 address,
    const AppliedDistance& record){
  if(records.size()>=1024)records.erase(records.begin());
  records[address]=record;
}
u32 LevelSettings(u32 level,u8* base){
  if(!level)return 0;
  const u32 hierarchy=legodimensions::ReadSceneWord({base+level,268},264);
  return hierarchy?legodimensions::ReadSceneWord({base+hierarchy,1816},1812):0;
}
}

REX_HOOK_RAW(sub_82C6F428) {
  const u32 instance=ctx.r3.u32;
  std::unique_lock lock(distance_mutex);
  Restore(instance_distances,instance,base);
  lock.unlock();
  __imp__sub_82C6F428(ctx,base);
  const double scale=legodimensions::SceneDistanceScale(REXCVAR_GET(native_scene_draw_distance_scale));
  AppliedDistance record;record.size=32;
  if(instance&&scale!=1.0)std::memcpy(record.baseline.data(),base+instance,32);
  if(instance && scale!=1.0 && legodimensions::ScaleSceneInstanceDistances({base+instance,32},scale)){
    std::memcpy(record.scaled.data(),base+instance,32);
    {std::lock_guard guard(distance_mutex);Remember(instance_distances,instance,record);}
    static std::atomic<u32> logs{0};
    if(logs.fetch_add(1)<16)REXLOG_INFO("Native distance: loaded scene instance {:08X} clip/fade scale={}",instance,scale);
  }
}
REX_HOOK_RAW(sub_83A11F50) {
  const u32 level=ctx.r3.u32;
  {std::lock_guard guard(distance_mutex);
    if(!level_distances.empty())Restore(level_distances,LevelSettings(level,base),base);}
  __imp__sub_83A11F50(ctx,base);
  const double scale=legodimensions::SceneDistanceScale(REXCVAR_GET(native_level_render_distance_scale));
  if(!level||scale==1.0)return;
  const u32 settings=LevelSettings(level,base);
  // Parser publishes this pointer only when it selected the level settings.
  const u32 selected=legodimensions::ReadSceneWord({base+0x84A1D938u,4},0);
  if(!settings||settings!=selected)return;
  AppliedDistance record;record.size=10;
  std::memcpy(record.baseline.data(),base+settings,10);
  if(legodimensions::ScaleLevelFarClip({base+settings,10},scale)){
    std::memcpy(record.scaled.data(),base+settings,10);
    {std::lock_guard guard(distance_mutex);Remember(level_distances,settings,record);}
    static std::atomic<u32> logs{0};
    if(logs.fetch_add(1)<8)REXLOG_INFO("Native distance: loaded level {:08X} far-clip scale={}",level,scale);
  }
}
