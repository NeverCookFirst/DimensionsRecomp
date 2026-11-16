from pathlib import Path
import argparse
import subprocess

parser=argparse.ArgumentParser();parser.add_argument('output',type=Path)
out=parser.parse_args().output;out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
hooks=(root/'rexlego/src/gpu_native/hooks_device.cpp').read_text()
def body(source,signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
source=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "gpu_native/tile_extent.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;using f64=double;
using be_f32=float;using be_u32=u32;using mapped_f32=const float*;
struct D3DRect {i32 left,top,right,bottom;};
struct DrawBindings {std::array<u32,4> render_targets{};u32 depth_stencil=0;};
struct D3DDevice {struct {float width=1280,height=720;}viewport;std::array<u8,0x6080> bytes;DrawBindings bindings;};
namespace rex::memory {constexpr u32 kMemoryAllocationCommit=1,kMemoryProtectRead=2;struct HeapAllocationInfo{u32 state=1,protect=2;};}
struct Memory {
  alignas(64) std::array<u8,0x20000> bytes{};bool readable=true;
  Memory* LookupHeap(u32 address){return address<bytes.size()?this:nullptr;}
  bool QueryRegionInfo(u32,rex::memory::HeapAllocationInfo*){return readable;}
  template<class T>T TranslateVirtual(u32 address){return reinterpret_cast<T>(bytes.data()+address);}
  u8* virtual_membase(){return bytes.data();}
} memory;
#define REX_KERNEL_MEMORY() (&::memory)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) (++errors)
unsigned errors=0,drains=0,submissions=0,clears=0;
bool worker=false,threaded=true,snapshot_state=true,g_seen_tiled_pass=false;
DrawBindings bindings;
std::vector<int> events;
std::vector<bool> polls;
std::deque<std::function<void()>> tasks;
bool NativeWorkerShouldQueue(){return threaded&&!worker;}
void NativeWorkerSubmit(std::function<void()> task){tasks.push_back(std::move(task));}
bool SnapshotStateBindingsEnabled(){return snapshot_state;}
DrawBindings SnapshotDrawBindings(){return bindings;}
DrawBindings ReadDeviceBindings(const D3DDevice* device){return device->bindings;}
struct HostDevice {
 static auto LockRecording(){if(!worker)++drains;static std::recursive_mutex mutex;return std::unique_lock(mutex);}
 static void SubmitRecordedWork(){events.push_back(20);++submissions;}
 static void PollCompletionCallbacks(bool wait=true){polls.push_back(wait);}
 static void BeginTiledPassSnapshot(){events.push_back(30);}
};
std::vector<std::array<u32,3>> promotions;
bool PromoteTiledSurface(u32 target,u32 width,u32 height){if(target)promotions.push_back({target,width,height});return true;}
u32 clear_color_value=0,clear_stencil_value=0;double clear_depth_value=0;DrawBindings clear_bindings;
u32 ClearRecorded(u32 flags,u32 color,f64 depth,u32 stencil,const std::vector<int>&,const DrawBindings* captured){
  assert(flags==0x31);++clears;events.push_back(40);clear_color_value=color;clear_depth_value=depth;
  clear_stencil_value=stencil;clear_bindings=*captured;return 0;
}
void Drain(){worker=true;while(!tasks.empty()){auto task=std::move(tasks.front());tasks.pop_front();task();}worker=false;}
'''
for origin,signature in [(hooks,'void KickOffSegmentHook('),(hooks,'u32 KickOffHook('),
                         (draw,'u32 BeginTilingRecorded('),(draw,'u32 BeginTilingHook(')]:
    source+=body(origin,signature)+'\n'
source+=r'''
int main(){
 auto* device=std::construct_at(memory.TranslateVirtual<D3DDevice*>(0x1000));
 auto* tiles=memory.TranslateVirtual<D3DRect*>(0x100);
 tiles[0]={0,0,1280,360};tiles[1]={0,360,1280,720};
 device->bindings.render_targets[0]=101;device->bindings.depth_stencil=201;
 float color[]{1,0.5f,0,1};
 NativeWorkerSubmit([]{events.push_back(10);});
 assert(KickOffHook(device)==0x7000 && !submissions && !polls.back());
 assert(BeginTilingHook(device,0,2,0x100,color,0.75,0,3)==0);
 NativeWorkerSubmit([]{events.push_back(50);});
 device->bindings={};tiles[0]={};color[0]=0;
 assert(!drains && promotions.empty() && !clears);
 Drain();assert(events==std::vector<int>({10,20,30,40,50}));
 assert((promotions==std::vector<std::array<u32,3>>({{101,1280,720},{201,1280,720}})));
 assert(clear_color_value==0xFFFF8000 && clear_depth_value==0.75 && clear_stencil_value==3);
 assert(clear_bindings.render_targets[0]==101 && clear_bindings.depth_stencil==201);
 assert(BeginTilingHook(device,0,0,0x100,color,1,0,0)!=0 && tasks.empty());
 tiles[0]={0,0,1280,720};tiles[1]=tiles[0];
 assert(BeginTilingHook(device,0,2,0x100,color,1,0,0)!=0 && tasks.empty());
 memory.readable=false;assert(BeginTilingHook(device,0,1,0x100,color,1,0,0)!=0);memory.readable=true;
 snapshot_state=false;
 NativeWorkerSubmit([]{bindings.render_targets[0]=301;});
 assert(BeginTilingHook(device,1,1,0x100,color,1,0,0)==0);Drain();
 assert(clears==1 && promotions.back()[0]==301);
 threaded=false;KickOffSegmentHook(nullptr);assert(submissions==2 && polls.back());
 assert(KickOffHook(nullptr)==0 && submissions==3);
 assert(BeginTilingHook(device,0,1,0x100,color,1,0,0)==0 && clears==2);
 assert(!errors);
}
'''
cpp=out/'queued-frame-boundaries.cpp';cpp.write_text(source);exe=out/'queued-frame-boundaries.exe'
subprocess.run(['clang++','-std=c++23','-O2','-UNDEBUG','-D_CRT_SECURE_NO_WARNINGS',
               '-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True,timeout=30)
print('PASS production KickOff/BeginTiling: FIFO boundaries, immediate marker, owned inputs, clear flags, invalid rectangles and legacy paths')
