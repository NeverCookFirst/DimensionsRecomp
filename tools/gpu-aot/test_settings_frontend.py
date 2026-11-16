"""Verify native settings page definitions and queued frontend interactions offline.

Executes production frontend, catalog/normalizer, transaction and queued service.
Registry setters, persistence, the UI event loop, native widget drawing and GPU
acknowledgement are boundaries; this is not a gameplay/menu rendering test.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--compiler', default='clang++')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    src, sdk = root/'rexlego/src', root.parent/'dev-assets/sdk-install'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = root.parent/'build/worlds-candidate-v18'
    for name in ['rexruntime.dll', 'amd_fidelityfx_dx12.dll']:
        shutil.copy2(runtime/name, out/name)
    fixture = out/'frontend-fixture.cpp'
    fixture.write_text(r'''
#include "settings_frontend.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <thread>
using namespace legodimensions::settings;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#c);std::exit(23);}}while(0)
struct Fixture {
 Values live=NativeSettingsDefaults(),saved;Clock::time_point time{};
 std::deque<std::function<void()>> jobs;std::shared_ptr<QueuedSettings> service;
 std::unique_ptr<FrontendController> menu;unsigned writes=0,saves=0;
 bool save_failure=false,partial_apply_failure=false,restore_failure=false,deny_fps=false,reject_queue=false,drop_queue=false;
 Fixture(){
  const auto owner=std::this_thread::get_id();
  Backend backend{[this,owner]{CHECK(owner==std::this_thread::get_id());return live;},
   [this,owner](auto k,auto v){CHECK(owner==std::this_thread::get_id());if(deny_fps&&k=="native_fps_limit")return NormalizedValue{false,{},"Explicit override"};return NormalizeNativeSetting(k,v);},
   [this,owner](auto k,auto v){CHECK(owner==std::this_thread::get_id());++writes;
    if(restore_failure&&k=="fullscreen"&&v=="false")return false;
    live[std::string(k)]=v;return !(partial_apply_failure&&k=="fullscreen"&&v=="true");},
   [this,owner](const Values&v,std::string&e){CHECK(owner==std::this_thread::get_id());if(save_failure){e="Disk unavailable";return false;}saved=v;++saves;return true;}};
  service=std::make_shared<QueuedSettings>(backend,[this](auto f){if(reject_queue)return false;if(drop_queue)return true;jobs.push_back(std::move(f));return true;},
   NativeSettingsDefaults(),NativeSettingsConfirmationKeys(),[this]{return time;});
  menu=std::make_unique<FrontendController>(service,[this]{return time;});
 }
 ~Fixture(){service->ShutdownFromUI();}
 void flush(){for(unsigned i=0;i<32;++i){if(!jobs.empty()){auto f=std::move(jobs.front());jobs.pop_front();f();}menu->Poll();if(jobs.empty()&&!menu->busy())return;}CHECK(false);}
 size_t row(std::string_view key){for(size_t i=0;i<menu->layout().rows.size();++i)if(menu->layout().rows[i].key==key)return i;CHECK(false);return 0;}
 size_t action(RowAction action){for(size_t i=0;i<menu->layout().rows.size();++i)if(menu->layout().rows[i].action==action)return i;CHECK(false);return 0;}
 void open(FrontendPage page=FrontendPage::kVideo){CHECK(menu->Enter(page));flush();CHECK(menu->reply().phase==Phase::kEditing);}
 void fullscreen(){CHECK(menu->Adjust(row("fullscreen"),1));flush();CHECK(menu->Activate(action(RowAction::kApply)));flush();CHECK(menu->reply().phase==Phase::kPreview);}
};
int main(int argc,char**argv){
 CHECK(argc==2);const auto output=std::filesystem::path(argv[1]);unsigned count=0,adjustments=0;
 CHECK(NormalizeNativeSetting("native_cel_shading","off").value=="original");++count;
 // Both native definitions cover each approved setting once; every action has
 // its own native focus target. Actual XML graph checks run in Python below.
 std::set<std::string> keys;
 for(auto page:{FrontendPage::kVideo,FrontendPage::kEffects}){
  auto layout=MakeFrontendLayout(page);CHECK(layout.rows.size()==(page==FrontendPage::kVideo?10:13));
  std::ofstream xml(output/(layout.name+".xml"));xml<<layout.xml;
  std::ofstream rows(output/(layout.name+".tsv"));
  for(const auto&row:layout.rows){rows<<row.element<<'\t'<<row.value_element<<'\t'<<row.key<<'\n';if(row.action==RowAction::kSetting)CHECK(keys.insert(row.key).second);}
  auto recreated=MakeFrontendLayout(page,2);CHECK(recreated.name==layout.name+"_g2"&&recreated.rows.size()==layout.rows.size());
  std::ofstream regenerated_xml(output/(recreated.name+".xml"));regenerated_xml<<recreated.xml;
  std::ofstream regenerated_rows(output/(recreated.name+".tsv"));
  for(size_t i=0;i<recreated.rows.size();++i){const auto&r=recreated.rows[i];CHECK(r.key==layout.rows[i].key);
   regenerated_rows<<r.element<<'\t'<<r.value_element<<'\t'<<r.key<<'\n';}++count;
  ++count;
 }
 CHECK(keys.size()==NativeSettingsCatalog().size());++count;
 std::string value="0";
 for(auto next:{"30","60","100","120","144","240","0"}){auto n=AdjustSettingValue("native_fps_limit",value,1);CHECK(n.valid&&n.value==next);value=n.value;++adjustments;}
 for(auto next:{"240","144","120","100","60","30","0"}){auto n=AdjustSettingValue("native_fps_limit",value,-1);CHECK(n.valid&&n.value==next);value=n.value;++adjustments;}
 CHECK(AdjustSettingValue("native_fps_limit","77",1).value=="100"&&AdjustSettingValue("native_fps_limit","77",-1).value=="60");
 CHECK(AdjustSettingValue("native_fps_limit","500",1).value=="0"&&AdjustSettingValue("native_fps_limit","500",-1).value=="240");++count;
 value="0";for(int i=20;i<=300;i+=5){auto n=AdjustSettingValue("native_resolution_scale",value,1);CHECK(n.valid&&n.value==std::to_string(i));value=n.value;++adjustments;}
 CHECK(AdjustSettingValue("native_resolution_scale",value,1).value=="300");
 for(int i=295;i>=20;i-=5){auto n=AdjustSettingValue("native_resolution_scale",value,-1);CHECK(n.valid&&n.value==std::to_string(i));value=n.value;++adjustments;}
 CHECK(AdjustSettingValue("native_resolution_scale","20",-1).value=="0");
 CHECK(AdjustSettingValue("native_resolution_scale","201",1).value=="205"&&AdjustSettingValue("native_resolution_scale","201",-1).value=="200");++count;
 for(auto [k,v,d,expected]:std::vector<std::tuple<std::string,std::string,int,std::string>>{
  {"fullscreen","true",1,"false"},{"fullscreen","false",-1,"true"},
  {"native_cel_shading","original",1,"everywhere"},{"native_cel_shading","everywhere",1,"original"},
  {"native_cel_shading","off",1,"everywhere"},{"native_cel_shading","original",-1,"everywhere"},
  {"native_bloom","true",1,"false"},{"native_bloom","false",-1,"true"},
  {"native_fsr3_quality","native",-1,"ultra_performance"},{"native_fsr3_quality","ultra_performance",1,"native"},
  {"native_fsr3_sharpness","0.123456",1,"0.133456"},{"native_fsr3_sharpness","1",1,"1"},
  {"native_fsr3_mip_bias","-1",-1,"-1"},{"present_fsr_max_upsampling_passes","1",-1,"1"}}){
  auto n=AdjustSettingValue(k,v,d);CHECK(n.valid&&n.value==expected);++count;
 }
 CHECK(!AdjustSettingValue("fullscreen","garbage",1).valid&&!AdjustSettingValue("native_fps_limit","0.5",1).valid);
 CHECK(!AdjustSettingValue("native_fsr3_sharpness","nan",1).valid&&!AdjustSettingValue("missing","0",1).valid);
 CHECK(!AdjustSettingValue("fullscreen","true",0).valid);++count;
 CHECK(DisplaySettingValue("native_resolution_scale","300")=="300% (3840 x 2160)");
 CHECK(DisplaySettingValue("native_cel_shading","original")=="Original"&&
       DisplaySettingValue("native_cel_shading","everywhere")=="Everywhere"&&
       DisplaySettingValue("native_cel_shading","off")=="Original");
 CHECK(DisplaySettingValue("native_resolution_scale","0")=="Automatic"&&DisplaySettingValue("native_fps_limit","0")=="Unlocked");
 for(unsigned i=1;i<=5;++i)CHECK(DisplaySettingValue("native_anisotropic_override",std::to_string(i))==std::to_string(1u<<(i-1))+"x");++count;
 {Fixture f;CHECK(f.menu->Enter(FrontendPage::kVideo));CHECK(f.menu->busy()&&f.jobs.size()==1&&f.writes==0);
  CHECK(!f.menu->Adjust(0,1));f.flush();CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));CHECK(f.writes==0);f.flush();
  CHECK(f.menu->reply().values.at("native_fps_limit")=="0"&&f.live.at("native_fps_limit")=="240"&&f.saves==0);
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();CHECK(f.live.at("native_fps_limit")=="0"&&f.saves==1&&f.menu->dialog().kind==FrontendDialog::kInfo);
  CHECK(f.menu->Choose(0)&&f.menu->Back());f.flush();CHECK(f.menu->exit_requested());++count;}
 {Fixture f;f.open();f.fullscreen();CHECK(f.saves==0&&f.live.at("fullscreen")=="true"&&f.menu->dialog().kind==FrontendDialog::kKeep);
  CHECK(f.menu->dialog().message.find("15 seconds")!=std::string::npos);f.time+=std::chrono::seconds(1);f.menu->Poll();
  CHECK(f.jobs.size()==1&&!f.menu->busy());CHECK(f.menu->Choose(0)&&f.menu->busy());f.flush();
  CHECK(f.saves==1&&f.saved.at("fullscreen")=="true"&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();f.fullscreen();f.time+=std::chrono::seconds(15);CHECK(f.service->AdvanceFromUI());
  f.menu->Poll();f.flush();CHECK(f.live.at("fullscreen")=="false"&&f.saves==0&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();f.fullscreen();f.time+=std::chrono::seconds(15);CHECK(f.menu->Choose(0));f.flush();
  CHECK(f.saves==0&&f.live.at("fullscreen")=="false"&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();f.fullscreen();f.time+=std::chrono::milliseconds(100);f.menu->Poll();CHECK(f.menu->Choose(0));
  f.time+=std::chrono::seconds(15);f.flush();CHECK(f.saves==0&&f.live.at("fullscreen")=="false"&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_show_fps"),1));f.flush();
  CHECK(f.menu->reply().values.at("native_show_fps")=="false"&&f.live.at("native_show_fps")=="true");
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();
  CHECK(f.live.at("native_show_fps")=="false"&&f.saved.at("native_show_fps")=="false"&&f.live.at("native_fps_limit")=="240");
  CHECK(f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_bloom"),1));f.flush();
  CHECK(f.menu->Adjust(f.row("native_cel_shading"),1));f.flush();
  CHECK(f.live.at("native_bloom")=="true"&&f.live.at("native_cel_shading")=="original");
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();
  CHECK(f.saved.at("native_bloom")=="false"&&f.saved.at("native_cel_shading")=="everywhere");
  CHECK(f.live.at("native_show_fps")=="true"&&f.live.at("native_fps_limit")=="240");++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));f.flush();CHECK(f.menu->Back()&&f.menu->dialog().kind==FrontendDialog::kDiscard);
  CHECK(f.menu->Choose(1)&&!f.menu->exit_requested()&&f.menu->reply().dirty);CHECK(f.menu->Back()&&f.menu->Choose(0));f.flush();
  CHECK(f.menu->exit_requested()&&f.writes==0&&f.saves==0);++count;}
 {Fixture f;f.live["native_fps_limit"]="123";f.live["native_fsr3"]="false";f.open(FrontendPage::kEffects);
  CHECK(f.menu->Activate(f.action(RowAction::kDefaults))&&f.menu->dialog().kind==FrontendDialog::kDefaults);CHECK(f.menu->Choose(0));f.flush();
  CHECK(f.menu->reply().values==NativeSettingsDefaults()&&f.writes==0&&f.saves==0&&f.menu->reply().dirty);
  CHECK(f.menu->Choose(0)&&f.menu->Activate(f.action(RowAction::kApply)));f.flush();CHECK(f.menu->reply().phase==Phase::kPreview);
  CHECK(f.menu->Choose(0));f.flush();CHECK(f.saves==1&&f.live==NativeSettingsDefaults());++count;}
 {Fixture f;f.open();f.deny_fps=true;CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));f.flush();
  CHECK(f.menu->dialog().kind==FrontendDialog::kError&&f.writes==0&&!f.menu->reply().dirty);
  CHECK(f.menu->Back()&&f.menu->Back());f.flush();CHECK(f.menu->exit_requested());++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));f.flush();f.save_failure=true;
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();CHECK(f.menu->dialog().kind==FrontendDialog::kSaveFailed&&f.live.at("native_fps_limit")=="0"&&f.saves==0);
  CHECK(f.menu->dialog().buttons[0]=="Retry save"&&f.menu->Choose(1));f.flush();CHECK(f.live.at("native_fps_limit")=="240"&&f.saves==0);++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));f.flush();f.save_failure=true;
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();f.save_failure=false;CHECK(f.menu->Choose(0));f.flush();
  CHECK(f.saves==1&&f.live.at("native_fps_limit")=="0"&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();f.partial_apply_failure=true;CHECK(f.menu->Adjust(f.row("fullscreen"),1));f.flush();
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();CHECK(f.live.at("fullscreen")=="false"&&f.menu->dialog().kind==FrontendDialog::kError&&f.saves==0);++count;}
 {Fixture f;f.open();f.fullscreen();f.restore_failure=true;CHECK(f.menu->Back());f.flush();
  CHECK(f.menu->dialog().kind==FrontendDialog::kRecovery&&!f.menu->exit_requested());f.restore_failure=false;
  CHECK(f.menu->Choose(0));f.flush();CHECK(f.live.at("fullscreen")=="false"&&f.menu->dialog().kind==FrontendDialog::kInfo);++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("fullscreen"),1));f.flush();CHECK(f.menu->Activate(f.action(RowAction::kApply)));
  f.menu->Leave();f.flush();CHECK(f.menu->exit_requested()&&f.live.at("fullscreen")=="false"&&f.saves==0);++count;}
 {Fixture f;CHECK(f.menu->Enter(FrontendPage::kVideo));f.menu->Leave();f.flush();CHECK(f.menu->exit_requested()&&f.writes==0);++count;}
 {Fixture f;f.menu->Leave();f.flush();CHECK(f.menu->exit_requested()&&f.writes==0&&f.jobs.empty());++count;}
 {Fixture f;f.open();CHECK(f.menu->Adjust(f.row("native_fps_limit"),1));f.menu->RebindPage(FrontendPage::kEffects);f.flush();
  CHECK(f.menu->page()==FrontendPage::kEffects&&f.menu->reply().values.at("native_fps_limit")=="0"&&f.menu->reply().dirty&&f.writes==0);
  CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();CHECK(f.saves==1&&f.live.at("native_fps_limit")=="0");++count;}
 {Fixture f;CHECK(f.service->SetStartupErrorFromUI("Old preference file was invalid"));f.open();CHECK(f.menu->dialog().kind==FrontendDialog::kStartup);
  CHECK(f.menu->Choose(0));f.flush();CHECK(f.menu->dialog().kind==FrontendDialog::kNone&&f.menu->reply().startup_error.empty());++count;}
 {Fixture f;f.reject_queue=true;CHECK(f.menu->Enter(FrontendPage::kVideo));f.flush();CHECK(f.menu->dialog().kind==FrontendDialog::kError);
  CHECK(f.menu->Choose(0)&&f.menu->Back()&&f.menu->exit_requested());++count;}
 {FrontendController menu({});CHECK(!menu.Enter(FrontendPage::kVideo)&&menu.dialog().kind==FrontendDialog::kError);
  CHECK(menu.Choose(0)&&menu.Back()&&menu.exit_requested());++count;}
 {Fixture f;f.drop_queue=true;CHECK(f.menu->Enter(FrontendPage::kVideo));f.flush();CHECK(f.menu->dialog().kind==FrontendDialog::kError);
  CHECK(f.menu->dialog().message=="Could not apply settings");CHECK(f.menu->reply().error.find("interrupted")!=std::string::npos);
  CHECK(f.menu->Choose(0)&&f.menu->Back()&&f.menu->exit_requested());++count;}
 {Fixture f;std::vector<std::string> audit;f.menu->SetTrace([&](std::string text){audit.push_back(std::move(text));});f.open();
  CHECK(f.menu->Adjust(f.row("native_vsync"),1));f.flush();CHECK(f.menu->Activate(f.action(RowAction::kApply)));f.flush();
  CHECK(f.menu->Choose(1));f.flush();
  auto contains=[&](std::string_view part){return std::any_of(audit.begin(),audit.end(),[&](const auto&line){return line.find(part)!=std::string::npos;});};
  CHECK(contains("edit native_vsync old=false requested=true")&&contains("action=apply")&&contains("action=revert"));
  CHECK(contains("notice=restored")&&contains("native_resolution_scale=")&&contains("native_fsr3_sharpness="));
  ++count;}
 // Dependent rows follow the staged selection without applying it. Disabled
 // controls retain their existing values and submit no edit/dialog.
 {Fixture f;f.live["native_fsr3"]="false";f.live["native_fsr3_sharpen"]="false";f.live["present_effect"]="bilinear";f.open(FrontendPage::kEffects);
  const auto original=f.menu->reply().values;
  for(const auto key:{"native_fsr3_quality","native_fsr3_sharpen","native_fsr3_sharpness","native_fsr3_mip_bias","present_fsr_sharpness_reduction","present_fsr_max_upsampling_passes"}){
   CHECK(!FrontendSettingEnabled(key,f.menu->reply().values));CHECK(!f.menu->Adjust(f.row(key),1));CHECK(!f.menu->Activate(f.row(key)));}
  CHECK(f.menu->reply().values==original&&f.writes==0&&f.saves==0&&f.menu->dialog().kind==FrontendDialog::kNone);
  CHECK(f.menu->Adjust(f.row("native_fsr3"),1));f.flush();
  CHECK(FrontendSettingEnabled("native_fsr3_quality",f.menu->reply().values));CHECK(!FrontendSettingEnabled("native_fsr3_sharpness",f.menu->reply().values));
  CHECK(!f.menu->Adjust(f.row("present_effect"),1));CHECK(f.menu->Adjust(f.row("native_fsr3_sharpen"),1));f.flush();
  CHECK(FrontendSettingEnabled("native_fsr3_sharpness",f.menu->reply().values));CHECK(f.menu->Adjust(f.row("native_fsr3_sharpness"),1));f.flush();
  CHECK(f.menu->Adjust(f.row("native_fsr3"),1));f.flush();CHECK(f.menu->Adjust(f.row("present_effect"),1));f.flush();
  CHECK(FrontendSettingEnabled("present_fsr_sharpness_reduction",f.menu->reply().values));CHECK(f.menu->Adjust(f.row("present_fsr_sharpness_reduction"),1));f.flush();
  CHECK(f.writes==0&&f.saves==0&&f.live==original);CHECK(f.menu->Back()&&f.menu->Choose(0));f.flush();CHECK(f.live==original);++count;}
 {auto v=NativeSettingsDefaults();
  for(bool temporal:{false,true})for(bool sharpen:{false,true})for(const auto spatial:{"bilinear","cas","fsr"}){
   v["native_fsr3"]=temporal?"true":"false";v["native_fsr3_sharpen"]=sharpen?"true":"false";v["present_effect"]=spatial;
   CHECK(FrontendSettingEnabled("native_resolution_scale",v)==!temporal);CHECK(FrontendSettingEnabled("native_fsr3_quality",v)==temporal);
   CHECK(FrontendSettingEnabled("native_fsr3_mip_bias",v)==temporal);CHECK(FrontendSettingEnabled("native_fsr3_sharpen",v)==temporal);
   CHECK(FrontendSettingEnabled("native_fsr3_sharpness",v)==(temporal&&sharpen));CHECK(FrontendSettingEnabled("present_effect",v)==!temporal);
   CHECK(FrontendSettingEnabled("present_fsr_sharpness_reduction",v)==(!temporal&&std::string_view(spatial)=="fsr"));
   CHECK(FrontendSettingEnabled("present_fsr_max_upsampling_passes",v)==(!temporal&&std::string_view(spatial)=="fsr"));
   CHECK(FrontendSettingEnabled("fullscreen",v)&&FrontendSettingEnabled("native_bloom",v));++count;}}
 std::printf("PASS %u native frontend scenarios + %u full-range adjustment steps; generated Video/Effects XML; real queued service/transaction, staged edits, dialogs, delayed confirmation, timeout, rollback, leave ordering, dependent row gating and complete native-settings audit values\n",count,adjustments);
}
''')
    production = src/'settings_frontend.cpp'
    flags = ['-std=c++23', '-O2', '-Wno-deprecated-literal-operator',
             '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS',
             '-I'+str(src), '-I'+str(sdk/'include')]
    # Common production units and the fixture are unchanged by the frontend
    # fault controls. Compile them once, in private storage, then relink each
    # mutated frontend. No application build outputs are used or modified.
    objects = []
    for source in [fixture, *[src/n for n in ['settings_registry.cpp', 'settings_service.cpp', 'settings_transaction.cpp', 'settings_config.cpp']]]:
        obj = out/(source.stem+'.obj')
        result = subprocess.run([a.compiler, *flags, '-c', str(source), '-o', str(obj)], capture_output=True, text=True, timeout=75)
        (out/(source.stem+'-compile.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        objects.append(obj)

    def run(label, frontend=production):
        case = out/(label+'-files')
        case.mkdir(exist_ok=False)
        exe = out/(label+'.exe')
        obj = out/(label+'-frontend.obj')
        result = subprocess.run([a.compiler, *flags, '-c', str(frontend), '-o', str(obj)], capture_output=True, text=True, timeout=75)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([a.compiler, *map(str, objects), str(obj), str(sdk/'lib/rexruntime.lib'), '-o', str(exe)], capture_output=True, text=True, timeout=75)
        (out/(label+'-link.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe), str(case)], capture_output=True, text=True, timeout=20)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        return result, case

    positive, case = run('production')
    positive.check_returncode()
    print(positive.stdout, end='', flush=True)
    graphs = []
    for path in case.glob('*.xml'):
        tree = ET.parse(path).getroot()
        rows = [line.split('\t') for line in path.with_suffix('.tsv').read_text().splitlines()]
        texts = tree.findall('./uielements/text')
        names = {node.attrib['name'] for node in texts}
        assert len(names) == len(texts) == len(rows)*2+1
        assert tree.attrib['focus'] == rows[0][0]
        nav = tree.findall('./navigation/navigate')
        assert len(nav) == len(rows)*2
        for i, row in enumerate(rows):
            assert row[0] in names and row[1] in names
            for direction, dest in [('up', rows[(i-1) % len(rows)][0]), ('down', rows[(i+1) % len(rows)][0])]:
                # 'from' is a reserved Python word, so compare via a literal.
                matches = [n for n in nav if n.attrib == {'from':row[0], 'direction':direction, 'to':dest}]
                assert len(matches) == 1
        for node in texts:
            y = int(node.find('dimensions').attrib['y'])
            assert 0 < y < 1000
            assert ('text_template' in node.attrib) == (node.attrib['name'] in {r[0] for r in rows})
        graphs.append(dict(path=str(path), rows=len(rows), native_focus_graph=True))
    assert len(graphs) == 4
    text = production.read_text()
    negative = []
    for label, old, new in [
        ('edit-applies-instead-of-staging', 'return Send({CommandKind::kEdit, key, std::move(value.value)});', 'return Send({CommandKind::kApply});'),
        ('keep-reverts', 'button == 0 ? CommandKind::kKeep : CommandKind::kRevert', 'CommandKind::kRevert'),
        ('skip-discard-confirmation', 'if (reply_.dirty) { Show(', 'if (false) { Show('),
        ('leave-keeps-preview', 'if (service_) Send({CommandKind::kCancel});', 'if (service_) Send({CommandKind::kKeep});'),
    ]:
        assert text.count(old) == 1, label
        mutant = out/(label+'.cpp')
        mutant.write_text(text.replace(old, new))
        result, _ = run(label, mutant)
        assert result.returncode == 23, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    files = [src/n for n in ['settings_frontend.cpp', 'settings_frontend.h', 'settings_registry.cpp', 'settings_registry.h',
             'settings_service.cpp', 'settings_service.h', 'settings_transaction.cpp', 'settings_transaction.h', 'settings_config.cpp', 'settings_config.h']]
    files += [Path(__file__).resolve(), fixture]
    (out/'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=positive.stdout,
        sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}, negative=negative, graphs=graphs,
        scope=__doc__, limits=['Native hooks/layout parser are not invoked by this test.',
        'Visual layout, real keyboard/controller focus, native dialog publication and renderer/window acknowledgement remain unproven.']), indent=2))


if __name__ == '__main__':
    main()
