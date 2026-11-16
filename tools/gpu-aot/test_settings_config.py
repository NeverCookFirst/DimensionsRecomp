"""Exercise real settings transactions and config writer on private files only."""
from pathlib import Path
import argparse, hashlib, json, subprocess

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];src=root/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    include=root.parent/'dev-assets/sdk-install/include'
    production=src/'settings_config.cpp';header=src/'settings_config.h';transaction=src/'settings_transaction.cpp'
    fixture=out/'config-files.cpp'
    fixture.write_text(r'''
#include "settings_config.h"
#include "settings_transaction.h"
#include <toml++/toml.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace legodimensions::settings;
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#c);std::exit(23);} } while(0)
std::string read(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);std::ostringstream s;s<<in.rdbuf();return s.str();}
void put(const std::filesystem::path& p,const std::string& s){std::ofstream out(p,std::ios::binary);out<<s;CHECK(out.good());}
int main(int argc,char** argv){
 CHECK(argc==2);auto dir=std::filesystem::path(argv[1])/std::filesystem::path(u8"private settings Юникод");
 CHECK(!std::filesystem::exists(dir));std::filesystem::create_directory(dir);auto path=dir/"game.toml";
 const std::string original=R"(native_fps_limit = 0
native_fsr3 = false
native_fsr3_sharpness = 0.2
mods = 'AllWorlds,QuickStartup'
game_data_root = 'E:\My Games\Dimensions'
[unknown_mod]
numbers = [1, 2, 3]
label = "unchanged"
)";
 put(path,original);std::string error;
 std::vector<ConfigChange> changes{{"native_fps_limit",ConfigType::kInteger,"144"},{"native_fsr3",ConfigType::kBoolean,"true"},
  {"native_fsr3_sharpness",ConfigType::kFloat,"0.65"},{"native_fsr3_quality",ConfigType::kString,"balanced"},
  {"test_string",ConfigType::kString,"a'quote\" slash\\ newline\n tab\t"}};
 CHECK(SaveConfigPatch(path,changes,error)&&error.empty());auto saved=read(path);auto parsed=toml::parse(saved);
 CHECK(parsed["native_fps_limit"].is_integer()&&parsed["native_fps_limit"].value<int>()==144);
 CHECK(parsed["native_fsr3"].is_boolean()&&parsed["native_fsr3"].value<bool>()==true);
 CHECK(parsed["native_fsr3_sharpness"].is_floating_point()&&parsed["native_fsr3_sharpness"].value<double>()==0.65);
 CHECK(parsed["native_fsr3_quality"].value<std::string>()=="balanced");
 CHECK(parsed["test_string"].value<std::string>()==changes.back().value);
 CHECK(parsed["mods"].value<std::string>()=="AllWorlds,QuickStartup");
 CHECK(parsed["game_data_root"].value<std::string>()==R"(E:\My Games\Dimensions)");
 CHECK(parsed["unknown_mod"]["numbers"].as_array()->size()==3&&parsed["unknown_mod"]["label"].value<std::string>()=="unchanged");
 unsigned scenarios=1;
 for(const auto& invalid:std::vector<ConfigChange>{
  {"native_fsr3",ConfigType::kBoolean,"yes"},{"native_fps_limit",ConfigType::kInteger,"60garbage"},
  {"native_fps_limit",ConfigType::kInteger,"9223372036854775808"},{"native_fsr3_sharpness",ConfigType::kFloat,"nan"},
  {"native_fsr3_sharpness",ConfigType::kFloat,"inf"},{"native_fsr3_sharpness",ConfigType::kFloat,"0.2foo"},
  {"",ConfigType::kString,"x"},{"bad.key",ConfigType::kString,"x"},{"unknown_mod",ConfigType::kInteger,"1"},
  {"wrong_type",static_cast<ConfigType>(99),"x"}}){
  std::vector<ConfigChange> batch{{"unrelated_new",ConfigType::kInteger,"1"},invalid};
  CHECK(!SaveConfigPatch(path,batch,error)&&!error.empty());CHECK(read(path)==saved);++scenarios;
 }
 auto duplicate=changes;duplicate.push_back(changes.front());CHECK(!SaveConfigPatch(path,duplicate,error)&&read(path)==saved);++scenarios;
 put(path,"not [ valid TOML");CHECK(!SaveConfigPatch(path,changes,error));CHECK(read(path)=="not [ valid TOML");++scenarios;
 put(path,original);
#ifdef _WIN32
 auto locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 CHECK(locked!=INVALID_HANDLE_VALUE);CHECK(!SaveConfigPatch(path,changes,error)&&!error.empty());CHECK(read(path)==original);CloseHandle(locked);++scenarios;
 CHECK(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY));CHECK(!SaveConfigPatch(path,changes,error));CHECK(read(path)==original);
 CHECK(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL));++scenarios;
#endif
 CHECK(!SaveConfigPatch(dir/"missing parent"/"x.toml",changes,error));++scenarios;
 CHECK(!SaveConfigPatch(dir,changes,error));++scenarios;
 auto new_path=dir/"new.toml";CHECK(SaveConfigPatch(new_path,changes,error));CHECK(toml::parse(read(new_path))["native_fps_limit"].value<int>()==144);++scenarios;
 auto empty_path=dir/"untouched.toml";CHECK(SaveConfigPatch(empty_path,{},error)&&!std::filesystem::exists(empty_path));++scenarios;
 CHECK(SaveConfigPatch(path,{},error)&&read(path)==original);++scenarios;
 // Wire the actual transaction to the actual file writer. Display preview
 // must never publish a config until Keep, and timeout must preserve bytes.
 Values live{{"native_fsr3","false"},{"native_fps_limit","0"}};unsigned saves=0;
 Backend backend{[&]{return live;},[](auto,auto value){return NormalizedValue{true,std::string(value),{}};},
  [&](auto key,auto value){live[std::string(key)]=value;return true;},
  [&](const Values& patch,std::string& e){++saves;std::vector<ConfigChange> batch;for(auto& [k,v]:patch)
    batch.push_back({k,k=="native_fsr3"?ConfigType::kBoolean:ConfigType::kInteger,v});return SaveConfigPatch(path,batch,e);}};
 Transaction t(backend,{"native_fsr3"});auto now=Clock::now();CHECK(t.Open());CHECK(t.Edit("native_fsr3","true"));CHECK(t.Edit("native_fps_limit","120"));
 CHECK(t.Apply(now)&&saves==0&&read(path)==original);t.Tick(t.deadline());CHECK(saves==0&&read(path)==original&&live.at("native_fsr3")=="false");++scenarios;
 CHECK(t.Edit("native_fsr3","true")&&t.Apply(now));
#ifdef _WIN32
 locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(locked!=INVALID_HANDLE_VALUE);
 CHECK(!t.Keep(now)&&t.notice()==Notice::kSaveFailed&&read(path)==original);CloseHandle(locked);++scenarios;
#endif
 CHECK(t.Keep(now)&&t.notice()==Notice::kSaved);CHECK(toml::parse(read(path))["native_fsr3"].value<bool>()==true);++scenarios;
 for(auto& item:std::filesystem::directory_iterator(dir))CHECK(item.path().extension()!=".tmp");++scenarios;
 std::printf("PASS %u real filesystem/transaction scenarios; typed TOML, paths/mods/unknown data, Unicode, failures, preview/Keep/revert and temp cleanup\n",scenarios);
}
''',encoding='utf-8')
    def run(label,source):
        exe=out/(label+'.exe');case=out/(label+'-files');case.mkdir(exist_ok=False)
        result=subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(src),'-I'+str(include),str(fixture),str(source),str(transaction),'-o',str(exe)],capture_output=True,text=True,timeout=60)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        result=subprocess.run([str(exe),str(case)],capture_output=True,text=True,timeout=15)
        (out/(label+'.log')).write_text(result.stdout+result.stderr);return result
    result=run('production',production);result.check_returncode();print(result.stdout,end='')
    original=production.read_text();negative=[]
    for label,old,new in [
        ('drop-existing-data','auto table = toml::parse(original);','auto table = toml::parse(original); table.clear();'),
        ('float-as-string','table.insert_or_assign(change.key, number);\n      break;\n    }\n    case ConfigType::kString:', 'table.insert_or_assign(change.key, value);\n      break;\n    }\n    case ConfigType::kString:'),
        ('ignore-publish-failure','Publish(path, output.str());','try { Publish(path, output.str()); } catch (...) {}')]:
        assert original.count(old)==1,label
        mutant=out/(label+'.cpp');mutant.write_text(original.replace(old,new))
        failure=run(label,mutant);assert failure.returncode==23,(label,failure.returncode,failure.stdout,failure.stderr)
        negative.append(dict(name=label,exit=failure.returncode,error=failure.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,output=result.stdout,
        sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [production,header,transaction,src/'settings_transaction.h',fixture]},
        negative=negative,scope='Actual config writer + transaction on private Windows filesystem files. Does not prove crash/power-loss durability, concurrent external-editor serialization, POSIX behavior or native menu/render integration.'),indent=2))
if __name__=='__main__':main()
