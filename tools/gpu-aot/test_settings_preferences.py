"""Test durable native preferences through real SDK restarts and private config generation.

Only the reviewed config-generation fragment is executed; no launcher or title.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');p.add_argument('--powershell',default='pwsh');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];pkg=root.parent;src=root/'rexlego/src';sdk=pkg/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    runtime=pkg/'build/worlds-candidate-v18/rexruntime.dll';ffx=runtime.with_name('amd_fidelityfx_dx12.dll')
    for f in [runtime,ffx]:shutil.copy2(f,out/f.name)
    launcher=pkg/'build/Run-Candidate.ps1';text=launcher.read_text();begin=text.index('$taskConfig=Get-Content -Raw');end=text.index('$taskOverrides=@{',begin)
    fragment=text[begin:end].strip();assert fragment.count('[IO.File]::WriteAllText')==2 and 'Start-Process' not in fragment
    config_script=out/'generate-private-config.ps1';config_script.write_text('''param([string]$FixtureRoot,[string]$NativeDir)
$ErrorActionPreference='Stop'
$taskRoot=$FixtureRoot
$taskProfileBuildRoot=Join-Path $FixtureRoot 'build'
$taskRun=Join-Path $taskProfileBuildRoot ('runs/'+$NativeDir)
$taskUser=Join-Path $taskProfileBuildRoot 'candidate-user-data'
$FpsLimit=0
New-Item -ItemType Directory -Force $taskRun,$taskUser,(Join-Path $taskProfileBuildRoot $NativeDir) | Out-Null
'''+fragment.replace('$PSScriptRoot','$taskProfileBuildRoot')+'\n',encoding='utf-8')
    fixture=out/'preferences.cpp';fixture.write_text(r'''
#include "settings_preferences.h"
#include "settings_service.h"
#include <rex/cvar.h>
#include <toml++/toml.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <windows.h>
using namespace legodimensions::settings;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
std::string read(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);std::ostringstream s;s<<f.rdbuf();return s.str();}
void put(const std::filesystem::path&p,const std::string&s){std::ofstream f(p,std::ios::binary);f<<s;CHECK(f.good());}
struct Registry{
 Values backing;std::vector<std::string> owned;bool fail=false,partial=false,throws=false,fail_restore=false;
 Registry(){for(const auto& spec:NativeSettingsCatalog()){
  auto key=std::string(spec.key);if(!rex::cvar::GetFlagByName(key).empty())continue;backing[key]=spec.default_value;
  auto type=spec.type;rex::cvar::FlagEntry e;e.name=key;e.type=type==ConfigType::kString?rex::cvar::FlagType::String:type==ConfigType::kBoolean?rex::cvar::FlagType::Boolean:type==ConfigType::kFloat?rex::cvar::FlagType::Double:rex::cvar::FlagType::Int32;
  e.default_value=spec.default_value;e.getter=[&,key,type]{return type==ConfigType::kFloat?std::to_string(std::stod(backing.at(key))):backing.at(key);};
  e.setter=[&,key,type](std::string_view v){bool reject=key=="native_fsr3"&&((fail&&v=="true")||(fail_restore&&v=="false"));
   if(!reject||partial){if(type==ConfigType::kFloat)backing[key]=std::to_string(std::stod(std::string(v)));else backing[key]=v;}
   if(reject&&throws)throw std::runtime_error("Injected setter failure");return !reject;};
  CHECK(rex::cvar::RegisterFlag(std::move(e)).has_value());owned.push_back(key);
 }
 if(rex::cvar::GetFlagByName("mods").empty()){
  backing["mods"]="default";rex::cvar::FlagEntry e;e.name="mods";e.type=rex::cvar::FlagType::String;e.default_value="default";
  e.getter=[&]{return backing.at("mods");};e.setter=[&](auto v){backing["mods"]=v;return true;};CHECK(rex::cvar::RegisterFlag(std::move(e)).has_value());owned.push_back("mods");
 }}
 ~Registry(){for(auto&k:owned)rex::cvar::UnregisterFlag(k);}
};
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==4);Registry registry;
 auto dir=std::filesystem::path(argv[2]);auto boot=std::filesystem::path(argv[3]);auto preferences=NativePreferencesPath(dir/"build/candidate-user-data");
 const std::string mode=argv[1];if(mode=="disabled")CHECK(rex::cvar::SetFlagFromCommandLine("native_settings_load","false"));
 auto reset=[&]{for(auto&spec:NativeSettingsCatalog())rex::cvar::ResetToDefault(spec.key);rex::cvar::LoadConfig(boot);};reset();rex::cvar::FinalizeInit();
 auto backend=MakeNativeRegistryBackend(preferences);const auto bootstrap=read(boot);
 CHECK(backend.read().at("native_fps_limit")=="0"&&backend.read().at("native_resolution_scale")=="150"&&backend.read().at("native_fsr3")=="false");
 const auto mods=rex::cvar::GetFlagByName("mods"),game_path=rex::cvar::GetFlagByName("game_data_root");
 if(mode=="seed"){
  CHECK(!std::filesystem::exists(preferences));auto empty=LoadNativePreferences(preferences,backend);CHECK(empty.ok&&empty.applied.empty()&&!std::filesystem::exists(preferences));
  Transaction t(backend,NativeSettingsConfirmationKeys());CHECK(t.Open());CHECK(t.Edit("native_fps_limit","144")&&t.Edit("native_resolution_scale","300")&&t.Edit("fullscreen","true"));
  CHECK(t.Apply(Clock::now())&&t.phase()==Phase::kPreview&&!std::filesystem::exists(preferences));CHECK(t.Keep(Clock::now()));
  auto saved=toml::parse(read(preferences));CHECK(saved["native_fps_limit"].value<int>()==144&&saved["native_resolution_scale"].value<int>()==300);
  CHECK(saved["fullscreen"].value<bool>()==true&&saved["native_fsr3"].value<bool>()==false); // unchanged, approved FSR choice persists too
  CHECK(!saved.contains("mods")&&!saved.contains("game_data_root"));CHECK(read(boot)==bootstrap);
  std::puts("PASS seed: approved native snapshot persisted separately; executable config unchanged");return 0;
 }
 if(mode=="restore"){
  const auto saved=read(preferences);CHECK(rex::cvar::SetFlagByName("native_fsr3","true")); // Changed launch/migration default must not replace approved false.
  const auto restored=LoadNativePreferences(preferences,backend);CHECK(restored.ok&&!restored.applied.empty());auto values=backend.read();
  CHECK(values.at("native_fps_limit")=="144"&&values.at("native_resolution_scale")=="300"&&values.at("fullscreen")=="true"&&values.at("native_fsr3")=="false");
  CHECK(read(preferences)==saved&&read(boot)==bootstrap&&rex::cvar::GetFlagByName("mods")==mods&&rex::cvar::GetFlagByName("game_data_root")==game_path);
  std::puts("PASS restore: fresh process restores native choices after launcher config regeneration; paths/mods unchanged");return 0;
 }
 if(mode=="disabled"){
  const auto saved=read(preferences);auto result=LoadNativePreferences(preferences,backend);CHECK(result.ok&&result.applied.empty());
  CHECK(backend.read().at("native_fps_limit")=="0"&&read(preferences)==saved);std::puts("PASS explicit comparison disable: launcher defaults retained, preferences untouched");return 0;
 }
 if(mode=="whitelist"){
  put(preferences,"native_fps_limit=120\nmods='ignored-other-mod'\ngame_data_root='Z:/never-apply'\nnative_settings_load=false\n");
  auto result=LoadNativePreferences(preferences,backend);CHECK(result.ok&&backend.read().at("native_fps_limit")=="120");
  CHECK(rex::cvar::GetFlagByName("mods")==mods&&rex::cvar::GetFlagByName("game_data_root")==game_path&&NativePreferencesEnabled());
  std::puts("PASS whitelist: unrelated title/path/loader controls ignored");return 0;
 }
 CHECK(mode=="edges");unsigned cases=0;const auto initial=backend.read();
 for(const auto&bad:std::vector<std::string>{"native_fps_limit='144'","native_fsr3='true'","native_resolution_scale=201","native_resolution_scale=301",
   "native_fps_limit=-1","native_fps_limit=9999999999999","native_fsr3_quality='bogus'","native_fsr3_sharpness=nan","native_fsr3_sharpness=1.0000001",
   "native_fsr3_sharpness='0.5'","native_vsync=1","broken=[not toml"}){
  const auto content=(bad.starts_with("native_vsync")?"native_fps_limit=120\n":"native_vsync=true\n")+bad+"\n";put(preferences,content);
  auto result=LoadNativePreferences(preferences,backend);CHECK(!result.ok&&!result.recovery_failed&&backend.read()==initial&&read(preferences)==content);++cases;
 }
 for(bool partial:{false,true})for(bool throws:{false,true}){
  const std::string content="native_fps_limit=144\nnative_fsr3=true\nnative_resolution_scale=300\n";put(preferences,content);registry.fail=true;registry.partial=partial;registry.throws=throws;
  auto result=LoadNativePreferences(preferences,backend);registry.fail=false;registry.throws=false;
  CHECK(!result.ok&&!result.recovery_failed&&result.applied.empty()&&backend.read()==initial&&read(preferences)==content);++cases;
 }
 put(preferences,"native_fps_limit=144\nnative_fsr3=true\n");registry.fail=true;registry.partial=true;registry.fail_restore=true;
 auto recovery=LoadNativePreferences(preferences,backend);registry.fail=registry.fail_restore=false;CHECK(!recovery.ok&&recovery.recovery_failed);reset();CHECK(backend.read()==initial);++cases;
 put(preferences,"native_fps_limit=120\nmods=['ignored']\ngame_data_root='Z:/never-apply'\nnative_settings_load=false\n[unrelated]\nwhatever=[1,2]\n");
 const auto other=read(preferences);auto filtered=LoadNativePreferences(preferences,backend);CHECK(filtered.ok&&backend.read().at("native_fps_limit")=="120"&&NativePreferencesEnabled());
 CHECK(rex::cvar::GetFlagByName("mods")==mods&&rex::cvar::GetFlagByName("game_data_root")==game_path&&read(preferences)==other);reset();++cases;
 put(preferences,"native_fsr3_sharpness=1\n");CHECK(LoadNativePreferences(preferences,backend).ok&&backend.read().at("native_fsr3_sharpness")=="1");reset();++cases;
 put(preferences,"native_fps_limit=144\nnative_resolution_scale=300\n");PreferencesResult off_thread;
 std::thread worker([&]{off_thread=LoadNativePreferences(preferences,backend);});worker.join();CHECK(!off_thread.ok&&backend.read()==initial);++cases;
 const auto read_impl=backend.read;auto unreadable=backend;unsigned reads=0;unreadable.read=[&]{if(++reads==2)throw std::runtime_error("Readback unavailable");return read_impl();};
 CHECK(!LoadNativePreferences(preferences,unreadable).ok&&backend.read()==initial);++cases;
 auto drift=backend;reads=0;drift.read=[&]{auto v=read_impl();if(++reads==2)v["native_fps_limit"]="999";return v;};
 CHECK(!LoadNativePreferences(preferences,drift).ok&&backend.read()==initial);++cases;
 // Explicit SDK command-line and native experiment overrides retain their
 // current values, and a separate commit must not overwrite stored choices.
 reset();CHECK(rex::cvar::SetFlagFromCommandLine("native_fps_limit","77"));auto cli=LoadNativePreferences(preferences,backend);
 CHECK(cli.ok&&cli.overridden.size()==1&&backend.read().at("native_fps_limit")=="77"&&backend.read().at("native_resolution_scale")=="300");
 CHECK(!backend.normalize("native_fps_limit","60").valid);std::string error;CHECK(backend.write("depth_of_field","false"));
 CHECK(backend.persist({{"depth_of_field","false"}},error));CHECK(toml::parse(read(preferences))["native_fps_limit"].value<int>()==144);reset();++cases;
 put(preferences,"native_fps_limit=144\nnative_fsr3=true\n");CHECK(_putenv_s("LEGO_NATIVE_FSR3","0")==0);auto env=LoadNativePreferences(preferences,backend);
 CHECK(env.ok&&env.overridden.size()==1&&backend.read().at("native_fsr3")=="false");CHECK(_putenv_s("LEGO_NATIVE_FSR3","")==0);reset();++cases;
 // Exercise the real SDK environment-source priority too.
 CHECK(_putenv_s("REX_NATIVE_FPS_LIMIT","88")==0);rex::cvar::ApplyEnvironment();auto sdk_env=LoadNativePreferences(preferences,backend);
 CHECK(sdk_env.ok&&backend.read().at("native_fps_limit")=="88"&&!backend.normalize("native_fps_limit","60").valid);CHECK(_putenv_s("REX_NATIVE_FPS_LIMIT","")==0);reset();++cases;
 // Native startup errors remain available until a native dialog acknowledges.
 std::vector<std::function<void()>> queue;QueuedSettings service(backend,[&](auto f){queue.push_back(std::move(f));return true;},NativeSettingsDefaults(),NativeSettingsConfirmationKeys());
 CHECK(service.SetStartupErrorFromUI("Unreadable saved settings"));auto warning=service.Submit({CommandKind::kOpen});queue.back()();queue.clear();CHECK(warning.get().startup_error=="Unreadable saved settings");
 auto ack=service.Submit({CommandKind::kAcknowledgeStartupError});queue.back()();queue.clear();CHECK(ack.get().startup_error.empty()&&service.ShutdownFromUI().ok);++cases;
 auto unicode_dir=dir/std::filesystem::path(u8"Игрок settings");std::filesystem::create_directory(unicode_dir);auto unicode=NativePreferencesPath(unicode_dir);
 std::vector<ConfigChange> patch{{"native_fps_limit",ConfigType::kInteger,"166"}};CHECK(SaveConfigPatch(unicode,patch,error));auto unicode_backend=MakeNativeRegistryBackend(unicode);
 CHECK(LoadNativePreferences(unicode,unicode_backend).ok&&unicode_backend.read().at("native_fps_limit")=="166");++cases;
 CHECK(read(boot)==bootstrap&&rex::cvar::GetFlagByName("mods")==mods&&rex::cvar::GetFlagByName("game_data_root")==game_path);
 std::printf("PASS %u preference edge cases: whole-file validation, rollback/recovery, read failures, unknown keys, types, overrides, owner thread, native startup notice and Unicode\n",cases);
}
''',encoding='utf-8')
    binary=out/'preferences.exe';command=[a.compiler,'-std=c++23','-O2','-D_CRT_SECURE_NO_WARNINGS','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-Wno-deprecated-literal-operator',
        '-I'+str(src),'-I'+str(sdk/'include'),str(fixture)]+[str(src/(name+'.cpp')) for name in ['settings_preferences','settings_registry','settings_service','settings_config','settings_transaction']]+[str(sdk/'lib/rexruntime.lib'),'-o',str(binary)]
    compiled=subprocess.run(command,capture_output=True,text=True,timeout=90);(out/'compile.log').write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
    env={k:v for k,v in os.environ.items() if not k.upper().startswith(('LEGO_NATIVE_','REX_NATIVE_'))}
    case=out/'private-launches';(case/'runtime').mkdir(parents=True,exist_ok=False)
    template="native_fps_limit = 240\nnative_resolution_scale = 150\nnative_fsr3 = true\nfullscreen = true\nmods = 'AllWorlds,QuickStartup'\ngame_data_root = 'E:/no-title-is-loaded'\nuser_data_root = 'placeholder'\nlog_file = 'placeholder'\nupdates_check = false\n"
    (case/'runtime/legodimensions.toml').write_text(template)
    outputs=[]
    def generate(folder,case_root=case):
        r=subprocess.run([a.powershell,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(config_script),'-FixtureRoot',str(case_root),'-NativeDir',folder],capture_output=True,text=True,timeout=20)
        (out/(folder+'-config.log')).write_text(r.stdout+r.stderr);r.check_returncode();return case/'build'/folder/'legodimensions.toml'
    def run(mode,boot):
        r=subprocess.run([str(binary),mode,str(case),str(boot)],env=env,capture_output=True,text=True,timeout=20)
        (out/(mode+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();outputs.append(r.stdout)
    boot=generate('first-exe');run('seed',boot)
    preferences=case/'build/candidate-user-data/native-settings.toml';stored=preferences.read_bytes()
    # Regenerate both run and executable configs exactly as the launcher does,
    # and move to another executable directory sharing the resolved user data.
    boot=generate('second-exe');assert preferences.read_bytes()==stored;run('restore',boot)
    assert preferences.read_bytes()==stored;run('disabled',boot);assert preferences.read_bytes()==stored;run('edges',boot);run('whitelist',boot)
    app=src/'legodimensions_app.h';app_text=app.read_text();start=app_text.index('  void OnPreLaunchModule() override {');end=app_text.index('\n#endif',start);hook=app_text[start:end]
    assert 'NativePreferencesPath(paths.user_data_root)' in app_text
    assert hook.index('LoadNativePreferences(')<hook.index('ApplyNativeFsr3Settings()')<hook.index('HostDevice::Create(window())')
    assert 'SetStartupErrorFromUI(preferences.error)' in hook
    negative=[]
    pref_source=src/'settings_preferences.cpp';pref_text=pref_source.read_text();registry_source=src/'settings_registry.cpp';registry_text=registry_source.read_text()
    controls=[
        ('wrong-store-location',pref_source,'return user_data_root / "native-settings.toml";','return user_data_root.parent_path() / "first-exe/legodimensions.toml";','seed'),
        ('partial-before-validation',pref_source,'proposal.emplace(spec.key, std::move(normalized.value));','backend.write(spec.key, normalized.value); proposal.emplace(spec.key, std::move(normalized.value));','edges'),
        ('load-unrelated-keys',pref_source,'const auto table = toml::parse(bytes.str());','const auto table = toml::parse(bytes.str()); rex::cvar::LoadConfig(path);','whitelist'),
        ('miss-partial-setter',pref_source,'attempted.push_back(key);','if (key != "native_fsr3") attempted.push_back(key);','edges'),
        ('changed-choices-only',registry_source,
         'for (const auto& spec : catalog) {\n          if (!NativeSettingOverrideReason(spec.key).empty()) continue;\n          auto normalized = Canonical(spec, current.at(std::string(spec.key)), true);\n          if (normalized.valid) patch.push_back({std::string(spec.key), spec.type, std::move(normalized.value)});\n        }',
         'for (const auto& [key, value] : changes) patch.push_back({key, Find(key)->type, value});','seed')]
    for label,original,old,new,mode in controls:
        code=original.read_text();assert code.count(old)==1,label;mutant=out/(label+'.cpp');mutant.write_text(code.replace(old,new));exe=out/(label+'.exe')
        args=[str(mutant) if arg==str(original) else arg for arg in command];args[-1]=str(exe)
        r=subprocess.run(args,capture_output=True,text=True,timeout=90);(out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        case_root=out/(label+'-files');(case_root/'runtime').mkdir(parents=True,exist_ok=False);(case_root/'runtime/legodimensions.toml').write_text(template)
        generate('first-exe',case_root);private_boot=case_root/'build/first-exe/legodimensions.toml'
        r=subprocess.run([str(exe),mode,str(case_root),str(private_boot)],env=env,capture_output=True,text=True,timeout=20)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==23,(label,r.returncode,r.stdout,r.stderr)
        negative.append(dict(name=label,exit=r.returncode,error=r.stderr))
    files=[fixture,launcher,config_script,runtime,ffx,app]+[src/name for name in ['settings_preferences.cpp','settings_preferences.h','settings_registry.cpp','settings_registry.h','settings_service.cpp','settings_service.h','settings_config.cpp','settings_config.h','settings_transaction.cpp','settings_transaction.h']]
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,launcher_launched=False,outputs=outputs,
        sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},negative=negative,config_fragment_sha256=hashlib.sha256(fragment.encode()).hexdigest(),
        scope='Fresh CPU test processes use real SDK registry and production preferences/transaction/service/config code. Only reviewed config generation is executed in private folders, not any launcher. No actual game, platform window/GPU, frontend dialog rendering or Linux acceptance.'),indent=2))
    print(''.join(outputs),end='')
if __name__=='__main__':main()
