"""Execute actual app close preparation and SDK close listeners with a renderer-wait trap.

No game or OS window. Settings/toypad/render boundaries are injected; the
complete production close/StopNativeSettings/Quit bodies and SDK Window execute.
The production watchdog body is also executed in a separate process.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,time
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=root/'rexlego/src/legodimensions_app.h';text=app.read_text()
def block(signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
close=block('  bool OnWindowCloseRequested() override').replace(' override','')
stop=block('  void StopNativeSettings(')
watch=block('  static void StartShutdownWatchdog()').replace('StartShutdownWatchdog','RealWatchdog')
quit=block('host.quit = [this]')+';'
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(root.parent/'build/native-menu-candidate-v22'/name,out/name)
code=r'''
#define NOMINMAX
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/window_listener.h>
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
using namespace rex::ui;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#define REXLOG_WARN(...) ((void)0)
struct Counters{unsigned watchdog=0,drains=0,advances=0,stops=0,shutdowns=0,detaches=0,toypad=0;bool closing=false;}calls;
namespace rex {void FlushLogging(){}}
namespace legodimensions::gpu_native {
struct HostDevice {static int LockRecording(){CHECK(!calls.closing);++calls.drains;return 0;}};
void DetachNativeOverlays(){CHECK(calls.watchdog);++calls.detaches;}
}
namespace legodimensions::toypad_app {void StopIfStarted(){CHECK(calls.watchdog);++calls.toypad;}}
struct Reply{bool ok=true;std::string error;};
struct Service{Reply ShutdownFromUI(){CHECK(!calls.closing||calls.watchdog);++calls.shutdowns;return {};}};
struct Runtime{void AdvanceFromUI(){++calls.advances;}void StopFromUI(){++calls.stops;}};
namespace legodimensions::settings {void PublishNativeSettingsService(std::shared_ptr<Service>){}}
struct Context:WindowedAppContext{void NotifyUILoopOfPendingFunctions()override{}void PlatformQuitFromUIThread()override{CHECK(false);}};
struct TestWindow:Window {
 TestWindow(WindowedAppContext&c):Window(c,"Offline close",320,240){}
 ~TestWindow()override{EnterDestructor();}
 bool OpenImpl()override{return true;}
 void RequestCloseImpl()override{WindowDestructionReceiver receiver(this);OnBeforeClose(receiver);OnAfterClose();}
 std::unique_ptr<Surface>CreateSurfaceImpl(Surface::TypeFlags)override{return {};}
 void RequestPaintImpl()override{}
 void OSRequest(){WindowDestructionReceiver receiver(this);if(SendCloseRequestToListeners(receiver))RequestClose();}
};
struct Harness:WindowListener {
 Context context;std::unique_ptr<TestWindow>win;
 std::shared_ptr<Service>native_settings_=std::make_shared<Service>();
 std::shared_ptr<Runtime>native_settings_runtime_=std::make_shared<Runtime>();
 unsigned closed=0;
 Harness(){win=std::make_unique<TestWindow>(context);win->AddListener(this);CHECK(win->Open());}
 ~Harness(){win->RemoveListener(this);}
 Window*window(){return win.get();}
 static void StartShutdownWatchdog(){++calls.watchdog;}
 WATCHDOG
 CLOSE
 STOP
 bool OnCloseRequested(UIEvent&)override{return OnWindowCloseRequested();}
 void OnClosing(UIEvent&)override{++closed;}
 std::function<void()>Quit(){struct Host{std::function<void()>quit;}host;QUIT return host.quit;}
};
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 if(argc>1){Harness::RealWatchdog();std::this_thread::sleep_for(std::chrono::seconds(6));CHECK(false);}
 // Module reset still has its real drain boundary. It is deliberately absent
 // when closing, where a worker can be waiting on the UI thread itself.
 {Harness h;h.StopNativeSettings();CHECK(calls.drains==1&&calls.advances==1&&calls.stops==1);}
 for(bool os:{false,true}){calls={};Harness h;calls.closing=true;if(os)h.win->OSRequest();else h.Quit()();
  CHECK(h.closed==1&&h.win->phase()!=Window::Phase::kOpen);
  CHECK(calls.watchdog==1&&calls.drains==0&&calls.advances==0&&calls.stops==1&&calls.shutdowns==1&&calls.detaches==1&&calls.toypad==1);
  CHECK(!h.native_settings_&&!h.native_settings_runtime_);}
 std::puts("PASS actual app close/StopNativeSettings/Quit through SDK close listeners: no renderer drain, early watchdog, module-reset drain preserved; no game or OS window.");
}
'''.replace('WATCHDOG',watch).replace('CLOSE',close).replace('STOP',stop).replace('QUIT',quit)
records=[]
def run(label,source,argument=None):
 cpp=out/(label+'.cpp');exe=out/(label+'.exe');cpp.write_text(source)
 r=subprocess.run(['clang++','-std=c++23','-O2','-DSPDLOG_FMT_EXTERNAL','-DLEGODIMENSIONS_NATIVE_GPU','-D_CRT_SECURE_NO_WARNINGS','-I'+str(sdk/'include'),str(cpp),str(sdk/'lib/rexruntime.lib'),'-o',str(exe)],capture_output=True,text=True,timeout=90)
 (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
 start=time.monotonic();r=subprocess.run([str(exe),*([argument] if argument else [])],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=8)
 (out/(label+'.log')).write_text(r.stdout+r.stderr)
 record=dict(label=label,exit=r.returncode,seconds=time.monotonic()-start,output=r.stdout,error=r.stderr);records.append(record);return record
positive=run('production',code);assert positive['exit']==0,positive
negative=run('close-render-drain',code.replace('StopNativeSettings(true);','StopNativeSettings();'));assert negative['exit']==23 and '!calls.closing' in negative['error'],negative
late=code.replace('    StartShutdownWatchdog();','',1)
negative=run('late-watchdog',late);assert negative['exit']==23 and 'calls.watchdog' in negative['error'],negative
watchdog=run('watchdog-process',code,'wedged');assert watchdog['exit']==0 and 0.15<watchdog['seconds']<1.5,watchdog
slow=code.replace('std::chrono::milliseconds(250)','std::chrono::milliseconds(3000)')
assert slow!=code
negative=run('old-slow-native-watchdog',slow,'wedged');assert negative['exit']==0 and 2.5<negative['seconds']<6,negative
blocked_log=code.replace('TerminateProcess(GetCurrentProcess(), 0);','std::this_thread::sleep_for(std::chrono::seconds(3)); TerminateProcess(GetCurrentProcess(), 0);')
assert blocked_log!=code
negative=run('blocking-work-before-termination',blocked_log,'wedged');assert negative['exit']==0 and 2.5<negative['seconds']<6,negative
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,visible_window=False,records=records,
 sources={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [app,Path(__file__)]},runtime_sha256=hashlib.sha256((out/'rexruntime.dll').read_bytes()).hexdigest(),
 limits=['Settings service/runtime/toypad/platform backend are injected; actual GPU worker/guest-thread termination remains an owner test.',
 'OS close request is injected at SDK listener dispatch; no Alt+F4 key or platform window is generated. Watchdog subprocess runs the production Win32 whole-process termination backstop.',
 'The timing oracle rejects a three-second watchdog and blocking work before termination. No actual game shutdown or in-flight save is exercised.']),indent=2))
print(positive['output']);print('PASS native whole-process backstop exits the intentionally stalled fixture in %.2f seconds; slow/blocking variants rejected.'%watchdog['seconds'])
