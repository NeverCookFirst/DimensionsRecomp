"""Run the actual player Quit callback against the SDK's window lifecycle.

No game, launcher, GPU or visible OS window. Application cleanup is injected;
the SDK context/window/OnClosing path executes against the delivered DLL.
Platform Open/Close are injected because this DLL has no SDL dummy driver.
"""
from pathlib import Path
import argparse,subprocess,hashlib,json,os,shutil
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
src=root/'rexlego/src/legodimensions_app.h';s=src.read_text();start=s.index('host.quit = [this]');end=s.index('};',start)+2;callback=s[start:end]
assert 'OnWindowCloseRequested()' in callback and 'win->RequestClose()' in callback
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(root.parent/'build/native-menu-candidate-v20'/name,out/name)
env=dict(os.environ,SDL_VIDEODRIVER='dummy',LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll'))
code=r'''
#define NOMINMAX
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/window_listener.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
using namespace rex::ui;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct Context:WindowedAppContext {
 unsigned quits=0;
 void NotifyUILoopOfPendingFunctions()override{}
 void PlatformQuitFromUIThread()override{++quits;}
};
struct TestWindow:Window {
 TestWindow(WindowedAppContext&c):Window(c,"Offline Quit",320,240){}
 ~TestWindow()override{EnterDestructor();}
 bool OpenImpl()override{return true;}
 void RequestCloseImpl()override{WindowDestructionReceiver receiver(this);OnBeforeClose(receiver);CHECK(!receiver.IsWindowDestroyed());OnAfterClose();}
 std::unique_ptr<Surface>CreateSurfaceImpl(Surface::TypeFlags)override{return {};}
 void RequestPaintImpl()override{}
};
struct Harness:WindowListener {
 Context context;
 std::unique_ptr<Window> win;
 unsigned prepared=0,closed=0;bool allow=true;
 bool OnWindowCloseRequested(){++prepared;return allow;}
 Window* window(){return win.get();}
 Context& app_context(){return context;}
 void OnClosing(UIEvent&)override{++closed;}
 std::function<void()> Build(){struct Host{std::function<void()>quit;}host;
 CALLBACK
 return host.quit;}
};
int main(){
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 Harness app;app.win=std::make_unique<TestWindow>(app.context);app.win->AddListener(&app);CHECK(app.win->Open());
 auto quit=app.Build();app.allow=false;quit();CHECK(app.prepared==1&&app.closed==0&&app.win->phase()==Window::Phase::kOpen);
 app.allow=true;quit();CHECK(app.prepared==2&&app.closed==1&&app.win->phase()!=Window::Phase::kOpen);
 app.win->RemoveListener(&app);app.win.reset();
 CHECK(app.context.quits==0);
 std::puts("PASS actual app Quit callback: preparation/veto, SDK RequestClose and OnClosing; platform backend injected, no game or window.");
}
'''
records=[]
for label,body in [('production',callback),('old-loop-quit','host.quit = [this] { app_context().QuitFromUIThread(); };')]:
 cpp=out/(label+'.cpp');exe=out/(label+'.exe');cpp.write_text(code.replace('CALLBACK',body))
 r=subprocess.run(['clang++','-std=c++23','-O2','-DSPDLOG_FMT_EXTERNAL','-I'+str(sdk/'include'),str(cpp),str(sdk/'lib/rexruntime.lib'),'-o',str(exe)],capture_output=True,text=True,timeout=90)
 (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],cwd=out,env=env,capture_output=True,text=True,timeout=20);(out/(label+'.log')).write_text(r.stdout+r.stderr)
 assert r.returncode==(0 if label=='production' else 23),(label,r.returncode,r.stdout,r.stderr)
 records.append(dict(label=label,exit=r.returncode,output=r.stdout,error=r.stderr))
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,visible_window=False,actual_sdk_window=True,records=records,
 sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [src,Path(__file__)]},runtime_sha256=hashlib.sha256((out/'rexruntime.dll').read_bytes()).hexdigest(),
 limits=['Platform Open/Close and cleanup are injected; SDK lifecycle/listener callbacks execute. Actual game TerminateTitle/worker shutdown requires owner close test.']),indent=2))
print(records[0]['output'])
