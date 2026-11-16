"""Exercise production native requests and Plume Present through live changes.

No game, window or monitor-mode changes. The DXGI boundary records calls.
"""
from pathlib import Path
import argparse,subprocess,json,hashlib

def function(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
 p.add_argument('--compiler',default='clang-cl');a=p.parse_args()
 root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 app=(root/'rexlego/src/gpu_native/device.cpp').read_text()
 plume=(root/'thirdparty/plume/plume_d3d12.cpp').read_text()
 request=function(app,'bool NativeVsyncRequested()')
 present=function(plume,'bool D3D12SwapChain::present(')
 setter=function(plume,'void D3D12SwapChain::setVsyncEnabled(')
 theme=(root/'rexlego/src/ui_theme.cpp').read_text()
 assert '{"native_vsync", "VSync"' in theme
 assert 'REXCVAR_DEFINE_BOOL(native_vsync, false' in app
 assert 'const bool vsync_on = NativeVsyncRequested();' in app
 assert 'const bool requested_vsync = NativeVsyncRequested();' in app
 assert 'state.swap_chain->setVsyncEnabled(requested_vsync);' in app
 code=r'''
#include <cstdint>
#include <cstdlib>
#include <cstdio>
using UINT=uint32_t;using HRESULT=int32_t;
#define SUCCEEDED(x) ((x)>=0)
constexpr UINT DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING=2048,DXGI_PRESENT_ALLOW_TEARING=512;
namespace rex::cvar { bool selected=false; template<class T>T Query(const char* name) {
 if(std::string_view(name)!="native_vsync")std::exit(24);return selected; } }
struct RenderCommandSemaphore {};
struct DXGI { UINT sync=99,flags=99,calls=0;HRESULT result=0;
 HRESULT Present(UINT s,UINT f){sync=s;flags=f;++calls;return result;} };
struct D3D12SwapChain { bool vsyncEnabled=true;UINT swapChainFlags=0;
 struct {bool enablePresentWait=false;}desc;DXGI* d3d;
 bool present(uint32_t,RenderCommandSemaphore**,uint32_t);
 void setVsyncEnabled(bool);
};
static void Check(bool ok){if(!ok)std::exit(23);}
'''
 code='#include <string_view>\n'+code+request+'\n'+present+'\n'+setter+r'''
int main(){
 DXGI dxgi;D3D12SwapChain chain;chain.d3d=&dxgi;
 for(const char* env:{"","0","1"})for(bool registry:{false,true}){
  _putenv_s("LEGO_NATIVE_VSYNC",env);rex::cvar::selected=registry;
  const bool requested=*env?*env!='0':registry;
  Check(NativeVsyncRequested()==requested);
 }
 _putenv_s("LEGO_NATIVE_VSYNC","");
 unsigned cases=0;
 for(bool tearing:{false,true})for(bool wait:{false,true}){
  chain.swapChainFlags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
  chain.desc.enablePresentWait=wait;
  // Alternate the UI request repeatedly over simulated focus/resize transitions.
  // Focus and size never feed the requested synchronization policy.
  for(unsigned event=0;event<2000;++event){
   rex::cvar::selected=event%3==0;
   chain.setVsyncEnabled(NativeVsyncRequested());
   Check(chain.present(0,nullptr,0));
   Check(dxgi.sync==(rex::cvar::selected&&!wait?1u:0u));
   Check(dxgi.flags==(!rex::cvar::selected&&tearing?512u:0u));++cases;
  }
 }
 dxgi.result=-1;Check(!chain.present(0,nullptr,0));
 std::printf("PASS: %u live Present policies, UI requests, empty/0/1 environment overrides, uncapped Present arguments when off, failed-Present propagation\n",cases);
}
'''
 source=out/'native-vsync.cpp';source.write_text(code);exe=out/'native-vsync.exe'
 r=subprocess.run([a.compiler,str(source),'/std:c++20','/O2','/MD','/EHsc','/Fo'+out.as_posix()+'/',
   '/Fe'+str(exe)],capture_output=True,text=True,timeout=60)
 (out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
 (out/'result.log').write_text(r.stdout+r.stderr);r.check_returncode()
 (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,
  window_opened=False,actual_dxgi=False,scope='Production request and Plume call policy; mocked DXGI boundary, not fullscreen performance',
  source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),output=r.stdout),indent=2))
 print(r.stdout,end='')
if __name__=='__main__':main()
