"""Offline regressions for passive F3 and the real content-enumerator entry.

Uses production function bodies and real Dear ImGui. Kernel/VFS and enumerator
allocation are bounded fixtures; AppendItem/WriteItems are production bodies.
No game process, window, saves, or settings are opened.
Run from a VS/clang environment; --before selects the saved negative controls.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def function(text, signature):
    start = text.index(signature)
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--package', type=pathlib.Path, required=True)
    ap.add_argument('--output', type=pathlib.Path, required=True)
    ap.add_argument('--before', type=pathlib.Path)
    args = ap.parse_args()
    sdk = args.package / 'source/rexglue-sdk'
    args.output.mkdir(parents=True, exist_ok=True)
    def read(rel):
        path = args.before / 'sdk' / rel if args.before else sdk / rel
        return path.read_text()
    draw = function(read('src/ui/overlay/debug_overlay.cpp'),
                    'void DebugOverlayDialog::OnDraw(')
    entry = function(read('src/kernel/xam/xam_content.cpp'),
                     'u32 XamContentCreateEnumerator_entry(')
    app = (args.before / 'installed/rex_app.cpp' if args.before else
           args.package / 'dev-assets/sdk-install/share/rexglue/rex_app.cpp').read_text()
    at = app.index('input_sys->SetActiveCallback([this]()')
    callback = function(app[at:], '[this]()')
    enumerator = (sdk / 'src/system/xenumerator.cpp').read_text()
    append = function(enumerator, 'uint8_t* XStaticUntypedEnumerator::AppendItem()')
    write = function(enumerator, 'uint32_t XStaticUntypedEnumerator::WriteItems(')
    cpp = r'''
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#define REXGLUE_BUILD_STAMP "offline input regression"
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) { if (failures < 8) std::cerr << message << '\n'; ++failures; }
}
namespace rex::ui {
struct FrameStats { double frame_time_ms=8, fps=125; uint64_t frame_count=1; };
struct Drawer {
  struct Style { struct Debug { ImVec4 muted_text{1,1,1,1}; } debug; } value;
  Style& style() { return value; }
  ImGuiIO& GetIO() { return ImGui::GetIO(); }
};
struct DebugOverlayDialog {
  std::function<FrameStats()> stats_provider_ = [] { return FrameStats{}; };
  Drawer drawer;
  Drawer* imgui_drawer() { return &drawer; }
  void OnDraw(ImGuiIO&);
};
@@DRAW@@
}
struct InputPolicy {
  bool debug_overlay_=false, console_overlay_=false, settings_overlay_=false,
       achievements_overlay_=false;
  rex::ui::Drawer drawer;
  rex::ui::Drawer* imgui_drawer_=&drawer;
  bool active() { return (@@CALLBACK@@)(); }
};
using u32=uint32_t; using mapped_u32=u32*;
constexpr u32 X_ERROR_SUCCESS=0, X_E_INVALIDARG=87, X_ERROR_NO_MORE_FILES=18;
#define assert_not_null(x) ((void)0)
#define XFAILED(x) ((x)!=0)
#define REXKRNL_DEBUG(...) ((void)0)
enum class DummyDeviceId : u32 { HDD=1, ODD=2 };
struct DeviceInfo { DummyDeviceId device_id; };
DeviceInfo* GetDummyDeviceInfo(u32 id) {
  static DeviceInfo h{DummyDeviceId::HDD}, o{DummyDeviceId::ODD};
  return id==1?&h:(id==2?&o:nullptr);
}
enum class XContentType : u32 { Saved=1 };
struct XCONTENT_DATA { char name[64]{}; };
struct XCONTENT_AGGREGATE_DATA : XCONTENT_DATA {
  std::string file_name() const { return name; }
};
struct Manager {
  std::vector<XCONTENT_AGGREGATE_DATA> user, common;
  auto ListContent(u32, uint64_t uid, XContentType) { return uid?user:common; }
};
struct Kernel {
  struct Profile { uint64_t id=1; uint64_t xuid(){return id;} } profile;
  Manager manager;
  auto user_profile(){return &profile;}
  auto content_manager(){return &manager;}
} kernel;
#define REX_KERNEL_STATE() (&kernel)
class XStaticUntypedEnumerator {
 public:
  XStaticUntypedEnumerator(Kernel*, size_t batch, size_t size):batch_(batch),size_(size){}
  u32 Initialize(u32,u32,u32,u32,u32){return 0;}
  size_t item_size() const {return size_;}
  size_t items_per_enumerate() const {return batch_;}
  size_t item_count() const {return item_count_;}
  u32 handle() const{return 123;}
  uint8_t* AppendItem();
  u32 WriteItems(u32,uint8_t*,u32*);
  size_t batch_,size_,item_count_=0,current_item_=0;
  std::vector<uint8_t> buffer_;
};
@@APPEND@@
@@WRITE@@
template<class T> struct XStaticEnumerator : XStaticUntypedEnumerator {
 XStaticEnumerator(Kernel*k,size_t n):XStaticUntypedEnumerator(k,n,sizeof(T)){}
 T* AppendItem(){return reinterpret_cast<T*>(XStaticUntypedEnumerator::AppendItem());}
};
std::shared_ptr<XStaticUntypedEnumerator> last;
template<class T, class... A> auto make_object(A... a) {
 auto e=std::make_shared<T>(a...); last=e; return e;
}
@@ENTRY@@
int main(int argc, char** argv) {
 bool only_policy=argc>1 && std::string(argv[1])=="policy";
 bool only_slots=argc>1 && std::string(argv[1])=="slots";
 if (!only_slots) {
  auto* ctx=ImGui::CreateContext(); auto& io=ImGui::GetIO();
  io.IniFilename=nullptr; io.DisplaySize={1280,720}; io.DeltaTime=1.0f/60;
  unsigned char* pixels; int width,height;
  io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
  rex::ui::DebugOverlayDialog overlay;
  InputPolicy policy;
  for (bool debug : {false,true}) for(bool console : {false,true})
  for(bool settings : {false,true}) for(bool achievements : {false,true})
  for(bool capture : {false,true}) {
   policy.debug_overlay_=debug; policy.console_overlay_=console;
   policy.settings_overlay_=settings; policy.achievements_overlay_=achievements;
   io.WantCaptureMouse=capture;
   check(policy.active()==(!(console||settings||achievements)||!capture),"input-policy capture failure");
  }
  // Enter/leave menus while F3 stays visible, including pointer over the meter.
  for (bool menu : {false,true,false,true,false}) {
   for(int frame=0;frame<6;frame++) {
    io.AddMousePosEvent(menu?420.0f:20.0f,menu?150.0f:35.0f);
    io.AddKeyEvent(ImGuiKey_W,frame%2==0);
    ImGui::NewFrame(); overlay.OnDraw(io);
    if(menu) {
      ImGui::SetNextWindowPos({400,100}); ImGui::SetNextWindowSize({300,250});
      ImGui::Begin("Interactive F4 fixture"); ImGui::Button("Settings"); ImGui::End();
    }
    ImGui::Render();
    if(frame>=3) {
      check(io.WantCaptureMouse==menu,"real ImGui mouse capture failure");
      if(!menu) check(!io.WantCaptureKeyboard,"passive F3 captured keyboard");
    }
   }
  }
  ImGui::DestroyContext(ctx);
  std::cout<<"32 input policy combinations; 30 real ImGui frames checked\n";
 }
 if(!only_policy) {
  std::vector<std::string> names={"appdata","install","savegame_1","savegame_2","savegame_3"};
  std::sort(names.begin(),names.end()); int cases=0;
  do {
   for(uint64_t uid : {uint64_t(0),uint64_t(1)}) {
    kernel.profile.id=uid;kernel.manager.user.clear();kernel.manager.common.clear();
    auto& list=uid?kernel.manager.user:kernel.manager.common;
    for(auto& name:names) { XCONTENT_AGGREGATE_DATA d;std::strcpy(d.name,name.c_str());list.push_back(d); }
    u32 bytes=0,handle=0;check(XamContentCreateEnumerator_entry(0,1,1,0,4,&bytes,&handle)==0,"enumerator failed");
    check(bytes==4*sizeof(XCONTENT_DATA)&&handle==123,"enumerator ABI/buffer failed");
    std::array<XCONTENT_DATA,4> out;u32 count=0;
    check(last->WriteItems(0,reinterpret_cast<uint8_t*>(out.data()),&count)==0&&count==4,"first batch size failed");
    std::vector<std::string> expected;for(auto& n:names)if(n!="install")expected.push_back(n);
    for(int i=0;i<4;i++)check(out[i].name==expected[i],"save missing or stable order changed");
    check(last->WriteItems(0,reinterpret_cast<uint8_t*>(out.data()),&count)==0&&count==1&&std::string(out[0].name)=="install","install cache lost");
    check(last->WriteItems(0,reinterpret_cast<uint8_t*>(out.data()),&count)==X_ERROR_NO_MORE_FILES,"enumerator end failed");
    ++cases;
   }
  } while(std::next_permutation(names.begin(),names.end()));
  u32 bytes=999,handle=0;
  check(XamContentCreateEnumerator_entry(0,99,1,0,4,&bytes,&handle)==X_E_INVALIDARG&&bytes==0,"invalid device regression");
  check(XamContentCreateEnumerator_entry(0,2,1,0,4,&bytes,&handle)==0&&last->item_count()==0,"ODD filtering regression");
  std::cout<<cases<<" production enumerator permutations/batches checked\n";
 }
 return failures?23:0;
}
'''
    for key, value in {'DRAW':draw, 'ENTRY':entry, 'CALLBACK':callback,
                       'APPEND':append, 'WRITE':write}.items():
        cpp = cpp.replace('@@'+key+'@@', value)
    source = args.output/'native_input_updates.cpp'
    source.write_text(cpp)
    imgui = sdk/'thirdparty/imgui'
    exe = args.output/'native_input_updates.exe'
    command = ['clang-cl','/nologo','/std:c++20','/EHsc','/O2','/MT',
               '/I'+str(imgui), str(source)]
    command += [str(imgui/(name+'.cpp')) for name in
                ['imgui','imgui_draw','imgui_tables','imgui_widgets']]
    command += ['/Fe:'+str(exe),'/Fo'+str(args.output)+'/', '/link']
    subprocess.run(command,check=True)
    results = {}
    for test in ['policy','slots']:
        run = subprocess.run([str(exe),test],capture_output=True,text=True,timeout=30)
        (args.output/(test+'.log')).write_text(run.stdout+run.stderr)
        results[test]={'exit_code':run.returncode,'output':run.stdout+run.stderr}
        print(test,run.returncode,run.stdout+run.stderr)
    report={'negative_control':bool(args.before),'results':results,
            'generated_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
            'scope':'production bodies; real ImGui; controlled kernel/VFS/object allocation; no game'}
    (args.output/'verification.json').write_text(json.dumps(report,indent=2))
    if args.before:
        assert all(item['exit_code']==23 for item in results.values()),report
    else:
        assert all(item['exit_code']==0 for item in results.values()),report


if __name__=='__main__':
    main()
