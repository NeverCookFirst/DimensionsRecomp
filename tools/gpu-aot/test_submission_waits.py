"""Exercise native submission waits and retirement with deterministic fences."""
import argparse
import json
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    production = (root / "rexlego/src/gpu_native/device.cpp").read_text()
    bodies = "\n".join(function(production, signature) for signature in (
        "void RefreshCompletedSubmissionsLocked(",
        "void MarkSubmissionLocked(",
        "bool WaitForSubmissionLocked(",
        "bool WaitForSubmittedFramesLocked(",
        "plume::RenderCommandList* BeginFrameCommandsLocked(",
        "bool HostDevice::Synchronize(",
    ))
    harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "gpu_native/descriptor_retirement.h"
using u32=std::uint32_t;using u64=std::uint64_t;
u64 g_recording_serial=0;
using legodimensions::gpu_native::DescriptorRetirement;
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
enum class Backend {kD3D12,kVulkan};
enum class SyncReason {kOther,kCount};
namespace plume {
struct NativeFence {u64 completed=0;u64 GetCompletedValue(){return completed;}};
struct RenderCommandFence {virtual ~RenderCommandFence()=default;};
struct D3D12CommandFence:RenderCommandFence {
 NativeFence native;NativeFence* d3d=&native;u64 fenceValue=2;
 std::optional<u64> wait_result;
};
struct RenderCommandList {
 int begins=0,ends=0;void begin(){++begins;}void end(){++ends;}
};
struct Queue {
 int waits=0,submissions=0;
 void waitForCommandFence(RenderCommandFence* base){
  ++waits;auto& fence=*static_cast<D3D12CommandFence*>(base);
  if(fence.wait_result)fence.native.completed=*fence.wait_result;
 }
 void executeCommandLists(RenderCommandList*,RenderCommandFence* base){
  ++submissions;++static_cast<D3D12CommandFence*>(base)->fenceValue;
 }
};
struct Swap {bool needsResize(){return false;}};
}
// Disabled optional timestamp boundary; test_gpu_timestamps covers its real enabled implementation.
struct GpuSubmissionTimestamps {
 void Begin(auto*,u32,u64){};void End(auto*,u32,const char*,u32=~0u,u32=~0u){};
 void Submitted(u32,u64){};void Complete(u32,u64,u64,u64){};
};
struct State {
 GpuSubmissionTimestamps* gpu_timestamps=nullptr;u32 present_number=0;
 static constexpr u32 kFramesInFlight=3;u32 command_slot_count=kFramesInFlight;
 Backend backend=Backend::kD3D12;
 std::array<bool,3> frame_submitted{};
 std::array<std::unique_ptr<plume::D3D12CommandFence>,3> frame_fences;
 std::array<std::unique_ptr<plume::RenderCommandList>,3> command_lists;
 std::unique_ptr<plume::Queue> queue=std::make_unique<plume::Queue>();
 std::unique_ptr<plume::Swap> swap_chain=std::make_unique<plume::Swap>();
 std::vector<int> framebuffers{1,2,3};
 std::array<u64,3> slot_submission{1,2,3};
 u64 last_submission=3,completed_submission=0;
 std::array<std::vector<std::shared_ptr<void>>,3> retired_resources;
 std::array<std::vector<int>,3> snapshots;
 DescriptorRetirement retired_descriptors{8};
 std::array<bool,8> texture_slots{true,true,true,true,true,true,true,true};
 bool command_list_open=false;u32 frame_slot=0,draw_upload_page=7;
 u64 draw_upload_offset=1024,frame_slot_wait_calls=0,sync_calls=0;
 u32 long_wait_log_count=0;
 double frame_slot_wait_ms=0,sync_ms=0;
 std::array<u64,1> sync_reason_calls{};
 std::array<double,1> sync_reason_ms{};
 State(){for(u32 i=0;i<3;++i){
  frame_fences[i]=std::make_unique<plume::D3D12CommandFence>();
  command_lists[i]=std::make_unique<plume::RenderCommandList>();
 }}
};
int saved_snapshots=0;
void SaveSnapshots(std::vector<int>& snapshots){saved_snapshots+=int(snapshots.size());snapshots.clear();}
bool NativeTextureTimingEnabled(){return true;}
void InitializeNullTextures(State&,plume::RenderCommandList*){}
bool RebuildSwapChain(State&){assert(false);return false;}
std::unique_ptr<State> g_state;
std::mutex g_mutex;
struct HostDevice {
 static auto LockRecording(){static std::recursive_mutex mutex;return std::unique_lock(mutex);}
 static bool Synchronize(SyncReason reason=SyncReason::kOther);
};
'''
    tests = r'''
std::weak_ptr<int> Retain(State& state,u32 slot){
 auto resource=std::make_shared<int>(int(slot));
 state.retired_resources[slot].push_back(resource);
 state.frame_submitted[slot]=true;
 state.snapshots[slot]={int(slot)};
 assert(!state.retired_descriptors.Retire(3+slot,1u<<slot));
 return resource;
}
int main(){
 {
  State state;auto resource=Retain(state,0);
  // A void wait can return without completing the submitted fence.
  assert(!BeginFrameCommandsLocked(state));
  assert(state.frame_submitted[0]&&!resource.expired()&&state.texture_slots[3]);
  assert(state.draw_upload_page==7&&state.draw_upload_offset==1024);
  assert(state.command_lists[0]->begins==0&&state.snapshots[0].size()==1);
  assert(state.completed_submission==0&&!state.command_list_open);
  state.frame_fences[0]->wait_result=1;
  assert(BeginFrameCommandsLocked(state)==state.command_lists[0].get());
  assert(!state.frame_submitted[0]&&resource.expired()&&!state.texture_slots[3]);
  assert(state.draw_upload_page==0&&state.draw_upload_offset==0);
  assert(state.command_lists[0]->begins==1&&state.snapshots[0].empty());
  assert(state.completed_submission==1&&state.command_list_open);
 }
 for(bool removed_before_wait:{false,true}){
  State state;auto resource=Retain(state,0);
  if(removed_before_wait)state.frame_fences[0]->native.completed=UINT64_MAX;
  else state.frame_fences[0]->wait_result=UINT64_MAX;
  assert(!BeginFrameCommandsLocked(state));
  assert(state.queue->waits==int(!removed_before_wait));
  assert(state.frame_submitted[0]&&!resource.expired()&&state.texture_slots[3]);
  assert(state.command_lists[0]->begins==0&&state.completed_submission==0);
 }
 {
  g_state=std::make_unique<State>();auto& state=*g_state;
  auto first=Retain(state,0),second=Retain(state,1),third=Retain(state,2);
  state.frame_fences[0]->native.completed=1;
  assert(!HostDevice::Synchronize());
  assert(!state.frame_submitted[0]&&first.expired()&&!state.texture_slots[3]);
  assert(state.frame_submitted[1]&&!second.expired()&&state.texture_slots[4]);
  assert(state.frame_submitted[2]&&!third.expired()&&state.texture_slots[5]);
  assert(state.snapshots[0].empty()&&state.snapshots[1].size()==1);
  assert(state.completed_submission==1&&state.queue->waits==2);
  // Retry drains only remaining submissions, not slot 0's consumed event.
  state.frame_fences[1]->wait_result=1;state.frame_fences[2]->wait_result=1;
  assert(HostDevice::Synchronize());
  assert(second.expired()&&third.expired()&&state.completed_submission==3);
  assert(state.queue->waits==4&&state.snapshots[1].empty()&&state.snapshots[2].empty());
 }
 for(bool removed_before_wait:{false,true}){
  g_state=std::make_unique<State>();auto& state=*g_state;auto resource=Retain(state,0);
  if(removed_before_wait)state.frame_fences[0]->native.completed=UINT64_MAX;
  else state.frame_fences[0]->wait_result=UINT64_MAX;
  assert(!HostDevice::Synchronize());
  assert(state.frame_submitted[0]&&!resource.expired()&&state.texture_slots[3]);
  assert(state.completed_submission==0&&state.snapshots[0].size()==1);
 }
 {
  g_state=std::make_unique<State>();auto& state=*g_state;auto resource=Retain(state,0);
  state.command_list_open=true;state.frame_fences[0]->wait_result=2;
  assert(HostDevice::Synchronize());
  assert(state.queue->submissions==1&&state.command_lists[0]->ends==1);
  assert(!state.command_list_open&&!state.frame_submitted[0]&&resource.expired());
  assert(state.completed_submission==4);
 }
 {
  State state;state.backend=Backend::kVulkan;auto resource=Retain(state,0);
  assert(BeginFrameCommandsLocked(state)&&resource.expired());
 }
 g_state.reset();assert(!HostDevice::Synchronize());
}
'''
    source = args.output / "submission-waits.cpp"
    source.write_text(harness + bodies + tests)
    exe = args.output / "submission-waits.exe"
    include = str(root / "rexlego/src")
    if Path(args.compiler).name.lower() in ("cl", "cl.exe"):
        command = [args.compiler, "/nologo", "/std:c++20", "/EHsc", "/UNDEBUG",
                   "/I" + include, str(source), "/Fe:" + str(exe)]
    else:
        command = [args.compiler, "-std=c++20", "-UNDEBUG", "-pthread",
                   "-I" + include, str(source), "-o", str(exe)]
    subprocess.run(command, check=True, timeout=45)
    subprocess.run([str(exe.resolve())], check=True, timeout=10)
    (args.output / "verification.json").write_text(json.dumps({
        "passed": True, "actual_production_bodies": True, "fake_gpu": True,
        "checks": ["incomplete wait preserves allocator/resources/descriptors",
                   "removal before and during wait", "successful retry",
                   "partial drain retires completed slots only",
                   "open-list submission before drain"],
        "real_GPU_gameplay_not_proven": True,
    }, indent=2))
    print("PASS: native submission waits reject failed fences before resource or allocator reuse.")


if __name__ == "__main__":
    main()
