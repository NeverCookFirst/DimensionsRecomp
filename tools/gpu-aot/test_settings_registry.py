"""Verify native settings model against the real SDK registry DLL, without a title."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];src=root/'rexlego/src';pkg=root.parent
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);sdk=pkg/'dev-assets/sdk-install'
    production=src/'settings_registry.cpp';runtime=pkg/'build/worlds-candidate-v18/rexruntime.dll'
    dependency=pkg/'build/worlds-candidate-v18/amd_fidelityfx_dx12.dll'
    shutil.copy2(runtime,out/'rexruntime.dll');shutil.copy2(dependency,out/dependency.name);fixture=out/'real-registry.cpp'
    fixture.write_text(r'''
#include "settings_registry.h"
#include "settings_service.h"
#include <rex/cvar.h>
#include <toml++/toml.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>
#include <windows.h>
using namespace legodimensions::settings;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#c);std::exit(23);}}while(0)
std::string read(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);std::ostringstream s;s<<f.rdbuf();return s.str();}
int main(int argc,char**argv){
 CHECK(argc==2);CHECK(GetModuleHandleW(L"rexruntime.dll"));auto path=std::filesystem::path(argv[1])/"settings.toml";CHECK(!std::filesystem::exists(path));
 Values backing;std::vector<std::string> registered;bool reject_fsr=false;
 for(const auto& spec:NativeSettingsCatalog()){
  const std::string key(spec.key);CHECK(!key.empty()&&!spec.label.empty());
  CHECK(NormalizeNativeSetting(key,spec.default_value).valid);
  if(!rex::cvar::GetFlagByName(key).empty())continue;
  backing[key]=spec.default_value;
  auto type=spec.type==ConfigType::kBoolean?rex::cvar::FlagType::Boolean:spec.type==ConfigType::kInteger?rex::cvar::FlagType::Int32:
    spec.type==ConfigType::kFloat?rex::cvar::FlagType::Double:rex::cvar::FlagType::String;
  rex::cvar::FlagEntry entry;entry.name=key;entry.type=type;entry.category="Test";entry.description="Actual native settings registry boundary";
  entry.default_value=spec.default_value;
  entry.getter=[&,key,type]{return type==rex::cvar::FlagType::Double?std::to_string(std::stod(backing.at(key))):backing.at(key);};
  entry.setter=[&,key,type](std::string_view raw){
   if(type==rex::cvar::FlagType::Boolean){if(raw!="true"&&raw!="false")return false;backing[key]=raw;}
   else if(type==rex::cvar::FlagType::Int32){int32_t value;auto parsed=std::from_chars(raw.data(),raw.data()+raw.size(),value);if(parsed.ec!=std::errc{})return false;backing[key]=std::to_string(value);}
   else if(type==rex::cvar::FlagType::Double){double value;if(!rex::cvar::ParseDouble(raw,value))return false;backing[key]=std::to_string(value);}
   else backing[key]=raw;
   return !(key=="native_fsr3"&&reject_fsr&&raw=="false");
  };
  CHECK(rex::cvar::RegisterFlag(std::move(entry)).has_value());registered.push_back(key);
 }
 for(const auto& [k,v]:NativeSettingsDefaults())CHECK(rex::cvar::SetFlagByName(k,v));
 // Existing SDK flags are used directly; app-defined flags use typed values
 // registered with the real DLL, including its lock, setter and callbacks.
 rex::cvar::FinalizeInit();auto backend=MakeNativeRegistryBackend(path);auto initial=backend.read();
 CHECK(initial==NativeSettingsDefaults());unsigned scenarios=1;
 for(auto [key,input,expected]:std::vector<std::tuple<std::string,std::string,std::string>>{
  {"native_fsr3_sharpness","0.200000","0.2"},{"native_fsr3_mip_bias","-0.0","0"},
  {"native_fsr3_sharpness","0.1234567","0.123457"},{"native_resolution_scale","0300","300"},
  {"native_resolution_scale","0","0"},{"native_resolution_scale","20","20"},
  {"native_fps_limit","0","0"},{"native_fps_limit","1000","1000"},{"present_effect","fsr","fsr"},
  {"native_fsr3_quality","native","native"},{"native_anisotropic_override","-2","-2"}}){
  auto normalized=backend.normalize(key,input);CHECK(normalized.valid&&normalized.value==expected);++scenarios;
 }
 for(auto [key,input]:std::vector<std::pair<std::string,std::string>>{
  {"native_fsr3","yes"},{"native_fps_limit","-1"},{"native_fps_limit","1001"},{"native_fps_limit","60x"},
  {"native_resolution_scale","15"},{"native_resolution_scale","301"},{"native_resolution_scale","201"},
  {"native_fsr3_sharpness","nan"},{"native_fsr3_sharpness","inf"},{"native_fsr3_sharpness","1.1"},
  {"native_fsr3_mip_bias","-1.1"},{"native_fsr3_quality","ultra"},{"present_effect","fsr2"},
  {"native_anisotropic_override","6"},{"missing","true"}}){CHECK(!backend.normalize(key,input).valid);++scenarios;}
 CHECK(!backend.write("missing","1"));++scenarios;
 std::string error;CHECK(!backend.persist({{"missing","1"}},error)&&!std::filesystem::exists(path));++scenarios;
 std::atomic<unsigned> rejected=0;
 std::thread wrong([&]{
  try{backend.read();}catch(const std::exception&){++rejected;}
  try{backend.normalize("native_fps_limit","60");}catch(const std::exception&){++rejected;}
  try{backend.write("native_fps_limit","60");}catch(const std::exception&){++rejected;}
  try{std::string e;backend.persist({{"native_fps_limit","60"}},e);}catch(const std::exception&){++rejected;}
 });wrong.join();CHECK(rejected==4&&backend.read()==initial&&!std::filesystem::exists(path));++scenarios;
 for(auto [key,env]:std::vector<std::pair<std::string,std::string>>{
  {"native_fps_limit","LEGO_NATIVE_FPS_LIMIT"},{"native_vsync","LEGO_NATIVE_VSYNC"},
  {"native_fsr3","LEGO_NATIVE_FSR3"},{"native_fsr3_quality","LEGO_NATIVE_FSR3_QUALITY"},
  {"native_fsr3_quality","LEGO_NATIVE_720P"}}){
   CHECK(_putenv_s(env.c_str(),"1")==0);auto denied=backend.normalize(key,initial.at(key));
   CHECK(!denied.valid&&denied.error.find(env)!=std::string::npos);CHECK(_putenv_s(env.c_str(),"")==0);++scenarios;
 }
 auto capability=MakeNativeRegistryBackend(path,[](auto key){return key=="native_fsr3"?"FSR unavailable on this device":"";});
 CHECK(!capability.normalize("native_fsr3","true").valid&&capability.normalize("native_fps_limit","60").valid);++scenarios;
 {std::ofstream f(path);f<<"mods = 'AllWorlds'\ngame_data_root = 'E:/My Games/Dimensions'\n";}
 const auto original=read(path);auto now=Clock::now();Transaction t(backend,NativeSettingsConfirmationKeys());CHECK(t.Open());
 CHECK(t.Edit("native_fps_limit","0")&&t.Edit("native_fsr3_sharpness","0.65")&&t.Edit("native_fsr3","false")&&t.Edit("native_resolution_scale","300"));
 CHECK(t.Apply(now)&&t.phase()==Phase::kPreview&&read(path)==original);
 CHECK(backend.read().at("native_fsr3_sharpness")=="0.65");CHECK(t.Keep(now)&&t.notice()==Notice::kSaved);++scenarios;
 auto saved=read(path);auto parsed=toml::parse(saved);CHECK(parsed["mods"].value<std::string>()=="AllWorlds");
 CHECK(parsed["native_resolution_scale"].value<int>()==300&&parsed["native_fsr3_sharpness"].is_floating_point());
 CHECK(parsed["native_fps_limit"].value<int>()==0&&parsed["native_fsr3"].value<bool>()==false);++scenarios;
 CHECK(t.Edit("native_resolution_scale","200")&&t.Apply(now));t.Tick(t.deadline());CHECK(backend.read().at("native_resolution_scale")=="300"&&read(path)==saved);++scenarios;
 CHECK(t.Defaults(NativeSettingsDefaults())&&t.Apply(now)&&t.Revert()&&t.Cancel());CHECK(read(path)==saved);++scenarios;
 // Verify preservation/restoration of legacy values rather than silently
 // clamping an installation's settings while opening or cancelling the page.
 CHECK(rex::cvar::SetFlagByName("native_resolution_scale","201"));Transaction legacy(backend,NativeSettingsConfirmationKeys());
 CHECK(legacy.Open()&&legacy.draft().at("native_resolution_scale")=="201");CHECK(legacy.Edit("native_resolution_scale","200")&&legacy.Apply(now)&&legacy.Revert());
 CHECK(backend.read().at("native_resolution_scale")=="201");++scenarios;
 CHECK(rex::cvar::SetFlagByName("native_fsr3","true"));reject_fsr=true;Transaction failing(backend,NativeSettingsConfirmationKeys());
 CHECK(failing.Open()&&failing.Edit("native_fsr3","false"));CHECK(!failing.Apply(now)&&failing.notice()==Notice::kApplyFailed);
 CHECK(backend.read().at("native_fsr3")=="true"&&read(path)==saved);reject_fsr=false;++scenarios;
 // Actual service transport: guest threads submit copied commands, and the
 // owning UI thread pumps them. The real SDK fullscreen callback proves the
 // same thread is used; no platform window is created in this test.
 std::deque<std::function<void()>> pending;std::mutex pending_mutex;unsigned post_calls=0,window_callbacks=0;
 auto ui_owner=std::this_thread::get_id();auto time=now;
 QueuedSettings::Post post=[&](std::function<void()> command){std::lock_guard lock(pending_mutex);++post_calls;pending.push_back(std::move(command));return true;};
 auto pump=[&]{for(;;){std::function<void()> next;{std::lock_guard lock(pending_mutex);if(pending.empty())break;next=std::move(pending.front());pending.pop_front();}next();}};
 auto service=std::make_shared<QueuedSettings>(MakeNativeRegistryBackend(path),post,NativeSettingsDefaults(),NativeSettingsConfirmationKeys(),[&]{return time;});
 PublishNativeSettingsService(service);CHECK(AcquireNativeSettingsService()==service);++scenarios;
 rex::cvar::RegisterChangeCallback("fullscreen",[&](auto,auto){CHECK(std::this_thread::get_id()==ui_owner);++window_callbacks;});
 auto invoke=[&](Command command){std::optional<std::future<Reply>> future;std::thread guest([&]{future.emplace(service->Submit(command));});guest.join();
  CHECK(future->wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);pump();return future->get();};
 CHECK(invoke({CommandKind::kOpen}).ok);++scenarios;
 auto draft=invoke({CommandKind::kEdit,"fullscreen","true"});CHECK(draft.ok&&draft.dirty&&backend.read().at("fullscreen")=="false");++scenarios;
 auto preview=invoke({CommandKind::kApply});CHECK(preview.ok&&preview.phase==Phase::kPreview&&preview.confirmation_ms==15000&&window_callbacks==1&&read(path)==saved);++scenarios;
 time+=std::chrono::seconds(15);CHECK(service->AdvanceFromUI());auto expired=invoke({CommandKind::kSnapshot});
 CHECK(expired.phase==Phase::kEditing&&expired.notice==Notice::kReverted&&backend.read().at("fullscreen")=="false"&&window_callbacks==2&&read(path)==saved);++scenarios;
 CHECK(invoke({CommandKind::kEdit,"fullscreen","true"}).ok&&invoke({CommandKind::kApply}).ok);
 auto focus=service->FocusLostFromUI();CHECK(focus.ok&&focus.phase==Phase::kEditing&&focus.notice==Notice::kReverted&&backend.read().at("fullscreen")=="false"&&read(path)==saved);++scenarios;
 CHECK(invoke({CommandKind::kDefaults}).ok);CHECK(invoke({CommandKind::kApply}).ok&&invoke({CommandKind::kRevert}).ok);++scenarios;
 // A menu page owns its command strings; mutating the caller's buffers after
 // submit must not change what the UI thread eventually receives.
 Command copied{CommandKind::kEdit,"native_fps_limit","90"};auto owned=service->Submit(copied);copied.key="fullscreen";copied.value="true";pump();
 CHECK(owned.get().values.at("native_fps_limit")=="90");++scenarios;
 CHECK(invoke({CommandKind::kApply}).ok);CHECK(toml::parse(read(path))["native_fps_limit"].value<int>()==90);auto service_saved=read(path);++scenarios;
 std::atomic<unsigned> service_rejected=0;std::thread bad_owner([&]{
  if(service->TickFromUI().unavailable)++service_rejected;
  if(!service->AdvanceFromUI())++service_rejected;
  if(service->FocusLostFromUI().unavailable)++service_rejected;
  if(service->ShutdownFromUI().unavailable)++service_rejected;
 });bad_owner.join();CHECK(service_rejected==4&&invoke({CommandKind::kSnapshot}).ok);++scenarios;
 CHECK(invoke({CommandKind::kEdit,"fullscreen","true"}).ok&&invoke({CommandKind::kApply}).ok);
 auto late=service->Submit({CommandKind::kSnapshot});const auto posted=post_calls;
 auto stopped=service->ShutdownFromUI();CHECK(stopped.ok&&stopped.phase==Phase::kClosed&&backend.read().at("fullscreen")=="false"&&read(path)==service_saved);++scenarios;
 auto denied=service->Submit({CommandKind::kApply});CHECK(denied.wait_for(std::chrono::milliseconds(0))==std::future_status::ready&&denied.get().unavailable&&post_calls==posted);
 PublishNativeSettingsService({});CHECK(!AcquireNativeSettingsService());service.reset();pump();CHECK(late.get().unavailable);++scenarios;
 rex::cvar::UnregisterChangeCallbacks("fullscreen");
 for(bool throws:{false,true}){
  QueuedSettings rejected_queue(MakeNativeRegistryBackend(path),[&](auto){if(throws)throw std::runtime_error("Queue closed");return false;},NativeSettingsDefaults(),NativeSettingsConfirmationKeys());
  auto denied=rejected_queue.Submit({CommandKind::kOpen});CHECK(denied.wait_for(std::chrono::milliseconds(0))==std::future_status::ready&&denied.get().unavailable);CHECK(rejected_queue.ShutdownFromUI().ok);++scenarios;
 }
 {QueuedSettings wrong_queue(MakeNativeRegistryBackend(path),post,NativeSettingsDefaults(),NativeSettingsConfirmationKeys());
  auto reply=wrong_queue.Submit({CommandKind::kOpen});std::function<void()> task;{std::lock_guard lock(pending_mutex);task=std::move(pending.front());pending.pop_front();}
  std::thread mistaken_ui([&]{task();});mistaken_ui.join();CHECK(reply.get().unavailable&&wrong_queue.ShutdownFromUI().ok);++scenarios;
 }
 rex::cvar::UnregisterFlag("native_fsr3");bool missing=false;try{backend.read();}catch(const std::exception&){missing=true;}CHECK(missing);++scenarios;
 for(const auto& key:registered)rex::cvar::UnregisterFlag(key);
 std::printf("PASS %u real SDK settings scenarios: validation, canonical values, UI-thread transport/callbacks, deadline/focus/shutdown rollback, queue failures, overrides/capabilities, actual transaction/persistence\n",scenarios);
}
''',encoding='utf-8')
    env=os.environ.copy()
    for key in ['LEGO_NATIVE_FPS_LIMIT','LEGO_NATIVE_VSYNC','LEGO_NATIVE_FSR3','LEGO_NATIVE_FSR3_QUALITY','LEGO_NATIVE_720P']:env.pop(key,None)
    service=src/'settings_service.cpp'
    def run(label,source,service_source=service):
        exe=out/(label+'.exe');case=out/(label+'-files');case.mkdir(exist_ok=False)
        command=[a.compiler,'-std=c++20','-O2','-Wno-deprecated-literal-operator','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX',
                 '-I'+str(src),'-I'+str(sdk/'include'),str(fixture),str(source),str(service_source),str(src/'settings_config.cpp'),str(src/'settings_transaction.cpp'),
                 str(sdk/'lib/rexruntime.lib'),'-o',str(exe)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=75)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        result=subprocess.run([str(exe),str(case)],capture_output=True,text=True,env=env,timeout=20)
        (out/(label+'.log')).write_text(result.stdout+result.stderr);return result
    positive=run('production',production);positive.check_returncode();print(positive.stdout,end='')
    text=production.read_text();negative=[]
    for label,old,new in [('no-owner-thread','if (owner != std::this_thread::get_id())','if (false)'),
                          ('wrong-fps-override','variable = "LEGO_NATIVE_FPS_LIMIT";','variable = "LEGO_NATIVE_FPS";'),
                          ('no-scale-step','(number < 20 || number % 5 != 0)','(number < 20)')]:
        assert text.count(old)==1,label
        mutant=out/(label+'.cpp');mutant.write_text(text.replace(old,new))
        result=run(label,mutant);assert result.returncode==23,(label,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    service_text=service.read_text()
    for label,old,new in [('queue-wrong-thread','if (!IsOwner()) return Unavailable("Native settings queue ran outside the platform UI thread");','if (false) return Unavailable("Native settings queue ran outside the platform UI thread");'),
                          ('ignore-queue-shutdown','if (stopped.load()) return Unavailable("Native settings are shutting down");','if (false) return Unavailable("Native settings are shutting down");'),
                          ('no-focus-rollback','ok = state_->transaction.Revert();','ok = true;')]:
        assert service_text.count(old)==1,label
        mutant=out/(label+'.cpp');mutant.write_text(service_text.replace(old,new))
        result=run(label,production,mutant);assert result.returncode==23,(label,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    files=[production,src/'settings_registry.h',service,src/'settings_service.h',src/'settings_config.cpp',src/'settings_config.h',src/'settings_transaction.cpp',src/'settings_transaction.h',fixture,runtime,dependency]
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,output=positive.stdout,
        sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},negative=negative,
        scope='Real SDK registry DLL and fullscreen change callback; typed app flag registrations, actual native backend/transaction/config writer/queued service. No game, native UI, actual platform window, renderer resource acknowledgement or GPU capability discovery.'),indent=2))
if __name__=='__main__':main()
