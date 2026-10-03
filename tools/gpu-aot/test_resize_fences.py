"""Exercise actual Plume wait and native resize bodies without a game/window.

Deterministic events/fences cover old auto-reset wakeups and partial retries.
"""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)

def body(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

plume = body('thirdparty/plume/plume_d3d12.cpp',
             'void D3D12CommandQueue::waitForCommandFence(')
resize = body('thirdparty/plume/plume_d3d12.cpp', 'bool D3D12SwapChain::resize(')
native = '\n'.join(body('rexlego/src/gpu_native/device.cpp', sig) for sig in (
    'void RefreshCompletedSubmissionsLocked(', 'bool WaitForSubmittedFramesLocked(',
    'bool RebuildSwapChain('))
source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>
using u32=uint32_t;using u64=uint64_t;
#define REXLOG_ERROR(...) ((void)0)
constexpr int WAIT_OBJECT_0=0,INFINITE=-1,FALSE=0;
enum class Backend {kD3D12,kVulkan};
namespace plume {
struct NativeFence {u64 completed=0;u64 GetCompletedValue(){return completed;}};
struct RenderCommandFence {virtual ~RenderCommandFence()=default;};
struct D3D12CommandFence:RenderCommandFence {
 NativeFence native;NativeFence* d3d=&native;u64 fenceValue=2;
 D3D12CommandFence* fenceEvent=this;std::deque<u64> wakes;int event_waits=0;
};
struct D3D12CommandQueue {
 int calls=0;
 void waitForCommandFence(RenderCommandFence*);
};
}
int WaitForSingleObjectEx(plume::D3D12CommandFence* event,int timeout,int alertable) {
 assert(timeout==INFINITE&&alertable==FALSE);++event->event_waits;
 // An empty wake queue would block forever with the old event-only wait.
 assert(!event->wakes.empty());
 event->native.completed=event->wakes.front();event->wakes.pop_front();
 return WAIT_OBJECT_0;
}
namespace plume {
'''+plume+r'''
using HRESULT=long;
#define FAILED(x) ((x)!=0)
constexpr int DXGI_FORMAT_UNKNOWN=0;
struct BackBuffer {int releases=0;void Release(){++releases;}};
struct FakeDxgi {bool fail=false;int calls=0;HRESULT ResizeBuffers(int,int,int,int,u32){++calls;return fail?1:0;}};
struct D3D12SwapChain {
 struct {u32 textureCount=3;} desc;
 struct Texture {BackBuffer* d3d=nullptr;};
 std::array<Texture,3> textures;std::array<BackBuffer,3> buffers;
 FakeDxgi dxgi;FakeDxgi* d3d=&dxgi;u32 swapChainFlags=0;
 u32 width=1280,height=720,requested_width=1920,requested_height=1080;
 int gets=0;bool device_removed=false;
 D3D12SwapChain(){setTextures();}
 void getWindowSize(u32& w,u32& h){w=requested_width;h=requested_height;}
 void setTextures(){++gets;for(u32 i=0;i<3;++i)textures[i].d3d=device_removed?nullptr:&buffers[i];}
 bool resize();
};
'''+resize+r'''
}
struct SwapChain {bool resize_ok=true,empty=false;int resizes=0;bool resize();bool isEmpty(){return empty;}};
struct State {
 static constexpr u32 kFramesInFlight=3;
 Backend backend=Backend::kD3D12;
 std::array<bool,3> frame_submitted{};
 std::array<std::unique_ptr<plume::D3D12CommandFence>,3> frame_fences;
 std::array<u64,3> slot_submission{1,2,3};u64 completed_submission=0;
 std::unique_ptr<plume::D3D12CommandQueue> queue=std::make_unique<plume::D3D12CommandQueue>();
 std::unique_ptr<SwapChain> swap_chain=std::make_unique<SwapChain>();
 std::vector<int> framebuffers{1,2,3},render_semaphores{1,2,3};
 bool command_list_open=false;int rings=0;
 State(){for(auto& f:frame_fences)f=std::make_unique<plume::D3D12CommandFence>();}
};
State* active=nullptr;
void AssertGpuIdle(){for(u32 i=0;i<3;++i){const auto& f=*active->frame_fences[i];
 assert(!active->frame_submitted[i]);assert(f.native.completed!=UINT64_MAX&&f.native.completed>=f.fenceValue-1);}}
bool SwapChain::resize(){AssertGpuIdle();++resizes;return resize_ok;}
bool CreateFrameRing(State& s){AssertGpuIdle();++s.rings;s.framebuffers={4,5,6};s.render_semaphores={4,5,6};return true;}
'''+native+r'''
int main(){
 {
  plume::D3D12SwapChain s;s.requested_width=0;
  assert(!s.resize()&&s.width==1280&&s.height==720&&s.dxgi.calls==0);
  assert(s.textures[0].d3d&&s.buffers[0].releases==0);
  s.requested_width=1920;s.dxgi.fail=true;assert(!s.resize());
  assert(s.width==1280&&s.height==720&&s.textures[0].d3d&&s.gets==2);
  s.dxgi.fail=false;assert(s.resize());assert(s.width==1920&&s.height==1080);
  assert(s.buffers[0].releases==2&&s.textures[0].d3d);
  s.device_removed=true;s.dxgi.fail=true;assert(!s.resize());
  assert(!s.textures[0].d3d);assert(!s.resize()); // No null Release on retry.
 }
 // Complete fence whose auto-reset event has already been consumed: no wait.
 plume::D3D12CommandQueue q;plume::D3D12CommandFence f;
 f.native.completed=1;q.waitForCommandFence(&f);assert(f.event_waits==0);
 // A stale event from submission 1 cannot satisfy submission 2.
 f.fenceValue=3;f.wakes={1,2};q.waitForCommandFence(&f);
 assert(f.native.completed==2&&f.event_waits==2);
 // Device removal must never be treated as a healthy completion or block.
 f.native.completed=UINT64_MAX;q.waitForCommandFence(&f);assert(f.event_waits==2);
 {
  State s;active=&s;s.frame_submitted.fill(true);
  s.frame_fences[0]->wakes={1};s.frame_fences[1]->wakes={1};s.frame_fences[2]->wakes={1};
  assert(RebuildSwapChain(s));assert(s.completed_submission==3&&s.rings==1&&s.swap_chain->resizes==1);
  for(auto& fence:s.frame_fences)assert(fence->event_waits==1);
 }
 {
  State s;active=&s;s.command_list_open=true;s.frame_submitted.fill(true);
  assert(!RebuildSwapChain(s));assert(s.rings==0&&s.swap_chain->resizes==0&&s.framebuffers.size()==3);
  for(auto& fence:s.frame_fences)assert(fence->event_waits==0);
 }
 {
  State s;active=&s;s.frame_submitted.fill(true);
  s.frame_fences[0]->native.completed=1;s.frame_fences[1]->native.completed=UINT64_MAX;
  assert(!RebuildSwapChain(s));assert(!s.frame_submitted[0]&&s.frame_submitted[1]);
  assert(s.framebuffers[0]==1&&s.rings==0&&s.swap_chain->resizes==0);
  // Retry must not wait again on slot 0's already-consumed event.
  s.frame_fences[1]->native.completed=1;s.frame_fences[2]->wakes={1};
  assert(RebuildSwapChain(s));assert(s.completed_submission==3);
 }
 {
  State s;active=&s;s.frame_submitted.fill(true);
  for(auto& fence:s.frame_fences)fence->native.completed=1;
  s.swap_chain->resize_ok=false;assert(!RebuildSwapChain(s));
  assert(s.rings==0);s.swap_chain->resize_ok=true;s.swap_chain->empty=true;
  assert(!RebuildSwapChain(s));assert(s.rings==0);
  s.swap_chain->empty=false;assert(RebuildSwapChain(s));assert(s.rings==1);
  for(auto& fence:s.frame_fences)assert(fence->event_waits==0);
 }
 {
  State s;active=&s;s.frame_submitted[0]=true;s.frame_fences[0]->wakes={UINT64_MAX};
  assert(!RebuildSwapChain(s));assert(s.frame_submitted[0]&&s.rings==0&&s.framebuffers[0]==1);
 }
}
'''
h = a.output / 'resize-fences.cpp'
h.write_text(source)
exe = a.output / 'resize-fences.exe'
subprocess.run(['clang++', '-std=c++20', '-DNOMINMAX', str(h), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
(a.output / 'verification.json').write_text(json.dumps({
    'actual_production_bodies': True, 'fake_gpu': True, 'passed': True,
    'checks': ['completed consumed event', 'stale event and newer fence', 'all three submissions before resize',
               'open list rejection', 'device removal before/during wait', 'partial retry',
               'failed/minimized resize retry', 'DXGI dimensions only after success',
               'backbuffer references restored on failure', 'null backbuffer retry',
               'no DXGI latency wait']}, indent=2))
print('PASS: actual fence wait and resize ordering; no game launched.')
