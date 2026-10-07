"""Inject every frame-ring allocation failure into production creation/retry bodies."""
import argparse
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
    production = (root / "rexlego/src/gpu_native/device.cpp").read_text()
    bodies = "\n".join(function(production, signature) for signature in (
        "bool CreateFrameRing(", "void RefreshCompletedSubmissionsLocked(",
        "bool WaitForSubmissionLocked(", "bool WaitForSubmittedFramesLocked(",
        "bool RebuildSwapChain(", "plume::RenderCommandList* BeginFrameCommandsLocked(",
    ))
    harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#include "gpu_native/descriptor_retirement.h"
using u32=std::uint32_t;using u64=std::uint64_t;
using legodimensions::gpu_native::DescriptorRetirement;
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
enum class Backend {kD3D12,kVulkan};
struct Tracked {static inline int live=0;Tracked(){++live;}virtual ~Tracked(){--live;}};
struct Allocation {
 int attempts=0,fail_at=0;
 template<class T>std::unique_ptr<T> Make(){
  if(++attempts==fail_at)return {};return std::make_unique<T>();
 }
} allocation;
namespace plume {
struct RenderTexture {};
struct RenderFramebuffer:Tracked {};
struct RenderCommandSemaphore:Tracked {};
struct NativeFence {u64 completed=0;u64 GetCompletedValue(){return completed;}};
struct RenderCommandFence:Tracked {};
struct D3D12CommandFence:RenderCommandFence {
 NativeFence native;NativeFence* d3d=&native;u64 fenceValue=2;
};
struct RenderCommandList:Tracked {int begins=0;void begin(){++begins;}};
struct RenderFramebufferDesc {RenderFramebufferDesc(const RenderTexture**,u32 count){assert(count==1);}};
struct Device {
 auto createFramebuffer(RenderFramebufferDesc){return allocation.Make<RenderFramebuffer>();}
 auto createCommandSemaphore(){return allocation.Make<RenderCommandSemaphore>();}
 auto createCommandFence(){return allocation.Make<D3D12CommandFence>();}
};
struct Queue {
 int waits=0;
 auto createCommandList(){return allocation.Make<RenderCommandList>();}
 void waitForCommandFence(RenderCommandFence* base){
  ++waits;auto& fence=*static_cast<D3D12CommandFence*>(base);
  fence.native.completed=fence.fenceValue-1;
 }
};
struct Swap {
 std::array<RenderTexture,3> textures;u32 count=3;int resizes=0;bool resize_pending=true;
 u32 getTextureCount(){return count;}
 RenderTexture* getTexture(u32 image){return &textures.at(image);}
 bool needsResize(){return resize_pending;}
 bool resize(){++resizes;resize_pending=false;return true;}
 bool isEmpty(){return count==0;}
};
}
struct State {
 static constexpr u32 kFramesInFlight=3;u32 command_slot_count=kFramesInFlight;Backend backend=Backend::kD3D12;
 std::unique_ptr<plume::Device> device=std::make_unique<plume::Device>();
 std::unique_ptr<plume::Queue> queue=std::make_unique<plume::Queue>();
 std::unique_ptr<plume::Swap> swap_chain=std::make_unique<plume::Swap>();
 std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
 std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;
 std::array<std::unique_ptr<plume::RenderCommandList>,3> command_lists;
 std::array<std::unique_ptr<plume::D3D12CommandFence>,3> frame_fences;
 std::array<std::unique_ptr<plume::RenderCommandSemaphore>,3> acquire_semaphores;
 std::array<bool,3> frame_submitted{};std::array<u64,3> slot_submission{1,2,3};
 u64 completed_submission=0;
 std::array<std::vector<std::shared_ptr<void>>,3> retired_resources;
 DescriptorRetirement retired_descriptors{8};std::array<bool,8> texture_slots{true,true,true,true,true,true,true,true};
 std::array<std::vector<int>,3> snapshots;
 bool command_list_open=false;u32 frame_slot=2,draw_upload_page=7,long_wait_log_count=0;
 u64 draw_upload_offset=1024,frame_slot_wait_calls=0;double frame_slot_wait_ms=0;
 State(){for(u32 i=0;i<3;++i){
  framebuffers.push_back(std::make_unique<plume::RenderFramebuffer>());
  render_semaphores.push_back(std::make_unique<plume::RenderCommandSemaphore>());
  command_lists[i]=std::make_unique<plume::RenderCommandList>();
  frame_fences[i]=std::make_unique<plume::D3D12CommandFence>();
  acquire_semaphores[i]=std::make_unique<plume::RenderCommandSemaphore>();
 }}
};
bool NativeTextureTimingEnabled(){return true;}
void SaveSnapshots(std::vector<int>& snapshots){snapshots.clear();}
void InitializeNullTextures(State&,plume::RenderCommandList*){}
'''
    tests = r'''
int main(){
 constexpr int kAllocations=15; // Three images and three submission slots.
 for(int failure=1;failure<=kAllocations;++failure){
  {
   State state;const int live=Tracked::live;
   auto* old_commands=state.command_lists[2].get();
   auto* old_fence=state.frame_fences[2].get();
   auto* old_acquire=state.acquire_semaphores[2].get();
   auto resource=std::make_shared<int>(1);std::weak_ptr<int> retained=resource;
   state.retired_resources[2].push_back(std::move(resource));
   assert(!state.retired_descriptors.Retire(3,1u<<2));
   allocation={0,failure};assert(!CreateFrameRing(state));
   assert(state.framebuffers.size()==3&&state.render_semaphores.size()==3);
   assert(state.command_lists[2].get()==old_commands&&state.frame_fences[2].get()==old_fence);
   assert(state.acquire_semaphores[2].get()==old_acquire&&state.frame_slot==2);
   assert(!retained.expired()&&state.texture_slots[3]&&Tracked::live==live);
  }
  assert(Tracked::live==0);
  {
   State state;auto* old_commands=state.command_lists[2].get();
   auto* old_fence=state.frame_fences[2].get();
   state.frame_submitted[2]=true;
   // Failed resize creation must leave the empty retry marker. Old allocator
   // and fence arrays stay intact, and temporary allocations are destroyed.
   allocation={0,failure};assert(!BeginFrameCommandsLocked(state));
   assert(state.framebuffers.empty()&&state.render_semaphores.empty());
   assert(state.command_lists[2].get()==old_commands&&state.frame_fences[2].get()==old_fence);
   assert(!state.command_list_open&&state.frame_slot==2&&state.queue->waits==1);
   assert(state.swap_chain->resizes==1&&!state.swap_chain->needsResize());
   assert(Tracked::live==9); // Old command lists, fences and acquire semaphores.
   // A repeated failure retries even though DXGI no longer requests resize.
   allocation={0,failure};assert(!BeginFrameCommandsLocked(state));
   assert(state.swap_chain->resizes==2&&Tracked::live==9);
   allocation={0,0};auto* commands=BeginFrameCommandsLocked(state);
   assert(commands&&commands==state.command_lists[0].get()&&commands->begins==1);
   assert(state.framebuffers.size()==3&&state.render_semaphores.size()==3);
   assert(state.command_list_open&&state.frame_slot==0&&state.swap_chain->resizes==3);
   assert(state.queue->waits==1&&Tracked::live==15);
  }
  assert(Tracked::live==0);
 }
 {
  State state;auto* old_commands=state.command_lists[2].get();
  state.swap_chain->count=0;allocation={0,0};assert(!CreateFrameRing(state));
  assert(allocation.attempts==0&&state.command_lists[2].get()==old_commands);
  assert(state.framebuffers.size()==3&&state.frame_slot==2);
 }
 assert(Tracked::live==0);
 std::cout<<"PASS: transactional frame-ring creation and retry after every allocation failure.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "frame-ring-transaction.cpp"
    exe = args.output.resolve() / "frame-ring-transaction.exe"
    source.write_text(harness + bodies + tests)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG",
                    "-I" + str(root / "rexlego/src"), str(source), "-o", str(exe)],
                   check=True, timeout=45)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
