#include "gpu_native/settings_runtime.h"
#include "gpu_native/temporal_present.h"
#include "settings_service.h"
#include <rex/cvar.h>
#include <rex/ui/window.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace legodimensions;
using namespace legodimensions::gpu_native;
namespace test {
bool temporal_ok=true,scene_ok=true;
int temporal_prepares=0,temporal_commits=0,scene_prepares=0,af=-1,saves=0;
uint32_t scene_width=1280,scene_height=720;
NativeFsr3Request applied{3,150,{true,.2f,0}};
int checks=0;
void Check(bool value,const char* message) { ++checks; if(!value) throw std::runtime_error(message); }
settings::Values Registry() {
 return {{"fullscreen","false"},{"native_vsync","false"},{"native_fps_limit","240"},
 {"native_show_fps","true"},{"native_bloom","true"},{"native_cel_shading","original"},
 {"native_resolution_scale","0"},{"native_fsr3","true"},{"native_fsr3_quality","quality"},
 {"native_fsr3_sharpen","true"},{"native_fsr3_sharpness","0.2"},{"native_fsr3_mip_bias","0"},
 {"present_effect","bilinear"},{"present_fsr_sharpness_reduction","0.2"},
 {"present_fsr_max_upsampling_passes","4"},{"native_anisotropic_override","-2"},{"depth_of_field","true"}};
}
struct Harness {
 settings::Values values=Registry();
 rex::ui::Window window;
 std::shared_ptr<NativeSettingsRuntime> runtime;
 settings::Backend backend;
 Harness() {
  mock_window={};rex::cvar::values={{"present_dither","true"},{"skip_pixel_shaders","1234"}};
  temporal_ok=scene_ok=true;temporal_prepares=temporal_commits=scene_prepares=0;
  applied={3,150,{true,.2f,0}};
  settings::Backend registry{
   [this]{return values;}, [](auto,auto value){return settings::NormalizedValue{true,std::string(value),{}};},
   [this](auto key,auto value){
    values[std::string(key)]=value;
    if(key=="fullscreen") {
      const bool full=value=="true";
      window.fullscreen=full;mock_window.style=full?(WS_POPUP|WS_MINIMIZEBOX):WS_OVERLAPPEDWINDOW;
      mock_window.width=full?1920:1280;mock_window.height=full?1080:720;
    }
    if(key=="depth_of_field") rex::cvar::values["skip_pixel_shaders"]=value=="false"?"1234,CFEAC7ADB912F8A9":"1234";
    return true;
   },
   [](const auto&,std::string&){++saves;return true;}};
  runtime=std::make_shared<NativeSettingsRuntime>(std::move(registry),&window);
  backend=runtime->MakeBackend();
 }
 auto Submit(settings::Values edits) { return backend.submit(edits); }
 void Present(bool vsync=false,bool effects=true) {
  runtime->Presented(runtime->Active(),{uint32_t(mock_window.width),uint32_t(mock_window.height),vsync},effects);
 }
};
void Tests() {
 {
  Harness h;
  const auto original=g_native_visual_effects.load();
  auto id=h.Submit({{"native_bloom","false"},{"native_cel_shading","everywhere"}}).revision;
  h.runtime->AdvanceFromUI();
  Check(g_native_visual_effects.load()==original,"draft effects reached material/draw consumers before boundary");
  h.runtime->CommitFrameBoundary();
  Check(!h.runtime->Active()->values.bloom&&h.runtime->Active()->values.cel_shading==NativeCelShadingMode::kEverywhere,
      "effect snapshot did not commit");
  Check(g_native_visual_effects.load()==0x80000002u,"material predicates did not receive committed effects");
  h.Present();Check(h.backend.observe(id).state==settings::RevisionState::kComplete,"effects did not acknowledge");
  h.Submit({{"native_bloom","true"},{"native_cel_shading","off"}});h.runtime->AdvanceFromUI();
  scene_ok=false;
  // Force a scene preparation too, to exercise a rejected whole revision.
  h.Submit({{"native_resolution_scale","200"},{"native_fsr3","false"}});h.runtime->AdvanceFromUI();
  h.runtime->CommitFrameBoundary();
  Check(g_native_visual_effects.load()==0x80000002u,"failed scene change published partial effects");
 }
 {
  Harness h;
  settings::Transaction transaction(h.backend,{"native_fsr3","fullscreen"});
  Check(transaction.Open() && transaction.Edit("fullscreen","true"),"menu transaction could not open/edit auxiliary snapshot");
  auto now=settings::Clock::now();Check(transaction.Apply(now),"menu transaction did not submit Apply");
  Check(transaction.phase()==settings::Phase::kApplying,"confirmation started before actual consumers");
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();transaction.Tick(now);
  Check(transaction.phase()==settings::Phase::kApplying,"transaction bypassed presentation gate");
  h.Present();transaction.Tick(now);
  Check(transaction.phase()==settings::Phase::kPreview,"confirmation did not start after actual receipt");
  Check(transaction.Keep(now) && transaction.phase()==settings::Phase::kEditing,"confirmed native transaction did not save");
 }
 {
  Harness h;std::vector<std::function<void()>> queue;
  settings::QueuedSettings service(h.backend,[&](auto callback){queue.push_back(std::move(callback));return true;},Registry(),{"fullscreen"});
  const auto command=[&](settings::Command value){auto future=service.Submit(std::move(value));auto callback=std::move(queue.front());queue.erase(queue.begin());callback();return future.get();};
  Check(command({settings::CommandKind::kOpen}).ok,"service could not open runtime backend");
  Check(command({settings::CommandKind::kEdit,"fullscreen","true"}).ok && command({settings::CommandKind::kApply}).phase==settings::Phase::kApplying,"service did not submit coherent revision");
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();h.Present();service.AdvanceFromUI();
  Check(service.FocusLostFromUI().phase==settings::Phase::kRestoring,"focus loss did not submit rollback");
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();h.Present();
  Check(service.TickFromUI().phase==settings::Phase::kEditing && h.values.at("fullscreen")=="false" && !h.runtime->Active()->values.fullscreen,"focus rollback did not restore UI/render/window together");
  Check(service.ShutdownFromUI().ok,"idle service/runtime shutdown failed");
 }
 {
  Harness h;
  auto id=h.Submit({{"native_fps_limit","0"},{"native_anisotropic_override","5"},{"depth_of_field","false"}}).revision;
  Check(h.values.at("native_fps_limit")=="240","submit wrote SDK registry inline");
  Check(h.runtime->Active()->values.fps_limit==240,"proposal leaked before UI write");
  h.runtime->AdvanceFromUI();
  Check(h.values.at("native_fps_limit")=="0","UI registry writer not connected");
  Check(h.runtime->Active()->values.fps_limit==240,"registry leaked before render commit");
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"registry falsely acknowledged");
  h.runtime->CommitFrameBoundary();
  Check(h.runtime->Active()->values.fps_limit==0 && af==5 && !h.runtime->Active()->values.depth_of_field,"full render payload/AF/DoF not committed");
  Check(h.runtime->Active()->values.skip_pixel_shaders=="1234","DoF callback corrupted auxiliary conflict snapshot");
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"render without present falsely acknowledged");
  h.Present();
  Check(h.backend.observe(id).state==settings::RevisionState::kComplete,"render/present/window did not acknowledge");
  std::string error; Check(h.backend.persist({{"native_fps_limit","0"}},error),"acknowledged save failed");
  mock_window.width=1111;
  Check(!h.backend.persist({{"native_fps_limit","0"}},error),"save ignored resized physical window");
 }
 {
  Harness h;auto id=h.Submit({{"fullscreen","true"},{"native_vsync","true"}}).revision;
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();h.Present(false);
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"wrong presentation VSync acknowledged");
  h.Present(true);mock_window.style=WS_OVERLAPPEDWINDOW;
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"desired SDK flag substituted for physical style");
  mock_window.style=WS_POPUP|WS_MINIMIZEBOX;mock_window.width=1600;h.Present(true);
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"incomplete monitor coverage accepted");
  mock_window.width=1920;h.Present(true);mock_window.iconic=true;
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"minimized window accepted");
  mock_window.iconic=false;mock_window.origin_x=20;
  Check(h.backend.observe(id).state==settings::RevisionState::kPending,"wrong monitor origin accepted");
  mock_window.origin_x=0;
  Check(h.backend.observe(id).state==settings::RevisionState::kComplete,"physical fullscreen completion rejected");
 }
 {
  Harness h;auto initial=h.runtime->Active();temporal_ok=false;
  auto id=h.Submit({{"native_fsr3_quality","performance"},{"native_fps_limit","0"}}).revision;
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();
  Check(h.runtime->Active()==initial && scene_prepares==0,"failed FSR preparation changed active scene/payload");
  Check(h.backend.observe(id).state==settings::RevisionState::kFailed,"resource rejection not reported");
  temporal_ok=true;scene_ok=false;
  id=h.Submit({{"native_fsr3_quality","performance"}}).revision;
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();
  Check(h.runtime->Active()==initial && temporal_commits==0 && applied.mode==3,"failed scene preparation committed FSR plan");
  Check(h.backend.observe(id).state==settings::RevisionState::kFailed,"scene allocation rejection not reported");
  scene_ok=true;id=h.Submit({{"native_fsr3_quality","performance"}}).revision;
  h.runtime->AdvanceFromUI();
  Check(h.runtime->CommitFrameBoundary()==NativeFsr3ApplyResult::kApplied,"preset commit not propagated to draw history");
  Check(applied.mode==7 && scene_width==960 && scene_height==540 && temporal_commits==1,"prepared FSR scene plan not consumed");
  h.Present();Check(h.backend.observe(id).state==settings::RevisionState::kComplete,"recovered request did not complete");
 }
 {
  Harness h;const auto old=h.runtime->Active();
  auto first=h.Submit({{"native_fps_limit","100"}}).revision;h.runtime->AdvanceFromUI();
  auto second=h.Submit({{"native_fps_limit","200"}}).revision;
  h.runtime->CommitFrameBoundary();Check(h.runtime->Active()==old,"superseded sealed request committed");
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();
  h.runtime->Presented(old,{1280,720,false},true);
  Check(h.backend.observe(second).state==settings::RevisionState::kPending,"old presentation confirmed new revision");
  h.Present();Check(h.backend.observe(first).state==settings::RevisionState::kFailed &&
   h.backend.observe(second).state==settings::RevisionState::kComplete,"supersession identity failed");
 }
 {
  Harness h;auto id=h.Submit({{"native_fsr3","false"},{"native_resolution_scale","300"},{"present_effect","fsr"}}).revision;
  h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();
  Check(scene_width==3840 && scene_height==2160 && temporal_prepares==0,"manual 4K/FSR-off plan incorrect");
  h.Present(false,false);Check(h.backend.observe(id).state==settings::RevisionState::kPending,"FSR1 fallback falsely confirmed");
  h.Present();Check(h.backend.observe(id).state==settings::RevisionState::kComplete,"FSR1 presentation not connected");
  auto active=h.runtime->Active();h.runtime->StopFromUI();h.runtime->CommitFrameBoundary();h.Present();
  Check(h.runtime->Active()==active,"stop invalidated copied final frame");
  bool rejected=false;try{h.Submit({{"native_fps_limit","0"}});}catch(...){rejected=true;}
  Check(rejected,"stopped runtime retained registry/window callbacks");
 }
 {
  Harness h;auto id=h.Submit({{"native_fps_limit","100"}}).revision;h.runtime->AdvanceFromUI();
  rex::cvar::values["skip_pixel_shaders"]="different";h.runtime->CommitFrameBoundary();h.Present();
  Check(h.backend.observe(id).state==settings::RevisionState::kFailed,"unrelated renderer input change was not a conflict");
 }
 {
  _putenv_s("LEGO_NATIVE_FPS_LIMIT","777");_putenv_s("LEGO_NATIVE_PRESENT","0,512");
  Harness h;Check(h.runtime->Active()->values.fps_limit==777 && !h.runtime->Active()->values.vsync,"startup overrides not copied");
  auto id=h.Submit({{"native_vsync","true"}}).revision;h.runtime->AdvanceFromUI();h.runtime->CommitFrameBoundary();h.Present(false);
  Check(h.backend.observe(id).state==settings::RevisionState::kComplete && !h.runtime->Active()->values.vsync,"actual presentation override not reflected");
  _putenv_s("LEGO_NATIVE_FPS_LIMIT","");_putenv_s("LEGO_NATIVE_PRESENT","");
 }
}
}
namespace legodimensions::gpu_native {
int32_t NativeInheritedAnisotropy(){return 3;}
void SetNativeActiveAnisotropy(int32_t value){test::af=value;}
bool Native720PRequested(){return false;}
uint32_t NativeResolutionScaleStep(int32_t percent){return uint32_t((std::clamp(percent,20,300)+2)/5*5);}
Fsr3Size NativeScaledSceneSize(uint32_t percent){return {1280*percent/100,720*percent/100};}
Fsr3Size NativeFsr3RequestRenderSize(NativeFsr3Request r){
 constexpr Fsr3Size sizes[]{{1920,1080},{1280,720},{1129,635},{960,540},{640,360}};
 return r.mode&1?sizes[r.mode>>1]:NativeScaledSceneSize(r.scale);
}
bool ReconfigureNativeSceneSurfaces(NativeRenderExtent extent){
 ++test::scene_prepares;if(!test::scene_ok)return false;
 test::scene_width=extent.host_width;test::scene_height=extent.host_height;return true;
}
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(NativeFsr3Request r,bool(*prepare)(NativeRenderExtent)){
 bool change=r.mode!=test::applied.mode || (!(r.mode&1)&&r.scale!=test::applied.scale);
 auto size=NativeFsr3RequestRenderSize(r);
 if(change && !prepare({1280,720,size.width,size.height}))return NativeFsr3ApplyResult::kDeferred;
 test::applied=r;return change?NativeFsr3ApplyResult::kApplied:NativeFsr3ApplyResult::kUnchanged;
}
std::shared_ptr<PreparedTemporalSettings> PrepareTemporalSettings(uint32_t,uint32_t,bool){++test::temporal_prepares;return test::temporal_ok?std::make_shared<PreparedTemporalSettings>():nullptr;}
bool CanCommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& p){return bool(p);}
bool CommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& p,bool){if(!p)return false;++test::temporal_commits;return true;}
bool TemporalDepthInverted(){return false;}
}
int main(){try{test::Tests();std::cout<<"PASS "<<test::checks<<" native runtime integration assertions\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 23;}}
