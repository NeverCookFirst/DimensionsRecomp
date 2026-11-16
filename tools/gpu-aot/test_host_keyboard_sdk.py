"""Exercise the app key handler and delivered SDK event chain without an OS window."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--previous-app',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=root/'rexlego/src/legodimensions_app.h';text=app.read_text()
start=text.index('  void OnKeyDown(rex::ui::KeyEvent& e) override {');end=text.index('{',start)+1;depth=1
while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
handler=text[start:end]
start=text.index('host.on_frame = ');end=text.index('{',start)+1;depth=1
while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
frame_callback=text[start:end+1]
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(root.parent/'build/native-menu-candidate-v22'/name,out/name)
code=r'''
#define NOMINMAX
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/keybinds.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/device_assignment.h>
#include <rex/cvar.h>
#include <imgui.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace rex::ui;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
namespace legodimensions::gpu_native {bool UsePm4Reference(){return false;}}
struct FakeDrawer{ImGuiIO&GetIO(){return ImGui::GetIO();}};
struct FrameService{void AdvanceFromUI(){}};
struct Context:WindowedAppContext {
 void NotifyUILoopOfPendingFunctions()override{}
 void PlatformQuitFromUIThread()override{}
};
struct TestWindow:Window {
 TestWindow(WindowedAppContext&c):Window(c,"Offline keyboard",320,240){}
 ~TestWindow()override{EnterDestructor();}
 bool OpenImpl()override{return true;}
 void RequestCloseImpl()override{WindowDestructionReceiver receiver(this);OnBeforeClose(receiver);OnAfterClose();}
 std::unique_ptr<Surface>CreateSurfaceImpl(Surface::TypeFlags)override{return {};}
 void RequestPaintImpl()override{}
 bool Key(VirtualKey key,bool down=true,bool repeat=false,bool alt=false){
  KeyEvent e(this,key,1,repeat,false,false,alt,false);WindowDestructionReceiver receiver(this);
  if(down)OnKeyDown(e,receiver);else OnKeyUp(e,receiver);return e.is_handled();
 }
 void Focus(bool value){WindowDestructionReceiver receiver(this);OnFocusUpdate(value,receiver);}
};
struct App:WindowInputListener {
 TestWindow* win;unsigned fullscreen=0;
 bool sdk_open=false;
 std::unique_ptr<int> mod_menu_,cheat_menu_,native_graphics_menu_;
 std::shared_ptr<FrameService> native_settings_runtime_,native_settings_;
 explicit App(TestWindow&w):win(&w){}
 Window* window(){return win;}
 bool AnyOverlayOpen(){return sdk_open;}
 bool RequestFullscreen(bool){++fullscreen;return true;}
 std::function<void(bool)> Frame(FakeDrawer*drawer){struct Host{std::function<void(bool)>on_frame;}host;FRAME_CALLBACK return host.on_frame;}
 HANDLER
};
struct Observer:WindowInputListener {
 unsigned downs=0,ups=0;VirtualKey last=VirtualKey::kNone;
 void OnKeyDown(KeyEvent&e)override{++downs;last=e.virtual_key();}
 void OnKeyUp(KeyEvent&)override{++ups;}
};
int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 Context context;TestWindow win(context);CHECK(win.Open());win.Focus(true);
 App app(win);Observer observer;win.AddInputListener(&observer,0);win.AddInputListener(&app,1);
 unsigned f4=0;rex::ui::RegisterBind("fixture_f4","F4","Offline F4",[&]{++f4;});
 CHECK(win.Key(VirtualKey::kF4)&&f4==1&&observer.downs==0);
 CHECK(!win.Key(VirtualKey::kW)&&observer.downs==1&&observer.last==VirtualKey::kW);
 CHECK(!win.Key(VirtualKey::kW,false)&&observer.ups==1);
 CHECK(win.Key(VirtualKey::kReturn,true,false,true)&&app.fullscreen==1);
 CHECK(win.Key(VirtualKey::kReturn,true,true,true)&&app.fullscreen==1);
 win.RemoveInputListener(&observer);
 rex::input::InputSystem input(&win);CHECK(input.Setup()==0);
 auto driver=std::make_unique<rex::input::mnk::MnkInputDriver>(&win,0);CHECK(driver->Setup()==0);
 input.AddDriver(std::move(driver));input.SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());input.AttachWindow(&win);
 auto poll=[&]{rex::input::X_INPUT_STATE state{};CHECK(input.GetState(0,&state)==0);return state;};
 win.Key(VirtualKey::kW);CHECK(poll().gamepad.thumb_ly>0);win.Key(VirtualKey::kW,false);CHECK(poll().gamepad.thumb_ly==0);
 win.Key(VirtualKey::kJ);CHECK(poll().gamepad.buttons&rex::input::X_INPUT_GAMEPAD_A);win.Key(VirtualKey::kJ,false);
 win.Focus(false);win.Key(VirtualKey::kW);CHECK(poll().gamepad.thumb_ly==0);win.Focus(true);
 win.Key(VirtualKey::kW);CHECK(poll().gamepad.thumb_ly>0);win.Key(VirtualKey::kW,false);
 CHECK(win.Key(VirtualKey::kF4)&&f4==2);
 ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={640,480};io.DeltaTime=1.f/60;
 unsigned char*pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
 FakeDrawer drawer;auto frame=app.Frame(&drawer);win.Key(VirtualKey::kW);
 // F3 is a real passive ImGui window: its presence must not zero actual
 // SDK InputSystem/MNK movement, even when the pointer lies over the meter.
 app.sdk_open=true;io.AddMousePosEvent(30,30);
 for(int i=0;i<5;++i){frame(false);ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({240,100});
  ImGui::Begin("Passive F3 fixture",nullptr,ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing);ImGui::TextUnformatted("Frame (ms)");ImGui::End();ImGui::Render();}
 CHECK(!io.WantCaptureMouse&&!io.WantCaptureKeyboard);frame(false);CHECK(poll().gamepad.thumb_ly>0);
 // Interactive SDK menus still withhold gameplay, and release it on close.
 io.WantCaptureKeyboard=true;frame(false);CHECK(poll().gamepad.thumb_ly==0);
 io.WantCaptureKeyboard=false;io.WantCaptureMouse=true;frame(false);CHECK(poll().gamepad.thumb_ly==0);
 app.sdk_open=false;io.WantCaptureMouse=false;frame(false);CHECK(poll().gamepad.thumb_ly>0);
 for(unsigned owner=0;owner<4;++owner){if(owner==0)app.mod_menu_=std::make_unique<int>();if(owner==1)app.cheat_menu_=std::make_unique<int>();if(owner==2)app.native_graphics_menu_=std::make_unique<int>();
  frame(owner==3);CHECK(poll().gamepad.thumb_ly==0);app.mod_menu_.reset();app.cheat_menu_.reset();app.native_graphics_menu_.reset();frame(false);CHECK(poll().gamepad.thumb_ly>0);}
 win.Key(VirtualKey::kW,false);CHECK(poll().gamepad.thumb_ly==0);input.Shutdown();ImGui::DestroyContext();
 win.RemoveInputListener(&app);rex::ui::UnregisterBind("fixture_f4");
 std::puts("PASS delivered SDK keyboard/F4/MNK/focus and actual app gate: passive F3 permits movement; interactive/modal owners block then release; no game or OS window.");
}
'''.replace('HANDLER',handler).replace('FRAME_CALLBACK',frame_callback)
cpp=out/'host-keyboard.cpp';cpp.write_text(code);exe=out/'host-keyboard.exe'
r=subprocess.run(['clang++','-std=c++23','-O2','-DSPDLOG_FMT_EXTERNAL','-DLEGODIMENSIONS_NATIVE_GPU','-I'+str(sdk/'include'),str(cpp),str(sdk/'lib/rexruntime.lib'),'-o',str(exe)],capture_output=True,text=True,timeout=90)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
r=subprocess.run([str(exe)],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=20)
(out/'run.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
positive_output=r.stdout
previous=a.previous_app.read_text();start=previous.index('host.on_frame = ');end=previous.index('{',start)+1;depth=1
while depth:depth+=(previous[end]=='{')-(previous[end]=='}');end+=1
old_callback=previous[start:end+1]
negative=out/'old-overlay-gate.cpp';negative.write_text(code.replace(frame_callback,old_callback));negative_exe=out/'old-overlay-gate.exe'
r=subprocess.run(['clang++','-std=c++23','-O2','-DSPDLOG_FMT_EXTERNAL','-DLEGODIMENSIONS_NATIVE_GPU','-I'+str(sdk/'include'),str(negative),str(sdk/'lib/rexruntime.lib'),'-o',str(negative_exe)],capture_output=True,text=True,timeout=90)
(out/'old-overlay-gate-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
r=subprocess.run([str(negative_exe)],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=20)
(out/'old-overlay-gate.log').write_text(r.stdout+r.stderr);assert r.returncode==23 and 'poll().gamepad.thumb_ly>0' in r.stderr,(r.returncode,r.stdout,r.stderr)
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,visible_window=False,
 sources={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [app,Path(__file__)]},
 runtime_sha256=hashlib.sha256((out/'rexruntime.dll').read_bytes()).hexdigest(),
 output=positive_output,negative=dict(source=str(a.previous_app),source_sha256=hashlib.sha256(a.previous_app.read_bytes()).hexdigest(),exit=r.returncode,error=r.stderr),
 limits=['Platform SDL/OS message delivery and actual ReXApp initialization are not exercised; event delivery begins at SDK Window::OnKeyDown.',
 'Passive stats uses real ImGui NoInputs flags; modal presence/capture states and host settings-service frame advance are injected. Actual SDK input routing/assignment/MNK/GetState and app input gate execute.']),indent=2))
