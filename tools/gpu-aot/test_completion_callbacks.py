"""Run actual host enqueue/poll/fence attribution against deterministic fences."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/device.cpp').read_text()
def body(sig):
 start=source.index(sig);end=source.index('{',start)+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
functions='\n'.join(body(sig) for sig in ['void RefreshCompletedSubmissionsLocked(',
 'void MarkSubmissionLocked(', 'bool HostDevice::EnqueueCompletionCallback(',
 'void HostDevice::PollCompletionCallbacks('])
h=a.output/'completion-callbacks.cpp'
h.write_text(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "gpu_native/completion_queue.h"
using namespace legodimensions::gpu_native;
using u32=std::uint32_t;using u64=std::uint64_t;
enum class Backend {kD3D12,kVulkan};
namespace plume {
struct NativeFence {u64 completed=0;u64 GetCompletedValue(){return completed;}};
struct D3D12CommandFence {NativeFence native;NativeFence* d3d=&native;u64 fenceValue=1;};
}
struct State {
 static constexpr u32 kFramesInFlight=3;
 Backend backend=Backend::kD3D12;
 std::array<bool,3> frame_submitted{};
 std::array<std::unique_ptr<plume::D3D12CommandFence>,3> frame_fences;
 std::array<u64,3> slot_submission{};
 u64 last_submission=0,completed_submission=0;
 CompletionQueue completion_callbacks;
 u64 callbacks_enqueued=0,callbacks_executed=0;
 bool command_list_open=false;
 State(){for(auto& f:frame_fences)f=std::make_unique<plume::D3D12CommandFence>();}
};
std::mutex g_mutex;std::recursive_mutex recording_mutex;std::unique_ptr<State> g_state=std::make_unique<State>();
struct HostDevice {
 static auto LockRecording(){return std::unique_lock(recording_mutex);}
 static bool EnqueueCompletionCallback(std::function<void()>);
 static void PollCompletionCallbacks();
};
'''+functions+r'''
int main(){
 std::vector<int> ran;
 auto task=[&](int n){return [&,n]{assert(g_mutex.try_lock());g_mutex.unlock();
  bool unlocked=false;std::thread check([&]{unlocked=recording_mutex.try_lock();if(unlocked)recording_mutex.unlock();});check.join();assert(unlocked);
  ran.push_back(n);};};
 auto submit=[&](u32 slot){auto& s=*g_state;RefreshCompletedSubmissionsLocked(s);
  s.command_list_open=false;++s.frame_fences[slot]->fenceValue;MarkSubmissionLocked(s,slot);};
 auto complete=[&](u32 slot){auto& f=*g_state->frame_fences[slot];f.native.completed=f.fenceValue-1;};
 // No prior GPU work: callback is ready without submission/wait.
 assert(HostDevice::EnqueueCompletionCallback(task(0)));HostDevice::PollCompletionCallbacks();
 assert((ran==std::vector<int>{0}));
 g_state->command_list_open=true;
 assert(HostDevice::EnqueueCompletionCallback(task(1)));
 assert(HostDevice::EnqueueCompletionCallback(task(2)));
 HostDevice::PollCompletionCallbacks();assert(ran.size()==1&&g_state->last_submission==0);
 submit(0);HostDevice::PollCompletionCallbacks();assert(ran.size()==1);
 // Pending frame attaches callback even when no command list is open.
 assert(HostDevice::EnqueueCompletionCallback(task(3)));
 // D3D12 removal sentinel must not be interpreted as completed.
 g_state->frame_fences[0]->native.completed=UINT64_MAX;
 HostDevice::PollCompletionCallbacks();assert(ran.size()==1);
 complete(0);HostDevice::PollCompletionCallbacks();
 assert((ran==std::vector<int>{0,1,2,3}));
 HostDevice::PollCompletionCallbacks();assert(ran.size()==4); // Exactly once.
 // Newer fence completion proves every older submission on one queue done.
 g_state->command_list_open=true;HostDevice::EnqueueCompletionCallback(task(4));submit(1);
 g_state->command_list_open=true;HostDevice::EnqueueCompletionCallback(task(5));submit(2);
 complete(2);HostDevice::PollCompletionCallbacks();assert(ran.back()==5&&ran.size()==6);
 // Reentrant callback enqueue/poll must not overtake an already queued peer.
 HostDevice::EnqueueCompletionCallback([&]{ran.push_back(6);
  HostDevice::EnqueueCompletionCallback(task(8));HostDevice::PollCompletionCallbacks();ran.push_back(60);});
 HostDevice::EnqueueCompletionCallback(task(7));HostDevice::PollCompletionCallbacks();
 assert((std::vector<int>(ran.end()-4,ran.end())==std::vector<int>{6,60,7,8}));
 // Reused ring fences must compare the newly signalled value, not the old one.
 for(int frame=0;frame<1000;++frame){u32 slot=frame%3;
  g_state->command_list_open=true;auto before=ran.size();HostDevice::EnqueueCompletionCallback(task(100+frame));
  submit(slot);HostDevice::PollCompletionCallbacks();assert(ran.size()==before);
  complete(slot);HostDevice::PollCompletionCallbacks();assert(ran.size()==before+1);}
 assert(g_state->completion_callbacks.size()==0);
 // Teardown discards pending guest callbacks rather than invoking on host UI thread.
 g_state->command_list_open=true;HostDevice::EnqueueCompletionCallback(task(-1));auto count=ran.size();
 g_state.reset();HostDevice::PollCompletionCallbacks();assert(ran.size()==count);
 assert(!HostDevice::EnqueueCompletionCallback(task(-2)));
}
''')
exe=a.output/'completion-callbacks.exe'
subprocess.run(['clang++','-std=c++20','-DNOMINMAX','-I'+str(root/'rexlego/src'),str(h),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
assert source.count('MarkSubmissionLocked(state, slot);')==4
hook=(root/'rexlego/src/gpu_native/hooks_device.cpp').read_text()
insert=hook[hook.index('void InsertCallbackHook('):hook.index('}  // namespace',hook.index('void InsertCallbackHook('))]
assert 'Synchronize(' not in insert and 'SubmitRecordedWork(' not in insert
(a.output/'verification.json').write_text(json.dumps({'passed':True,'actual_host_functions':True,
 'checks':['callback waits for containing submission','pending last submission without open list',
 'D3D12 post-signal fenceValue - 1','device removal never completion','FIFO and exactly once',
 'newer queue completion covers older work','guest callback outside state AND recording mutexes','reentrant poll preserves order',
 '1000 ring slot reuse iterations','teardown cancels guest callbacks','all four submission paths tagged',
 'InsertCallback neither submits nor waits'],'real_GPU_gameplay_not_proven':True},indent=2))
print('Passed actual enqueue/poll: fence generations, FIFO, removal, reentrancy, ring reuse and shutdown.')
