"""Actual native runtime/coordinator/decoder with injected SDK/window/device boundaries; never launches a game or window."""
from pathlib import Path
import argparse,hashlib,json,subprocess,os

def main():
 p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
 src=Path(__file__).resolve().parents[2]/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 mock=out/'mock';mock.mkdir(exist_ok=True)
 stubs={
 'windows.h':r'''#pragma once
#include <cstdint>
using HWND=void*;using DPI_AWARENESS_CONTEXT=void*;using LONG_PTR=intptr_t;
struct RECT{long left,top,right,bottom;};struct POINT{long x,y;};struct MONITORINFO{unsigned cbSize;RECT rcMonitor;};
constexpr int GWL_STYLE=-16,MONITOR_DEFAULTTONEAREST=2;constexpr LONG_PTR WS_POPUP=0x80000000,WS_OVERLAPPEDWINDOW=0x00cf0000,WS_MINIMIZEBOX=0x00020000,WS_CAPTION=0x00c00000,WS_THICKFRAME=0x00040000;
struct MockWindow{bool valid=true,iconic=false;LONG_PTR style=WS_OVERLAPPEDWINDOW;int width=1280,height=720,origin_x=0,origin_y=0;};inline MockWindow mock_window;
inline bool IsWindow(HWND h){return h&&mock_window.valid;}inline bool IsIconic(HWND){return mock_window.iconic;}
inline void* GetWindowDpiAwarenessContext(HWND){return (void*)1;}inline void* SetThreadDpiAwarenessContext(void*){return (void*)1;}
inline bool GetClientRect(HWND,RECT* r){*r={0,0,mock_window.width,mock_window.height};return true;}
inline bool ClientToScreen(HWND,POINT* p){*p={mock_window.origin_x,mock_window.origin_y};return true;}
inline LONG_PTR GetWindowLongPtrW(HWND,int){return mock_window.style;}inline void* MonitorFromWindow(HWND,int){return (void*)1;}
inline bool GetMonitorInfoW(void*,MONITORINFO* m){m->rcMonitor={0,0,1920,1080};return true;}
''',
 'rex/ui/window.h':r'''#pragma once
#include <windows.h>
namespace rex::ui{class Window{public:bool fullscreen=false;bool HasActualState()const{return mock_window.valid;}
void* GetNativeWindowHandle()const{return (void*)1;}unsigned GetActualPhysicalWidth()const{return mock_window.width;}
unsigned GetActualPhysicalHeight()const{return mock_window.height;}bool IsFullscreen()const{return fullscreen;}};}
''',
 'rex/cvar.h':r'''#pragma once
#include <map>
#include <string>
#include <string_view>
namespace rex::cvar{inline std::map<std::string,std::string> values;
inline std::string GetFlagByName(std::string_view key){return values[std::string(key)];}
template<class T>T Query(std::string_view key){return GetFlagByName(key)=="true";}}
''',
 'rex/logging.h':'#pragma once\n#define REXLOG_INFO(...) ((void)0)\n',
 'gpu_native/device.h':'#pragma once\n#include <cstdint>\nnamespace legodimensions::gpu_native{int32_t NativeInheritedAnisotropy();void SetNativeActiveAnisotropy(int32_t);}\n',
 'gpu_native/textures.h':'#pragma once\n#include "gpu_native/fsr3_settings.h"\nnamespace legodimensions::gpu_native{bool ReconfigureNativeSceneSurfaces(NativeRenderExtent);}\n',
 'gpu_native/temporal_present.h':'''#pragma once
#include <memory>
#include <cstdint>
namespace legodimensions::gpu_native{struct PreparedTemporalSettings{};
std::shared_ptr<PreparedTemporalSettings> PrepareTemporalSettings(uint32_t,uint32_t,bool);
bool CanCommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>&);
bool CommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>&,bool=false);bool TemporalDepthInverted();}
''',
 'gpu_native/fsr3_settings.h':'''#pragma once
#include <cstdint>
namespace legodimensions::gpu_native{struct Fsr3Size{uint32_t width,height;};
struct NativeRenderExtent{uint32_t guest_width,guest_height,host_width,host_height;};
struct NativeFsr3DetailSettings{bool sharpen;float sharpness,mip_bias_adjustment;};
struct NativeFsr3Request{uint32_t mode,scale;NativeFsr3DetailSettings details;};
enum class NativeFsr3ApplyResult{kUnchanged,kApplied,kDeferred};
bool Native720PRequested();uint32_t NativeResolutionScaleStep(int32_t);Fsr3Size NativeScaledSceneSize(uint32_t);
Fsr3Size NativeFsr3RequestRenderSize(NativeFsr3Request);
NativeFsr3ApplyResult TryApplyNativeFsr3Settings(NativeFsr3Request,bool(*)(NativeRenderExtent));}
'''}
 for name,text in stubs.items():
  dest=mock/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(text)
 flags=['-std=c++23','-O2','-I'+str(mock),'-I'+str(src)]
 fixture=Path(__file__).with_suffix('.cpp');production=src/'gpu_native/settings_runtime.cpp'
 objects=[]
 for source in [fixture,src/'settings_revision.cpp',src/'settings_transaction.cpp',src/'settings_service.cpp',src/'gpu_native/settings_snapshot.cpp']:
  obj=out/(source.stem+'.obj');r=subprocess.run(['clang++',*flags,'-c',str(source),'-o',str(obj)],capture_output=True,text=True,timeout=60)
  (out/(source.stem+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();objects.append(obj)
 env=os.environ.copy()
 for key in list(env):
  if key.startswith('LEGO_NATIVE_'):env.pop(key)
 def run(label,text):
  source=out/(label+'.cpp');source.write_text(text);exe=out/(label+'.exe')
  r=subprocess.run(['clang++',*flags,str(source),*map(str,objects),'-o',str(exe)],capture_output=True,text=True,timeout=60)
  (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
  r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=20)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);return r
 text=production.read_text();r=run('production',text);r.check_returncode();print(r.stdout,end='',flush=True)
 negatives=[]
 for label,old,new in [
  ('acknowledge-failed-effect','frame && effects_ready','frame'),
  ('skip-window-style','borderless != desired->fullscreen','false'),
  ('reject-sdl-minimize-style','!(style & (WS_CAPTION | WS_THICKFRAME))','!(style & WS_OVERLAPPEDWINDOW)'),
  ('skip-monitor-extent','width != uint32_t(monitor.rcMonitor.right-monitor.rcMonitor.left)','false'),
  ('ignore-scene-failure','if (result == NativeFsr3ApplyResult::kDeferred)','if (false)'),
 ]:
  assert text.count(old)==1,(label,text.count(old));r=run(label,text.replace(old,new));assert r.returncode==23,(label,r.returncode,r.stderr)
  negatives.append(dict(name=label,exit=r.returncode,error=r.stderr))
 files=[production,production.with_suffix('.h'),src/'settings_revision.cpp',src/'settings_revision.h',src/'settings_transaction.cpp',src/'settings_transaction.h',src/'settings_service.cpp',src/'settings_service.h',src/'gpu_native/settings_snapshot.cpp',src/'gpu_native/settings_snapshot.h',fixture,Path(__file__).resolve()]
 (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,negative=negatives,
  sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
  scope=__doc__,limits=['SDK registry, Win32 window and scene/temporal allocation boundaries are injected. Actual production runtime/coordinator/decoder execute; no physical window, game, compositor or GPU result is asserted.']),indent=2))
if __name__=='__main__':main()
