"""Execute production settings with a minimal cvar boundary, without the title."""
import argparse
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
include=a.output/'include/rex';include.mkdir(parents=True,exist_ok=True)
(include/'cvar.h').write_text('''#pragma once
#include <string>
#include <string_view>
namespace rex::cvar {enum class Lifecycle{kRequiresRestart};}
template<class T> struct CvarFixture {T value;CvarFixture& lifecycle(rex::cvar::Lifecycle){return *this;}CvarFixture& range(double,double){return *this;}};
extern CvarFixture<bool> native_fsr3,native_fsr3_sharpen;
extern CvarFixture<std::string> native_fsr3_quality;
extern CvarFixture<double> native_fsr3_sharpness,native_fsr3_mip_bias;
namespace rex::cvar {
inline std::string GetFlagByName(std::string_view name){return name=="native_fsr3_quality"?native_fsr3_quality.value:(native_fsr3.value?"true":"false");}
template<class T>T Query(std::string_view);
template<> inline bool Query<bool>(std::string_view name){return name=="native_fsr3_sharpen"?native_fsr3_sharpen.value:native_fsr3.value;}
template<> inline double Query<double>(std::string_view name){return name=="native_fsr3_sharpness"?native_fsr3_sharpness.value:native_fsr3_mip_bias.value;}
}
#define REXCVAR_DEFINE_BOOL(n,v,...) CvarFixture<bool> n=CvarFixture<bool>{v}
#define REXCVAR_DEFINE_STRING(n,v,...) CvarFixture<std::string> n=CvarFixture<std::string>{v}
#define REXCVAR_DEFINE_DOUBLE(n,v,...) CvarFixture<double> n=CvarFixture<double>{v}
#define REXCVAR_GET(n) (n.value)
''')
cpp=a.output/'settings.cpp';exe=a.output/'settings.exe'
cpp.write_text('''#include <rex/cvar.h>
#include "gpu_native/fsr3_settings.h"
#include <cassert>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <algorithm>
extern CvarFixture<bool> native_fsr3,native_fsr3_sharpen;
extern CvarFixture<std::string> native_fsr3_quality;
extern CvarFixture<double> native_fsr3_sharpness,native_fsr3_mip_bias;
using namespace legodimensions::gpu_native;
int main(int argc,char** argv){assert(argc==4);
 assert(NativeFsr3Enabled()==bool(std::atoi(argv[3])));
 auto size=NativeFsr3RenderSize();assert(size.width==unsigned(std::atoi(argv[1]))&&size.height==unsigned(std::atoi(argv[2])));
 auto scene=NativeConfiguredSceneExtent(1280,720,true);
 assert(scene.logical_width==1280&&scene.logical_height==720);
 assert(scene.width==(NativeFsr3Enabled()?size.width:1920)&&scene.height==(NativeFsr3Enabled()?size.height:1080));
 auto selected=NativeConfiguredSceneExtent(1280,720,!Native720PRequested());
 assert(selected.width==(NativeFsr3Enabled()?size.width:(Native720PRequested()?1280:1920)));
 auto other=NativeConfiguredSceneExtent(512,256,true);assert(other.width==512&&other.height==256);
 assert(NativeFsr3Sharpness()==.2f&&NativeFsr3Sharpening());
 native_fsr3_sharpness.value=-10;
 assert(NativeFsr3Sharpness()==.2f&&NativeFsr3RequestedDetails().sharpness==0);
 assert(!ApplyNativeFsr3Settings());assert(NativeFsr3Sharpness()==0);
 native_fsr3_sharpness.value=10;assert(NativeFsr3Sharpness()==0);
 assert(!ApplyNativeFsr3Settings());assert(NativeFsr3Sharpness()==1);
 native_fsr3_sharpness.value=std::numeric_limits<double>::quiet_NaN();
 assert(!ApplyNativeFsr3Settings());assert(NativeFsr3Sharpness()==.2f);
 native_fsr3_sharpen.value=false;assert(NativeFsr3Sharpening()&&!NativeFsr3RequestedDetails().sharpen);
 assert(!ApplyNativeFsr3Settings());assert(!NativeFsr3Sharpening());
 const float bias=NativeFsr3Enabled()?std::clamp(std::log2(float(size.width)/1920)-1.f,-3.f,0.f):0.f;
 assert(std::abs(NativeFsr3MipBias()-bias)<.0001f);
 native_fsr3_mip_bias.value=std::numeric_limits<double>::quiet_NaN();assert(!ApplyNativeFsr3Settings());assert(std::abs(NativeFsr3MipBias()-bias)<.0001f);
 native_fsr3_mip_bias.value=1;assert(std::abs(NativeFsr3MipBias()-bias)<.0001f);
 assert(NativeFsr3RequestedDetails().mip_bias_adjustment==1&&!ApplyNativeFsr3Settings());
 if(NativeFsr3Enabled())assert(std::abs(NativeFsr3MipBias()-std::clamp(std::log2(float(size.width)/1920),-3.f,0.f))<.0001f);
 native_fsr3_quality.value="ultra_performance";native_fsr3.value=!NativeFsr3Enabled();
 assert(NativeFsr3RenderSize().width==size.width); // Stable within a frame.
 const bool changed=ApplyNativeFsr3Settings();
 assert(!ApplyNativeFsr3Settings()); // Idempotent next-frame commit.
 const auto active=NativeFsr3RenderSize();
 const auto* qualityOverride=std::getenv("LEGO_NATIVE_FSR3_QUALITY");
 const auto* enabledOverride=std::getenv("LEGO_NATIVE_FSR3");
 const bool forcedQuality=(qualityOverride&&*qualityOverride)||Native720PRequested();
 const bool forcedEnabled=enabledOverride&&*enabledOverride;
 assert(active.width==(forcedQuality?size.width:640));
 if(!forcedEnabled)assert(NativeFsr3Enabled()!=bool(std::atoi(argv[3])));
 if(!forcedQuality||!forcedEnabled)assert(changed);
}
''')
subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(a.output/'include'),'-I'+str(root/'rexlego/src'),str(cpp),
               str(root/'rexlego/src/gpu_native/fsr3_settings.cpp'),'-o',str(exe)],check=True,timeout=45)
base=os.environ.copy()
for key in ['LEGO_NATIVE_FSR3','LEGO_NATIVE_FSR3_QUALITY','LEGO_NATIVE_720P']:base.pop(key,None)
cases=[({},1280,720,1),({'LEGO_NATIVE_FSR3':'0'},1280,720,0),({'LEGO_NATIVE_FSR3_QUALITY':'native'},1920,1080,1),
       ({'LEGO_NATIVE_FSR3_QUALITY':'balanced'},1129,635,1),({'LEGO_NATIVE_FSR3_QUALITY':'performance'},960,540,1),
       ({'LEGO_NATIVE_FSR3_QUALITY':'ultra_performance'},640,360,1),({'LEGO_NATIVE_FSR3_QUALITY':'invalid'},1280,720,1),
       ({'LEGO_NATIVE_FSR3_QUALITY':'performance','LEGO_NATIVE_720P':'1'},1280,720,1),
       ({'LEGO_NATIVE_FSR3':'','LEGO_NATIVE_FSR3_QUALITY':'','LEGO_NATIVE_720P':''},1280,720,1),
       ({'LEGO_NATIVE_FSR3_QUALITY':'performance','LEGO_NATIVE_720P':'0'},960,540,1)]
for env,w,h,enabled in cases:
    subprocess.run([str(exe.resolve()),str(w),str(h),str(enabled)],env=base|env,check=True,timeout=10)
print('PASS: production FSR settings, next-frame mode/detail changes, requested UI values, overrides, idempotence and clamps; cvar storage is a fixture')
