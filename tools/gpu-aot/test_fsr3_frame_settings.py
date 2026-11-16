"""Exercise production FSR settings through a synchronized CVar boundary."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');p.add_argument('--expect-old-failure',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
include=out/'include/rex';include.mkdir(parents=True,exist_ok=True)
(include/'cvar.h').write_text(r'''#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <cstdlib>
#include <cstdint>
namespace fixture {
inline std::mutex registry;
inline std::atomic<unsigned> unsafe_reads=0,locked_reads=0;
}
template<class T>struct CvarFixture {
 T value;
 CvarFixture&range(double,double){return *this;}
 T&Unsafe(){++fixture::unsafe_reads;return value;}
};
extern CvarFixture<bool>native_fsr3,native_fsr3_sharpen;
extern CvarFixture<std::string>native_fsr3_quality;
extern CvarFixture<double>native_fsr3_sharpness,native_fsr3_mip_bias;
extern CvarFixture<int32_t>native_resolution_scale;
inline CvarFixture<std::string>present_effect{"bilinear"};
inline CvarFixture<double>present_fsr_sharpness_reduction{.2};
namespace fixture {
template<class T>void Set(CvarFixture<T>& field,T value){
 std::lock_guard lock(registry);field.value=std::move(value);
}
}
namespace rex::cvar {
inline std::string GetFlagByName(std::string_view name){
 std::lock_guard lock(fixture::registry);++fixture::locked_reads;
 if(name=="native_fsr3_quality")return native_fsr3_quality.value;
 if(name=="native_fsr3")return native_fsr3.value?"true":"false";
 if(name=="native_fps_limit")return "240";
 if(name=="present_effect")return present_effect.value;
 std::abort();
}
template<class T>T Query(std::string_view);
template<>inline int32_t Query<int32_t>(std::string_view name){
 std::lock_guard lock(fixture::registry);++fixture::locked_reads;
 if(name=="native_resolution_scale")return native_resolution_scale.value;
 std::abort();
}
template<>inline bool Query<bool>(std::string_view name){
 std::lock_guard lock(fixture::registry);++fixture::locked_reads;
 if(name=="native_fsr3")return native_fsr3.value;
 if(name=="native_fsr3_sharpen")return native_fsr3_sharpen.value;
 std::abort();
}
template<>inline double Query<double>(std::string_view name){
 std::lock_guard lock(fixture::registry);++fixture::locked_reads;
 if(name=="native_fsr3_sharpness")return native_fsr3_sharpness.value;
 if(name=="native_fsr3_mip_bias")return native_fsr3_mip_bias.value;
 if(name=="present_fsr_sharpness_reduction")return present_fsr_sharpness_reduction.value;
 std::abort();
}
inline bool SetFlagByName(std::string_view name,std::string_view value){
 if(name=="present_effect"){fixture::Set(present_effect,std::string(value));return true;}
 if(name=="native_fsr3"){fixture::Set(native_fsr3,value=="true");return true;}
 if(name=="native_fsr3_quality"){fixture::Set(native_fsr3_quality,std::string(value));return true;}
 if(name=="present_fsr_sharpness_reduction"){fixture::Set(present_fsr_sharpness_reduction,std::stod(std::string(value)));return true;}
 if(name=="native_resolution_scale"){fixture::Set(native_resolution_scale,int32_t(std::stoi(std::string(value))));return true;}
 if(name=="native_fsr3_sharpen"){fixture::Set(native_fsr3_sharpen,value=="true");return true;}
 if(name=="native_fsr3_sharpness"){fixture::Set(native_fsr3_sharpness,std::stod(std::string(value)));return true;}
 if(name=="native_fsr3_mip_bias"){fixture::Set(native_fsr3_mip_bias,std::stod(std::string(value)));return true;}
 std::abort();
}
}
#define REXCVAR_DEFINE_BOOL(n,v,...) CvarFixture<bool>n=CvarFixture<bool>{v}
#define REXCVAR_DEFINE_STRING(n,v,...) CvarFixture<std::string>n=CvarFixture<std::string>{v}
#define REXCVAR_DEFINE_DOUBLE(n,v,...) CvarFixture<double>n=CvarFixture<double>{v}
#define REXCVAR_DEFINE_INT32(n,v,...) CvarFixture<int32_t>n=CvarFixture<int32_t>{v}
#define REXCVAR_GET(n) (n.Unsafe())
''')
header=(root/'rexlego/src/gpu_native/fsr3_settings.h').read_text()
menu_source=(root/'rexlego/src/gpu_native/native_overlay.cpp').read_text()
start=menu_source.index('class GraphicsMenu final:')
end=menu_source.index('{',start)+1;depth=1
while depth:depth+=(menu_source[end]=='{')-(menu_source[end]=='}');end+=1
menu=menu_source[start:end]+';'
ui=r'''
#ifdef HAS_FRAME_DETAILS
struct ImGuiIO{int ConfigFlags=0;};struct ImVec2{ImVec2(float,float){}};
constexpr int ImGuiConfigFlags_NavEnableKeyboard=1,ImGuiCond_FirstUseEver=1;
constexpr int ImGuiSliderFlags_AlwaysClamp=1;
namespace rex::ui {
struct ImGuiDrawer{};
struct ImGuiDialog{ImGuiDialog(ImGuiDrawer*){}virtual~ImGuiDialog()=default;virtual void OnDraw(ImGuiIO&)=0;};
}
namespace ImGui {
inline float shown_sharpness=-1,shown_bias=-2;inline bool shown_sharpen=false,inject=false;
inline int choose=-1; inline int shown_resolution_max=0;
inline void SetNextWindowSize(ImVec2,int){}inline bool Begin(const char*){return true;}
template<class...T>void Text(T...){}template<class...T>void TextWrapped(T...){}template<class...T>void TextDisabled(T...){}
inline bool IsWindowAppearing(){return false;}inline void SetKeyboardFocusHere(){}
inline bool Combo(const char*,int*value,const char*const*,int){if(choose>=0){*value=choose;return true;}return false;}
inline void BeginDisabled(bool){}inline void EndDisabled(){}inline bool IsItemActive(){return false;}
inline bool Checkbox(const char*,bool*value){shown_sharpen=*value;if(inject){*value=false;return true;}return false;}
inline bool SliderFloat(const char*label,float*value,float,float){
 if(std::strcmp(label,"Sharpness")==0){shown_sharpness=*value;if(inject){*value=.9f;return true;}}
 else{shown_bias=*value;if(inject){*value=-.25f;return true;}}return false;
}
inline bool SliderInt(const char* label,int*,int,int max,const char* = nullptr,int = 0){if(std::strcmp(label,"Internal resolution")==0)shown_resolution_max=max;return false;}inline void Separator(){}inline void End(){}
}
namespace legodimensions::gpu_native {
struct Bridge{std::atomic<uint32_t>width{0},height{0};};std::atomic<Bridge*>bridge{nullptr};
struct Stats{double fps=100,frame_time_ms=10;};
Stats NativeOverlayStats(){return {};}const char*NativeTemporalPresentationStatus(){return "fixture";}
''' + menu + '\n}\n#endif\n'
main=r'''
#include <rex/cvar.h>
#include "gpu_native/fsr3_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <latch>
#include <thread>
#include <functional>
#include <tuple>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
// UI_FIXTURE
using namespace legodimensions::gpu_native;
std::function<bool(NativeRenderExtent)> prepare_frame;
bool PrepareFrameFixture(NativeRenderExtent extent){return prepare_frame(extent);}
int main(int argc,char**argv){
 CHECK(argc==2);
 if(std::strcmp(argv[1],"sealed")==0){
  const auto old_enabled=NativeFsr3Enabled();const auto old_scene=NativeSceneRenderSize();
  const auto request=NativeFsr3Request{0,300,{false,.65f,-.25f}};
  fixture::Set(native_fsr3,true);fixture::Set(native_fsr3_quality,std::string("ultra_performance"));
  fixture::Set(native_resolution_scale,int32_t(20));fixture::Set(native_fsr3_sharpness,.99);
  fixture::Set(native_fsr3_sharpen,true);fixture::Set(native_fsr3_mip_bias,1.0);
  const auto reads=fixture::locked_reads.load();bool ready=false;unsigned calls=0;
  prepare_frame=[&](NativeRenderExtent extent){++calls;CHECK(extent.width==3840&&extent.height==2160);
   CHECK(NativeFsr3Enabled()==old_enabled&&NativeSceneRenderSize().width==old_scene.width);return ready;};
  CHECK(TryApplyNativeFsr3Settings(request,PrepareFrameFixture)==NativeFsr3ApplyResult::kDeferred);
  ready=true;CHECK(TryApplyNativeFsr3Settings(request,PrepareFrameFixture)==NativeFsr3ApplyResult::kApplied);
  CHECK(!NativeFsr3Enabled()&&NativeResolutionScale()==300&&NativeSceneRenderSize().width==3840);
  CHECK(!NativeFsr3Sharpening()&&NativeFsr3Sharpness()==.65f&&fixture::locked_reads==reads&&calls==2);
  for(auto invalid:std::vector<NativeFsr3Request>{
   {10,300,{false,.65f,0}},{0,0,{false,.65f,0}},{0,301,{false,.65f,0}},{0,201,{false,.65f,0}},
   {0,300,{false,std::numeric_limits<float>::quiet_NaN(),0}},{0,300,{false,.5f,2}}}){
   CHECK(TryApplyNativeFsr3Settings(invalid,PrepareFrameFixture)==NativeFsr3ApplyResult::kDeferred);
   CHECK(!NativeFsr3Enabled()&&NativeResolutionScale()==300&&NativeFsr3Sharpness()==.65f&&calls==2);
  }
  puts("PASS copied FSR request ignores later registry changes, retains old settings during preparation and rejects six invalid payloads");return 0;
 }
 if(std::strcmp(argv[1],"deferred")==0){
  unsigned calls=0,scenarios=0;
  for(auto [enabled,quality,scale,width,height]:std::vector<std::tuple<bool,std::string,int,int,int>>{
   {false,"quality",300,3840,2160},{true,"native",300,1920,1080},
   {true,"balanced",300,1129,635},{true,"ultra_performance",300,640,360},
   {false,"quality",200,2560,1440},{false,"quality",100,1280,720}}){
   const auto old_enabled=NativeFsr3Enabled(),old_sharpen=NativeFsr3Sharpening();
   const auto old_scene=NativeSceneRenderSize();const auto old_scale=NativeResolutionScale();
   const auto old_sharpness=NativeFsr3Sharpness(),old_bias=NativeFsr3MipBias();
   fixture::Set(native_fsr3,enabled);fixture::Set(native_fsr3_quality,quality);
   fixture::Set(native_resolution_scale,int32_t(scale));fixture::Set(native_fsr3_sharpness,.63);
   fixture::Set(native_fsr3_sharpen,false);fixture::Set(native_fsr3_mip_bias,-.25);
   bool ready=false;
   prepare_frame=[&](NativeRenderExtent extent){++calls;
    CHECK(extent.logical_width==1280&&extent.logical_height==720&&extent.width==width&&extent.height==height);
    CHECK(NativeFsr3Enabled()==old_enabled&&NativeFsr3Sharpening()==old_sharpen);
    CHECK(NativeResolutionScale()==old_scale&&NativeFsr3Sharpness()==old_sharpness&&NativeFsr3MipBias()==old_bias);
    auto scene=NativeSceneRenderSize();CHECK(scene.width==old_scene.width&&scene.height==old_scene.height);
    return ready;};
   for(int attempt=0;attempt<2;++attempt){CHECK(TryApplyNativeFsr3Settings(PrepareFrameFixture)==NativeFsr3ApplyResult::kDeferred);
    CHECK(NativeFsr3Enabled()==old_enabled&&NativeFsr3Sharpness()==old_sharpness);++scenarios;}
   ready=true;CHECK(TryApplyNativeFsr3Settings(PrepareFrameFixture)==NativeFsr3ApplyResult::kApplied);
   auto scene=NativeSceneRenderSize();CHECK(scene.width==width&&scene.height==height&&NativeResolutionScale()==unsigned(scale));
   CHECK(NativeFsr3Enabled()==enabled&&NativeFsr3Sharpness()==.63f&&!NativeFsr3Sharpening());++scenarios;
   const auto before=calls;fixture::Set(native_fsr3_sharpness,.77);
   CHECK(TryApplyNativeFsr3Settings(PrepareFrameFixture)==NativeFsr3ApplyResult::kUnchanged);
   CHECK(calls==before&&NativeFsr3Sharpness()==.77f);++scenarios;
  }
  CHECK(calls==18);
  puts("PASS 24 staged mode/extent scenarios: failed preparation retains all active settings, retry publishes after preparation, exact native/FSR extents, detail-only no-resize");return 0;
 }
 if(std::strcmp(argv[1],"resolution")==0){
  CHECK(NativeFsr3Enabled());
  CHECK(NativeResolutionScaleStep(std::numeric_limits<int32_t>::min())==20);
  CHECK(NativeResolutionScaleStep(std::numeric_limits<int32_t>::max())==300);
  for(int n=20;n<=300;n+=5){
   CHECK(NativeResolutionScaleStep(n)==unsigned(n));
   auto extent=NativeScaledSceneSize(unsigned(n));
   CHECK(extent.width*9==extent.height*16);
  }
  fixture::Set(native_fsr3,false);fixture::Set(native_resolution_scale,int32_t(300));
  CHECK(ApplyNativeFsr3Settings());
  CHECK(!NativeFsr3Enabled()&&NativeResolutionScale()==300);
  auto size=NativeSceneRenderSize();CHECK(size.width==3840&&size.height==2160);
  auto scene=NativeConfiguredSceneExtent(1280,720);
  CHECK(scene.width==3840&&scene.height==2160);
  auto atlas=NativeConfiguredSceneExtent(512,512);CHECK(atlas.width==512&&atlas.height==512);
  fixture::Set(native_resolution_scale,int32_t(200));CHECK(ApplyNativeFsr3Settings());
  size=NativeSceneRenderSize();CHECK(size.width==2560&&size.height==1440);
  CHECK(!ApplyNativeFsr3Settings());
#ifdef HAS_FRAME_DETAILS
  rex::ui::ImGuiDrawer drawer;GraphicsMenu menu(&drawer);ImGuiIO io;menu.OnDraw(io);
  CHECK(ImGui::shown_resolution_max==300);
#endif
  std::puts("PASS 57 exact 16:9 steps through 4K, extreme input clamps, frame-boundary 4K/1440p transition and unscaled atlas");return 0;
 }
 const auto enabled=NativeFsr3Enabled();const auto size=NativeFsr3RenderSize();
 const auto sharp=NativeFsr3Sharpness(),bias=NativeFsr3MipBias();const auto sharpen=NativeFsr3Sharpening();
 CHECK(enabled&&size.width==1280&&sharp==.2f&&sharpen);
 if(std::strcmp(argv[1],"boundary")==0){
  std::printf("unguarded_cvar_reads=%u\n",fixture::unsafe_reads.load());
  CHECK(fixture::unsafe_reads==0);return 0;
 }
 fixture::Set(native_fsr3_sharpness,.85);fixture::Set(native_fsr3_sharpen,false);
 fixture::Set(native_fsr3_mip_bias,.75);
 // No frame commit has happened. Every render draw keeps the first values.
 CHECK(NativeFsr3Sharpness()==sharp&&NativeFsr3Sharpening()==sharpen&&NativeFsr3MipBias()==bias);
#ifdef HAS_FRAME_DETAILS
 const auto requested=NativeFsr3RequestedDetails();
 CHECK(requested.sharpness==.85f&&!requested.sharpen&&requested.mip_bias_adjustment==.75f);
#endif
 CHECK(!ApplyNativeFsr3Settings()); // Detail-only changes don't request resize.
 CHECK(NativeFsr3Sharpness()==.85f&&!NativeFsr3Sharpening());
 CHECK(std::abs(NativeFsr3MipBias()-(bias+.75f))<.00001f);
 CHECK(!ApplyNativeFsr3Settings()&&fixture::unsafe_reads==0);
 if(std::strcmp(argv[1],"stability")==0){std::puts("PASS next-frame detail commit, stable draws, no mode resize");return 0;}
#ifdef HAS_FRAME_DETAILS
 if(std::strcmp(argv[1],"choices")==0){
  rex::ui::ImGuiDrawer drawer;GraphicsMenu menu(&drawer);ImGuiIO io;
  ImGui::choose=1;menu.OnDraw(io);
  CHECK(!native_fsr3.value&&present_effect.value=="fsr");ApplyNativeFsr3Settings();
  CHECK(!NativeFsr3Enabled());
  const char*quality[]{"native","quality","balanced","performance","ultra_performance"};
  for(int selected=2;selected<7;++selected){
   ImGui::choose=selected;menu.OnDraw(io);
   CHECK(native_fsr3.value&&native_fsr3_quality.value==quality[selected-2]&&present_effect.value=="bilinear");
   ApplyNativeFsr3Settings();CHECK(NativeFsr3Enabled());
  }
  ImGui::choose=0;menu.OnDraw(io);ApplyNativeFsr3Settings();
  CHECK(!NativeFsr3Enabled()&&present_effect.value=="bilinear");
  puts("PASS actual F5 menu: Off, ReXGlue FSR1 and all five FSR3 choices");return 0;
 }
 if(std::strcmp(argv[1],"menu")==0){
  fixture::Set(native_fsr3_sharpness,.4);fixture::Set(native_fsr3_mip_bias,-.5);fixture::Set(native_fsr3_sharpen,true);
  rex::ui::ImGuiDrawer drawer;GraphicsMenu menu(&drawer);ImGuiIO io;ImGui::inject=true;menu.OnDraw(io);
  CHECK(ImGui::shown_sharpness==.4f&&ImGui::shown_bias==-.5f&&ImGui::shown_sharpen);
  CHECK(NativeFsr3Sharpness()==.85f&&!NativeFsr3Sharpening());
  const auto request=NativeFsr3RequestedDetails();CHECK(request.sharpness==.9f&&request.mip_bias_adjustment==-.25f&&!request.sharpen);
  CHECK(!ApplyNativeFsr3Settings());CHECK(NativeFsr3Sharpness()==.9f);
  CHECK(std::abs(NativeFsr3MipBias()-(bias-.25f))<.00001f);
  std::puts("PASS actual GraphicsMenu requested slider values, registry setters and next-frame render commit");return 0;
 }
#endif
 CHECK(std::strcmp(argv[1],"threads")==0);
 std::atomic<bool> running=true;std::atomic<unsigned> writes=0;
 std::latch first_write(1);
 std::thread ui([&]{
  while(running.load()){
   const auto n=writes.fetch_add(1);
   fixture::Set(native_fsr3_sharpness,n%2?-.5:1.5);
   fixture::Set(native_fsr3_mip_bias,n%3?-.75:std::numeric_limits<double>::quiet_NaN());
   fixture::Set(native_fsr3_sharpen,bool(n%2));
   fixture::Set(native_fsr3_quality,std::string(n%2?"quality":"ultra_performance"));
   fixture::Set(native_fsr3,bool(n%3));
   if(n==0)first_write.count_down();
   std::this_thread::yield();
  }
 });
 first_write.wait();
 for(unsigned frame=0;frame<2000;++frame){
  ApplyNativeFsr3Settings();
  const auto e=NativeFsr3Enabled(),s=NativeFsr3Sharpening();
  const auto extent=NativeFsr3RenderSize();
  const float strength=NativeFsr3Sharpness(),offset=NativeFsr3MipBias();
  CHECK(std::isfinite(strength)&&strength>=0&&strength<=1);
  CHECK(std::isfinite(offset)&&offset>=-3&&offset<=0);
  for(unsigned draw=0;draw<100;++draw){
   CHECK(NativeFsr3Enabled()==e&&NativeFsr3Sharpening()==s);
   CHECK(NativeFsr3Sharpness()==strength&&NativeFsr3MipBias()==offset);
   const auto after=NativeFsr3RenderSize();CHECK(after.width==extent.width&&after.height==extent.height);
  }
#ifdef HAS_FRAME_DETAILS
  const auto ui_values=NativeFsr3RequestedDetails();
  CHECK(std::isfinite(ui_values.sharpness)&&std::isfinite(ui_values.mip_bias_adjustment));
#endif
 }
 running=false;ui.join();CHECK(writes>0&&fixture::unsafe_reads==0);
 std::printf("PASS 2000 frame commits/200000 stable draw snapshots, concurrent writer batches=%u, locked_reads=%u\n",writes.load(),fixture::locked_reads.load());
}
'''
main=main.replace('// UI_FIXTURE',ui)
source=root/'rexlego/src/gpu_native/fsr3_settings.cpp'
cpp=out/'frame-settings.cpp';cpp.write_text(main)
env=os.environ.copy()
for key in ('LEGO_NATIVE_FSR3','LEGO_NATIVE_FSR3_QUALITY','LEGO_NATIVE_720P'):env.pop(key,None)
def build(name,text,harness=None):
    production=out/(name+'-production.cpp');production.write_text(text)
    exe=out/(name+'.exe')
    selected_cpp=cpp
    if harness is not None:
        selected_cpp=out/(name+'-main.cpp');selected_cpp.write_text(harness)
    cmd=[a.compiler,'-std=c++20','-O2','-UNDEBUG','-D_CRT_SECURE_NO_WARNINGS',
        '-I'+str(out/'include'),'-I'+str(root/'rexlego/src'),str(selected_cpp),str(production),'-o',str(exe)]
    if 'NativeFsr3DetailSettings' in header:cmd+=['-DHAS_FRAME_DETAILS']
    if os.name!='nt':cmd+=['-pthread']
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();return exe
def run(exe,case):
    r=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=20,env=env)
    (out/(exe.stem+'-'+case+'.log')).write_text(r.stdout+r.stderr);return r
text=source.read_text();exe=build('current',text);positive=[]
for case in ('boundary','stability','resolution','deferred','sealed'):
    r=run(exe,case);assert r.returncode==(23 if a.expect_old_failure else 0),(case,r.stdout,r.stderr)
    positive.append(dict(case=case,exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
if a.expect_old_failure:
    (out/'verification.json').write_text(json.dumps(dict(before_fix=True,results=positive,source_sha256=hashlib.sha256(text.encode()).hexdigest(),
        scope='actual settings functions; deterministic raw-access seam and mid-frame changes; no racing execution or TSan claim'),indent=2))
    print('REPRODUCED unguarded detail reads and mid-frame FSR value changes');raise SystemExit(0)
r=run(exe,'threads');assert r.returncode==0,r.stdout+r.stderr
positive.append(dict(case='threads',exit=r.returncode,stdout=r.stdout));print(r.stdout)
r=run(exe,'menu');assert r.returncode==0,r.stdout+r.stderr
positive.append(dict(case='menu',exit=r.returncode,stdout=r.stdout));print(r.stdout)
r=run(exe,'choices');assert r.returncode==0,r.stdout+r.stderr
positive.append(dict(case='choices',exit=r.returncode,stdout=r.stdout));print(r.stdout)
negative=[]
for name,old,new in [
    ('raw-detail-read','rex::cvar::Query<double>("native_fsr3_sharpness")','REXCVAR_GET(native_fsr3_sharpness)'),
    ('live-render-getter','return ActiveSettings().sharpness.load();','return NativeFsr3RequestedDetails().sharpness;'),
    ('ignore-prepare-failure','if (!prepare_scene({1280,720,size.width,size.height})) return NativeFsr3ApplyResult::kDeferred;',
     'prepare_scene({1280,720,size.width,size.height});'),
    ('sealed-mode-reads-registry','const auto requested = request.mode;','const auto requested = RequestedMode();')]:
    assert text.count(old)==1,(name,text.count(old))
    bad=build(name,text.replace(old,new));case='boundary' if name=='raw-detail-read' else 'deferred' if name=='ignore-prepare-failure' else 'sealed' if name=='sealed-mode-reads-registry' else 'stability'
    r=run(bad,case);assert r.returncode==23,(name,r.stdout,r.stderr)
    negative.append(dict(name=name,exit=r.returncode,stderr=r.stderr))
old='float sharpness=detail.sharpness;';assert main.count(old)==1
bad=build('ui-applied-value',text,main.replace(old,'float sharpness=NativeFsr3Sharpness();'))
r=run(bad,'menu');assert r.returncode==23,r.stdout+r.stderr
negative.append(dict(name='ui-applied-value',exit=r.returncode,stderr=r.stderr))
block='  active.SetDetails(requested,details);\n  active.mode.store(requested);\n  active.scale.store(scale);'
assert text.count(block)==1
bad_text=text.replace(block,'').replace('  if (reconfigure && prepare_scene) {',block+'\n  if (reconfigure && prepare_scene) {')
bad=build('publish-before-prepare',bad_text);r=run(bad,'deferred');assert r.returncode==23,r.stdout+r.stderr
negative.append(dict(name='publish-before-prepare',exit=r.returncode,stderr=r.stderr))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,production_settings_functions=True,
    cvar_storage_and_registry_boundary_mocked=True,positive=positive,negative=negative,source_sha256=hashlib.sha256(text.encode()).hexdigest(),
    production_graphics_menu_class=True,header_sha256=hashlib.sha256(header.encode()).hexdigest(),
    scope='actual frame publication/getters and GraphicsMenu class; synchronized mock registry and ImGui boundary, real host threads/atomics; not TSan or real UI/game/GPU/FPS'),indent=2))
