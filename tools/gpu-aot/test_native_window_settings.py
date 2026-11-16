"""Exercise actual app fullscreen paths and native transactions using the SDK DLL.

No app entry point, game, launcher, platform window or graphics device is run.
The window/event boundary is simulated; registry callbacks and settings code
are real. Extract app methods to catch a menu path bypassing the shared policy.
"""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess


def method(text, signature):
    begin = text.index(signature)
    end = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end].replace(' override', '')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    pkg, src = root.parent, root / 'rexlego/src'
    sdk = pkg / 'dev-assets/sdk-install'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    app = src / 'legodimensions_app.h'
    app_text = app.read_text()
    handler = method(app_text, '  void OnKeyDown(rex::ui::KeyEvent& e) override {')
    request = method(app_text, '  bool RequestFullscreen(bool fullscreen) {')
    begin = app_text.index('    auto settings_backend = legodimensions::settings::MakeNativeRegistryBackend(')
    end = app_text.index('\n    auto preferences =', begin)
    backend = app_text[begin:end]
    deps = [pkg / 'build/worlds-candidate-v18' / name for name in
            ['rexruntime.dll', 'amd_fidelityfx_dx12.dll']]
    for dep in deps:
        shutil.copy2(dep, out / dep.name)
    fixture = r'''
#include "window_mode.h"
#include "settings_preferences.h"
#include "settings_service.h"
#include <rex/cvar.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
using namespace legodimensions::settings;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#c); std::exit(23); } } while(0)
namespace rex::ui {
enum class VirtualKey { kReturn, kF4 };
struct KeyEvent {
 VirtualKey key=VirtualKey::kReturn;
 bool alt=true,ctrl=false,shift=false,super=false,repeat=false,handled=false;
 VirtualKey virtual_key()const{return key;}bool is_alt_pressed()const{return alt;}
 bool is_ctrl_pressed()const{return ctrl;}bool is_shift_pressed()const{return shift;}
 bool is_super_pressed()const{return super;}bool prev_state()const{return repeat;}
 void set_handled(bool value){handled=value;}
};
unsigned delegated=0;
void ProcessKeyEvent(KeyEvent&){++delegated;}
}
struct Window {
 bool fullscreen=false,throw_after_change=false;
 unsigned width=900,height=600,monitor=3,changes=0,resizes=0;
 bool IsFullscreen()const{return fullscreen;}bool HasFocus()const{return true;}
 unsigned GetActualLogicalWidth()const{return width;}unsigned GetActualLogicalHeight()const{return height;}
 void SetFullscreen(bool value){
  ++changes;fullscreen=value;if(value){width=1920;height=1080;}
  if(throw_after_change){throw_after_change=false;throw std::runtime_error("Platform callback failed after changing state");}
 }
 void SetDesiredLogicalSize(unsigned w,unsigned h){++resizes;width=w;height=h;}
};
struct App {
 Window win;bool has_window=true;legodimensions::WindowedSize windowed_size_;
 std::filesystem::path native_preferences_path_;
 Window* window(){return has_window?&win:nullptr;}
@REQUEST@
@HANDLER@
 Backend BackendForMenu(){
@BACKEND@
  return settings_backend;
 }
};
int main(int argc,char**argv){
 CHECK(argc==2);auto path=std::filesystem::path(argv[1])/"preferences.toml";
 Values values;std::vector<std::string> registered;
 for(const auto& spec:NativeSettingsCatalog()){
  std::string key(spec.key);if(!rex::cvar::GetFlagByName(key).empty())continue;
  values[key]=spec.default_value;
  rex::cvar::FlagEntry e;e.name=key;e.type=rex::cvar::FlagType::String;e.category="Test";e.description="Test";e.default_value=spec.default_value;
  e.getter=[&,key]{return values.at(key);};
  e.setter=[&,key](std::string_view value){values[key]=value;return true;};
  CHECK(rex::cvar::RegisterFlag(std::move(e)).has_value());registered.push_back(key);
 }
 for(const auto& [key,value]:NativeSettingsDefaults())CHECK(rex::cvar::SetFlagByName(key,value));
 rex::cvar::FinalizeInit();
 App app;app.native_preferences_path_=path;
 const auto owner=std::this_thread::get_id();unsigned callbacks=0,scenarios=0;
 rex::cvar::RegisterChangeCallback("fullscreen",[&](auto,auto value){
  CHECK(std::this_thread::get_id()==owner);++callbacks;app.win.SetFullscreen(value=="true");
 });
 auto reset=[&]{CHECK(app.RequestFullscreen(false));app.win=Window{};app.windowed_size_={};};
 auto backend=app.BackendForMenu();auto now=Clock::now();
 {
  Transaction t(backend,NativeSettingsConfirmationKeys());CHECK(t.Open()&&t.Edit("fullscreen","true"));
  CHECK(!app.win.fullscreen&&app.windowed_size_.width==0);++scenarios;
  CHECK(t.Apply(now)&&t.phase()==Phase::kPreview&&app.win.fullscreen);
  CHECK(app.windowed_size_.width==900&&app.windowed_size_.height==600&&app.win.monitor==3);++scenarios;
  t.Tick(t.deadline());CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kReverted);
  CHECK(!app.win.fullscreen&&app.win.width==900&&app.win.height==600&&!std::filesystem::exists(path));++scenarios;
  CHECK(t.Edit("fullscreen","true")&&t.Apply(now));
  CHECK(t.Keep(now)&&t.notice()==Notice::kSaved&&std::filesystem::exists(path));++scenarios;
  rex::ui::KeyEvent event;app.OnKeyDown(event);
  CHECK(event.handled&&!app.win.fullscreen&&app.win.width==900&&app.win.height==600);++scenarios;
 }
 reset();
 {
  rex::ui::KeyEvent event;app.OnKeyDown(event);CHECK(app.win.fullscreen&&app.windowed_size_.width==900);++scenarios;
  auto before=callbacks;event.repeat=true;app.OnKeyDown(event);CHECK(callbacks==before&&app.win.fullscreen);++scenarios;
  Transaction t(backend,NativeSettingsConfirmationKeys());CHECK(t.Open()&&t.Edit("fullscreen","false")&&t.Apply(now));
  CHECK(!app.win.fullscreen&&app.win.width==900&&app.win.height==600);++scenarios;
  CHECK(t.Revert()&&app.win.fullscreen&&app.windowed_size_.width==900);++scenarios;
  event.repeat=false;app.OnKeyDown(event);CHECK(!app.win.fullscreen&&app.win.width==900&&app.win.height==600);++scenarios;
 }
 reset();
 {
  std::deque<std::function<void()>> queue;
  auto service=std::make_shared<QueuedSettings>(app.BackendForMenu(),[&](auto f){queue.push_back(std::move(f));return true;},
    NativeSettingsDefaults(),NativeSettingsConfirmationKeys(),[&]{return now;});
  auto command=[&](Command cmd){std::optional<std::future<Reply>> result;std::thread guest([&]{result.emplace(service->Submit(cmd));});guest.join();
    CHECK(result->wait_for(std::chrono::seconds(0))==std::future_status::timeout);
    CHECK(queue.size()==1);auto fn=std::move(queue.front());queue.pop_front();fn();return result->get();};
  CHECK(command({CommandKind::kOpen}).ok&&command({CommandKind::kEdit,"fullscreen","true"}).ok);
  CHECK(!app.win.fullscreen);CHECK(command({CommandKind::kApply}).ok&&app.win.fullscreen);++scenarios;
  auto reply=service->FocusLostFromUI();CHECK(reply.ok&&!app.win.fullscreen&&app.win.width==900&&app.win.height==600);++scenarios;
  CHECK(command({CommandKind::kEdit,"fullscreen","true"}).ok&&command({CommandKind::kApply}).ok);
  CHECK(service->ShutdownFromUI().ok&&!app.win.fullscreen&&app.win.width==900&&app.win.height==600);++scenarios;
 }
 reset();
 {
  app.win.throw_after_change=true;Transaction t(backend,NativeSettingsConfirmationKeys());CHECK(t.Open()&&t.Edit("fullscreen","true"));
  CHECK(!t.Apply(now)&&t.notice()==Notice::kApplyFailed&&t.phase()==Phase::kEditing);
  CHECK(!app.win.fullscreen&&app.win.width==900&&app.win.height==600&&backend.read().at("fullscreen")=="false");++scenarios;
  CHECK(app.RequestFullscreen(true));app.win.throw_after_change=true;
  Transaction leaving(backend,NativeSettingsConfirmationKeys());CHECK(leaving.Open()&&leaving.Edit("fullscreen","false"));
  CHECK(!leaving.Apply(now)&&leaving.phase()==Phase::kEditing&&app.win.fullscreen);
  CHECK(app.windowed_size_.width==900&&app.windowed_size_.height==600);++scenarios;
 }
 reset();
 {
  CHECK(app.RequestFullscreen(true));auto saved=app.windowed_size_;CHECK(app.RequestFullscreen(true));
  CHECK(saved.width==app.windowed_size_.width&&saved.height==app.windowed_size_.height);++scenarios;
  CHECK(app.RequestFullscreen(false));app.win.width=1100;app.win.height=700;
  CHECK(app.RequestFullscreen(true)&&app.RequestFullscreen(false));CHECK(app.win.width==1100&&app.win.height==700);++scenarios;
  auto resizes=app.win.resizes;CHECK(app.RequestFullscreen(false));CHECK(app.win.resizes==resizes);++scenarios;
  app.win.width=0;app.win.height=0;CHECK(app.RequestFullscreen(true)&&app.RequestFullscreen(false));
  CHECK(app.win.width==1100&&app.win.height==700);++scenarios;
 }
 reset();
 {
  CHECK(app.RequestFullscreen(true));app.windowed_size_={};auto resizes=app.win.resizes;
  CHECK(app.RequestFullscreen(false)&&app.win.resizes==resizes);++scenarios;
  app.win.width=0;app.win.height=0;CHECK(app.RequestFullscreen(true)&&app.RequestFullscreen(false));
  CHECK(!app.windowed_size_.width&&!app.windowed_size_.height&&app.win.resizes==resizes);++scenarios;
 }
 reset();
 {
  auto before=callbacks;bool denied=false;std::thread guest([&]{try{backend.write("fullscreen","true");}catch(const std::exception&){denied=true;}});guest.join();
  CHECK(denied&&callbacks==before&&!app.win.fullscreen&&!app.windowed_size_.width);++scenarios;
  CHECK(!backend.write("unknown","true")&&callbacks==before);++scenarios;
  app.has_window=false;CHECK(!backend.write("fullscreen","true")&&callbacks==before&&backend.read().at("fullscreen")=="false");
  rex::ui::KeyEvent event;app.OnKeyDown(event);CHECK(event.handled&&callbacks==before);app.has_window=true;++scenarios;
  rex::ui::KeyEvent modified;modified.ctrl=true;app.OnKeyDown(modified);CHECK(!modified.handled&&rex::ui::delegated==1&&callbacks==before);++scenarios;
 }
 reset();
 {
  std::ofstream file(path);file<<"fullscreen = true\n";file.close();
  auto restored=LoadNativePreferences(path,backend);CHECK(restored.ok&&restored.applied.at("fullscreen")=="true");
  CHECK(app.win.fullscreen&&app.windowed_size_.width==900&&app.windowed_size_.height==600);++scenarios;
  CHECK(app.RequestFullscreen(false)&&app.win.width==900&&app.win.height==600);++scenarios;
 }
 // Rejected callbacks do not themselves change a window. Successful-looking
 // desired-state changes are deliberately not treated as a physical ack.
 reset();
 {
  bool requested=false;
  CHECK(!legodimensions::RequestBorderless(app.win,app.windowed_size_,true,[&](bool v){requested=v;return false;}));
  CHECK(requested&&!app.win.fullscreen&&app.windowed_size_.width==900);++scenarios;
 }
 rex::cvar::UnregisterChangeCallbacks("fullscreen");
 for(const auto& key:registered)rex::cvar::UnregisterFlag(key);
 std::printf("PASS %u native window settings scenarios through real SDK callbacks and extracted app methods\n",scenarios);
}
'''

    env = os.environ.copy()
    for name in ['LEGO_NATIVE_FPS_LIMIT', 'LEGO_NATIVE_VSYNC', 'LEGO_NATIVE_FSR3', 'LEGO_NATIVE_FSR3_QUALITY', 'LEGO_NATIVE_720P']:
        env.pop(name, None)

    # Only the extracted app methods/header vary for the fault controls.
    # Compile their unchanged production backend/service units once.
    common_flags = ['-std=c++20', '-O2', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-Wno-deprecated-literal-operator',
                    '-I' + str(src), '-I' + str(sdk / 'include')]
    objects = []
    for name in ['settings_registry', 'settings_transaction', 'settings_config', 'settings_service', 'settings_preferences']:
        obj = out / (name + '.obj')
        compiled = subprocess.run([args.compiler, *common_flags, '-c', str(src / (name + '.cpp')), '-o', str(obj)],
                                  capture_output=True, text=True, timeout=90)
        (out / (name + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        objects.append(obj)

    def run(label, header, app_request=request, app_backend=backend):
        folder = out / label
        folder.mkdir(exist_ok=False)
        (folder / 'window_mode.h').write_text(header)
        source = folder / 'window-settings.cpp'
        source.write_text(fixture.replace('@REQUEST@', app_request).replace('@HANDLER@', handler).replace('@BACKEND@', app_backend))
        exe = out / (label + '.exe')
        command = [args.compiler, '-I' + str(folder), *common_flags, str(source), *map(str, objects),
                   str(sdk / 'lib/rexruntime.lib'), '-o', str(exe)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=90)
        (folder / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        result = subprocess.run([str(exe), str(folder)], capture_output=True, text=True, env=env, timeout=20)
        (folder / 'run.log').write_text(result.stdout + result.stderr)
        return result

    header = (src / 'window_mode.h').read_text()
    positive = run('production', header)
    positive.check_returncode()
    print(positive.stdout, end='')
    negative = []
    for label, old, new in [
        ('no-capture', 'if (!was_fullscreen && fullscreen)', 'if (false)'),
        ('no-restore', 'if (was_fullscreen && !fullscreen && !window.IsFullscreen() && saved.width && saved.height)', 'if (false)'),
        ('overwrite-fullscreen-size', 'if (!was_fullscreen && fullscreen)', 'if (fullscreen)'),
    ]:
        assert header.count(old) == 1
        result = run(label, header.replace(old, new))
        assert result.returncode == 23, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    old = 'if (key == "fullscreen") return RequestFullscreen(value == "true");'
    assert backend.count(old) == 1
    result = run('menu-bypasses-policy', header, app_backend=backend.replace(old, ''))
    assert result.returncode == 23, result.stderr
    negative.append(dict(name='menu-bypasses-policy', exit=result.returncode, error=result.stderr))
    files = [app, src / 'window_mode.h', src / 'settings_registry.h', src / 'settings_registry.cpp',
             src / 'settings_transaction.h', src / 'settings_transaction.cpp', src / 'settings_service.h', src / 'settings_service.cpp',
             src / 'settings_preferences.h', src / 'settings_preferences.cpp', src / 'settings_config.h', src / 'settings_config.cpp',
             Path(__file__).resolve(), *deps]
    (out / 'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=positive.stdout,
        negative=negative, sources={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
        scope='Actual app fullscreen helper, Alt+Enter handler and menu-backend construction extracted from live source; actual native settings registry/transaction/service/preferences and real SDK DLL callbacks. Simulated window, UI queue and keyboard events; no platform window, renderer acknowledgement, fullscreen FPS or physical mode acceptance.'), indent=2))


if __name__ == '__main__':
    main()
