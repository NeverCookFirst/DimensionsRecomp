"""Run the actual detached UI packet collector and async lifetime bridge."""
import argparse
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_frame_ring_transaction import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'rexlego/src/gpu_native/native_overlay.cpp').read_text()
private=s[s.index('struct Bridge {'):s.index('class GraphicsMenu final:')]
public='\n'.join(function(s,x)for x in ('std::unique_ptr<rex::ui::ImmediateDrawer> CreateNativeImmediateDrawer(',
 'void AttachNativeOverlays(', 'void DetachNativeOverlays(', 'void QueueNativeOverlayFrame(',
 'std::shared_ptr<const OverlayPacket> NativeOverlayPacket(', 'void SetNativeOverlayDeviceReady(',
 'void PublishNativeOverlayStats(', 'rex::ui::FrameStats NativeOverlayStats('))
app=(root/'rexlego/src/legodimensions_app.h').read_text();device=(root/'rexlego/src/gpu_native/device.cpp').read_text()
assert 'OnCreateImmediateDrawer() override' in app and 'AttachNativeOverlays(app_context(), drawer)' in app
assert app.count('DetachNativeOverlays()')>=2
present=function(device,'bool HostDevice::PresentTexture(')
assert present.index('commands->drawInstanced(3, 1, 0, 0);')<present.index('RenderNativeOverlays(')<present.index('RenderTextureLayout::PRESENT')
assert 'CallInUIThreadSynchronous' not in s and 'HostDevice::' not in s
cpp=out/'overlay.cpp';cpp.write_text(r'''
#include "gpu_native/overlay_packet.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>
#include <cstdio>
namespace rex::ui {
class UIDrawContext{public:UIDrawContext(uint32_t w,uint32_t h):w(w),h(h){}uint32_t render_target_width()const{return w;}uint32_t render_target_height()const{return h;}uint32_t w,h;};
class AppUIDrawContext:public UIDrawContext{public:using UIDrawContext::UIDrawContext;};
enum class ImmediateTextureFilter{kNearest,kLinear};
class ImmediateTexture{public:virtual~ImmediateTexture()=default;ImmediateTexture(uint32_t w,uint32_t h):width(w),height(h){}uint32_t width,height;};
enum class ImmediatePrimitiveType{kLines,kTriangles};
struct ImmediateVertex{float x,y,u,v;uint32_t color;};
struct ImmediateDrawBatch{const ImmediateVertex* vertices=nullptr;int vertex_count=0;const uint16_t*indices=nullptr;int index_count=0;};
struct ImmediateDraw{ImmediatePrimitiveType primitive_type=ImmediatePrimitiveType::kTriangles;int count=0,index_offset=0,base_vertex=0;ImmediateTexture*texture=nullptr;bool scissor=false;float scissor_left=0,scissor_top=0,scissor_right=0,scissor_bottom=0;};
class ImmediateDrawer{public:virtual~ImmediateDrawer()=default;virtual std::unique_ptr<ImmediateTexture>CreateTexture(uint32_t,uint32_t,ImmediateTextureFilter,bool,const uint8_t*)=0;
 virtual void Begin(UIDrawContext&,float,float){}virtual void BeginDrawBatch(const ImmediateDrawBatch&)=0;virtual void Draw(const ImmediateDraw&)=0;virtual void EndDrawBatch()=0;virtual void End(){};};
class ImGuiDrawer{public:bool open=true;int draws=0;std::function<void(UIDrawContext&)>function;bool HasDialogs()const{return open;}void Draw(UIDrawContext&ctx){++draws;function(ctx);}};
class WindowedAppContext{public:bool accept=true;std::vector<std::function<void()>>pending;bool CallInUIThreadDeferred(std::function<void()>fn){if(!accept)return false;pending.push_back(std::move(fn));return true;}
 void Run(){auto work=std::move(pending);pending.clear();for(auto&fn:work)fn();}};
struct FrameStats{double frame_time_ms,fps;uint64_t frame_count;};
}
namespace legodimensions::gpu_native {
void DetachNativeOverlays();
namespace{
''' + private + '\n}\n' + public + r'''
}
using namespace legodimensions::gpu_native;
int main(){
 rex::ui::WindowedAppContext context;rex::ui::ImGuiDrawer gui;auto drawer=CreateNativeImmediateDrawer();
 const uint8_t rgba[]{255,0,0,128};
 assert(!drawer->CreateTexture(1,1,rex::ui::ImmediateTextureFilter::kLinear,false,rgba));
 SetNativeOverlayDeviceReady(true);
 assert(!drawer->CreateTexture(8192,8192,rex::ui::ImmediateTextureFilter::kLinear,false,rgba));
 gui.function=[&](rex::ui::UIDrawContext&ctx){
  auto font=drawer->CreateTexture(1,1,rex::ui::ImmediateTextureFilter::kLinear,false,rgba);assert(font);
  drawer->Begin(ctx,64,64);const rex::ui::ImmediateVertex vertices[]{{0,0,0,0,~0u},{32,0,1,0,~0u},{0,32,0,1,~0u}};
  const uint16_t indices[]{0,1,2};drawer->BeginDrawBatch({vertices,3,indices,3});
  rex::ui::ImmediateDraw draw;draw.texture=font.get();draw.count=3;drawer->Draw(draw);
  draw.count=4;drawer->Draw(draw);draw.count=3;draw.base_vertex=-1;drawer->Draw(draw);
  drawer->EndDrawBatch();drawer->End();
 };
 AttachNativeOverlays(context,&gui);
 for(unsigned i=0;i<1000;++i)QueueNativeOverlayFrame(128+i,128);
 assert(context.pending.size()==1&&gui.draws==0);context.Run();
 auto first=NativeOverlayPacket();assert(first&&first->width==1127&&first->draws.size()==1);
 assert(first->draws[0].texture->rgba==std::vector<uint8_t>({255,0,0,128}));
 for(unsigned frame=0;frame<1000;++frame){QueueNativeOverlayFrame(128,128);context.Run();}
 assert(first->width==1127&&first->vertices[1].x==32);assert(gui.draws==1001);
 gui.open=false;QueueNativeOverlayFrame(128,128);context.Run();assert(!NativeOverlayPacket());
 gui.open=true;QueueNativeOverlayFrame(128,128);const int draws=gui.draws;DetachNativeOverlays();
 context.Run();assert(gui.draws==draws&&!NativeOverlayPacket());
 AttachNativeOverlays(context,&gui);context.accept=false;QueueNativeOverlayFrame(128,128);assert(context.pending.empty());
 context.accept=true;QueueNativeOverlayFrame(128,128);assert(context.pending.size()==1);context.Run();assert(NativeOverlayPacket());
 std::vector<std::thread> workers;for(unsigned i=0;i<8;++i)workers.emplace_back([]{for(unsigned j=0;j<1000;++j)QueueNativeOverlayFrame(128,128);});
 for(auto&worker:workers)worker.join();assert(context.pending.size()==1);context.Run();
 PublishNativeOverlayStats(120,8.333,42);auto stats=NativeOverlayStats();assert(stats.fps==120&&stats.frame_count==42);
 DetachNativeOverlays();SetNativeOverlayDeviceReady(false);
 std::puts("PASS detached menu packets: no early GPU/font use, immutable copies, bounded requests, UI-thread async only, close/cancel/failure/retry and concurrent producers");
}
''')
exe=out/'overlay.exe';subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe)],check=True,timeout=15)
