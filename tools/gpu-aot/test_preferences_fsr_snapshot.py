"""Verify preference restoration refreshes the real native FSR/extent snapshot before device setup."""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];pkg=root.parent;src=root/'rexlego/src';sdk=pkg/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    deps=[pkg/'build/worlds-candidate-v18'/name for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']]
    for f in deps:shutil.copy2(f,out/f.name)
    fixture=out/'snapshot.cpp';fixture.write_text(r'''
#include "settings_preferences.h"
#include "gpu_native/fsr3_settings.h"
#include <rex/cvar.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <windows.h>
using namespace legodimensions::settings;
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==2);Values backing;std::vector<std::string> owned;
 for(auto&spec:NativeSettingsCatalog()){
  const std::string key(spec.key);if(!rex::cvar::GetFlagByName(key).empty())continue;backing[key]=spec.default_value;
  rex::cvar::FlagEntry e;e.name=key;e.default_value=spec.default_value;e.type=spec.type==ConfigType::kBoolean?rex::cvar::FlagType::Boolean:spec.type==ConfigType::kInteger?rex::cvar::FlagType::Int32:spec.type==ConfigType::kFloat?rex::cvar::FlagType::Double:rex::cvar::FlagType::String;
  e.getter=[&,key]{return backing.at(key);};e.setter=[&,key](auto value){backing[key]=value;return true;};CHECK(rex::cvar::RegisterFlag(std::move(e)).has_value());owned.push_back(key);
 }
 CHECK(rex::cvar::SetFlagByName("native_fsr3","false")&&rex::cvar::SetFlagByName("native_resolution_scale","150"));
 // Warm the real renderer's cached state as an early overlay could do.
 CHECK(!NativeFsr3Enabled()&&NativeResolutionScale()==150);CHECK(NativeSceneRenderSize().width==1920);
 auto preferences=NativePreferencesPath(std::filesystem::path(argv[1]));
 {std::ofstream f(preferences);f<<"native_resolution_scale=300\nnative_fsr3=false\nnative_fsr3_sharpness=0.7\n";CHECK(f.good());}
 auto backend=MakeNativeRegistryBackend(preferences);CHECK(LoadNativePreferences(preferences,backend).ok);
 CHECK(NativeRequestedResolutionScale()==300&&NativeResolutionScale()==150); // Still old until the explicit pre-device refresh.
 const bool changed = ApplyNativeFsr3Settings();
 CHECK(changed&&NativeResolutionScale()==300&&!NativeFsr3Enabled());auto manual=NativeSceneRenderSize();CHECK(manual.width==3840&&manual.height==2160);
 CHECK(std::abs(NativeFsr3Sharpness()-0.7f)<0.000001f);
 auto scene=NativeConfiguredSceneExtent(1280,720),asset=NativeConfiguredSceneExtent(512,512);
 CHECK(scene.width==3840&&scene.height==2160&&asset.width==512&&asset.height==512);
 {std::ofstream f(preferences);f<<"native_fsr3=true\nnative_fsr3_quality='performance'\nnative_fsr3_mip_bias=0.3\n";CHECK(f.good());}
 CHECK(LoadNativePreferences(preferences,backend).ok);CHECK(!NativeFsr3Enabled());CHECK(ApplyNativeFsr3Settings());
 CHECK(NativeFsr3Enabled());auto upscaled=NativeSceneRenderSize();CHECK(upscaled.width==960&&upscaled.height==540&&NativeResolutionScale()==300);
 CHECK(std::abs(NativeFsr3MipBias()+1.7f)<0.000001f);
 for(auto&key:owned)rex::cvar::UnregisterFlag(key);
 std::puts("PASS: real native snapshot refresh after durable preference restore; cached/manual 4K and FSR3 performance input extents, details and unscaled asset dimensions");
}
''')
    env={k:v for k,v in os.environ.items() if not k.upper().startswith(('LEGO_NATIVE_','REX_NATIVE_'))}
    sources=[src/name for name in ['settings_preferences.cpp','settings_registry.cpp','settings_config.cpp','settings_transaction.cpp','gpu_native/fsr3_settings.cpp']]
    def run(label,source):
        exe=out/(label+'.exe');case=out/(label+'-files');case.mkdir(exist_ok=False)
        command=[a.compiler,'-std=c++23','-O2','-D_CRT_SECURE_NO_WARNINGS','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-Wno-deprecated-literal-operator',
                 '-I'+str(src),'-I'+str(sdk/'include'),str(source)]+[str(f) for f in sources]+[str(sdk/'lib/rexruntime.lib'),'-o',str(exe)]
        r=subprocess.run(command,capture_output=True,text=True,timeout=90);(out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        r=subprocess.run([str(exe),str(case)],capture_output=True,text=True,env=env,timeout=15);(out/(label+'.log')).write_text(r.stdout+r.stderr);return r
    positive=run('production',fixture);positive.check_returncode();print(positive.stdout,end='')
    text=fixture.read_text();old='const bool changed = ApplyNativeFsr3Settings();';assert text.count(old)==1
    mutant=out/'skip-pre-device-refresh.cpp';mutant.write_text(text.replace(old,'const bool changed = false;'))
    negative=run('skip-pre-device-refresh',mutant);assert negative.returncode==23,(negative.returncode,negative.stdout,negative.stderr)
    app=src/'legodimensions_app.h';hook=app.read_text().split('  void OnPreLaunchModule() override {',1)[1].split('\n#endif',1)[0]
    assert hook.index('LoadNativePreferences(')<hook.index('ApplyNativeFsr3Settings()')<hook.index('HostDevice::Create(window())')
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,output=positive.stdout,
        sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources+deps+[fixture,app,src/'settings_preferences.h',src/'settings_registry.h',src/'gpu_native/fsr3_settings.h']},
        negative=dict(name='skip-pre-device-refresh',exit=negative.returncode,error=negative.stderr),
        scope='Real native fsr3_settings.cpp, real SDK registry and production preference restore. CPU state/surface extent selection only; no GPU device/resource allocation, FSR dispatch, actual window, title launch or visual acceptance.'),indent=2))
if __name__=='__main__':main()
