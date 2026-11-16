#include "gpu_native/settings_snapshot.h"
#include "settings_registry.h"
#include <rex/cvar.h>
#include <cstdio>
#include <cstdlib>
using namespace legodimensions::settings;
using namespace legodimensions::gpu_native;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#x); std::exit(23); } } while (0)
int main(){
 unsigned cases=0;SettingsRevision revision{17,NativeSettingsDefaults(),{}};NativeSettingsOverrides overrides;
 std::string error="old error";auto value=DecodeNativeRenderSettings(revision,overrides,error);
 CHECK(value&&error.empty()&&value->revision==17&&value->fsr3.mode==3&&value->fsr3.scale==150);
 CHECK(!value->fullscreen&&!value->vsync&&value->depth_of_field&&!value->fsr1&&value->fps_limit==240&&value->anisotropy==-1);
 CHECK(value->show_fps);++cases;
 CHECK(value->bloom&&value->cel_shading==NativeCelShadingMode::kOriginal);++cases;
 for(auto [text,mode]:std::vector<std::pair<std::string,NativeCelShadingMode>>{
   {"original",NativeCelShadingMode::kOriginal},{"everywhere",NativeCelShadingMode::kEverywhere},
   {"off",NativeCelShadingMode::kOff}})for(bool bloom:{false,true}){
  auto effects=revision;effects.values["native_bloom"]=bloom?"true":"false";
  effects.values["native_cel_shading"]=text;
  auto decoded=DecodeNativeRenderSettings(effects,overrides,error);
  CHECK(decoded&&decoded->bloom==bloom&&decoded->cel_shading==mode);
  effects.values["native_cel_shading"]="invalid";effects.values["native_bloom"]="invalid";
  CHECK(decoded->bloom==bloom&&decoded->cel_shading==mode);++cases;
 }
 // Hiding the meter is a copied presentation choice, independent of the cap.
 auto visible=*value;auto hidden_revision=revision;
 hidden_revision.values["native_show_fps"]="false";
 auto hidden=DecodeNativeRenderSettings(hidden_revision,overrides,error);
 CHECK(hidden&&!hidden->show_fps&&hidden->fps_limit==visible.fps_limit&&visible.show_fps);
 hidden_revision.values["native_show_fps"]="true";
 CHECK(!hidden->show_fps);++cases;
 CHECK(value->fsr1_passes==4&&value->fsr1_sharpness_reduction==.2f&&value->fsr3.details.sharpen&&value->fsr3.details.sharpness==.2f);++cases;
 // Differential FSR decoding against the existing requested-settings path and
 // real SDK registry. Other native flags need not be registered: decoder data
 // must come from its immutable proposal, rather than global CVar reads.
 for(bool enabled:{false,true})for(auto quality: {"native","quality","balanced","performance","ultra_performance","legacy_unknown"})
  for(int scale:{0,20,100,201,300,400,-1}){
   revision.values["native_fsr3"]=enabled?"true":"false";revision.values["native_fsr3_quality"]=quality;
   revision.values["native_resolution_scale"]=std::to_string(scale);
   CHECK(rex::cvar::SetFlagByName("native_fsr3",enabled?"true":"false"));
   CHECK(rex::cvar::SetFlagByName("native_fsr3_quality",quality));
   CHECK(rex::cvar::SetFlagByName("native_resolution_scale",std::to_string(scale)));
   const auto expected=CaptureNativeFsr3SettingsRequest();value=DecodeNativeRenderSettings(revision,overrides,error);
   CHECK(value&&value->fsr3.mode==expected.mode&&value->fsr3.scale==expected.scale);
   CHECK(value->fsr3.details.sharpen==expected.details.sharpen&&value->fsr3.details.sharpness==expected.details.sharpness&&value->fsr3.details.mip_bias_adjustment==expected.details.mip_bias_adjustment);++cases;
  }
 revision.values=NativeSettingsDefaults();revision.values["present_effect"]="fsr";revision.values["fullscreen"]="true";
 revision.values["depth_of_field"]="false";revision.values["native_fps_limit"]="0";
 overrides.fsr3=false;overrides.vsync=true;overrides.fps_limit=75;overrides.fsr3_quality="native";
 overrides.inherited_anisotropy=5;overrides.skip_pixel_shaders="ABC,DEF";
 value=DecodeNativeRenderSettings(revision,overrides,error);
 CHECK(value&&value->fsr3.mode==0&&value->fullscreen&&value->vsync&&!value->depth_of_field&&value->fsr1);
 CHECK(value->fps_limit==75&&value->anisotropy==5&&value->skip_pixel_shaders=="ABC,DEF");++cases;
 overrides.native_720p=true;value=DecodeNativeRenderSettings(revision,overrides,error);
 CHECK(value&&value->fsr3.mode==2&&value->fsr3.scale==100);++cases;
 auto sealed=*value;overrides.skip_pixel_shaders="changed";revision.values["fullscreen"]="false";
 CHECK(sealed.fullscreen&&sealed.skip_pixel_shaders=="ABC,DEF");++cases;
 revision.values=NativeSettingsDefaults();overrides={};
 for(int selected:{-3,-2,-1,0,1,2,3,4,5,6})for(int inherited:{-1,0,5,8}){
  revision.values["native_anisotropic_override"]=std::to_string(selected);overrides.inherited_anisotropy=inherited;
  value=DecodeNativeRenderSettings(revision,overrides,error);
  const int expected=selected>=-1&&selected<=5?selected:inherited>=0&&inherited<=5?inherited:-1;
  CHECK(value&&value->anisotropy==expected);++cases;
 }
 revision.values=NativeSettingsDefaults();overrides={};
 for(auto[key,input,expected]:std::vector<std::tuple<std::string,std::string,int>>{
  {"native_fps_limit","-1",0},{"native_fps_limit","2000",2000},{"present_fsr_max_upsampling_passes","0",1},
  {"present_fsr_max_upsampling_passes","8",4}}){
  auto changed=revision;changed.values[key]=input;value=DecodeNativeRenderSettings(changed,overrides,error);
  CHECK(value&&(key=="native_fps_limit"?value->fps_limit:value->fsr1_passes)==expected);++cases;
 }
 for(int cap:{-1,0,60,1001}){overrides.fps_limit=cap;value=DecodeNativeRenderSettings(revision,overrides,error);
  CHECK(value&&value->fps_limit==std::clamp(cap,0,1000));++cases;}
 overrides={};
 for(const auto& spec:NativeSettingsCatalog()){
  auto changed=revision;changed.values.erase(std::string(spec.key));CHECK(!DecodeNativeRenderSettings(changed,overrides,error)&&!error.empty());++cases;
 }
 for(auto[key,input]:std::vector<std::pair<std::string,std::string>>{
  {"fullscreen","yes"},{"depth_of_field","0"},{"native_vsync","1"},{"native_fsr3","falsex"},{"native_show_fps","yes"},
  {"native_bloom","1"},{"native_cel_shading","on"},{"native_cel_shading","Original"},
  {"native_fsr3_sharpness","nan"},{"native_fsr3_mip_bias","inf"},{"native_fps_limit","240x"},
  {"native_resolution_scale","2147483648"},{"present_fsr_max_upsampling_passes","1.2"},
  {"present_fsr_sharpness_reduction","Infinity"}}){
  auto changed=revision;changed.values[key]=input;CHECK(!DecodeNativeRenderSettings(changed,overrides,error)&&!error.empty());++cases;
 }
 std::printf("PASS %u native snapshot cases: all catalog controls, real-SDK FSR differential, legacy fallback/clamps, explicit overrides, copied auxiliary data and malformed/missing payload rejection\n",cases);
}
