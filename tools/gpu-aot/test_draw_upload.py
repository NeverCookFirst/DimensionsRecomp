"""Exercise actual upload allocation and fence-slot reset with a fake driver."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/device.cpp').read_text()
def function(signature):
 start=source.index(signature);pos=source.index('{',start);depth=1;end=pos+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
begin=function('plume::RenderCommandList* BeginFrameCommandsLocked')
allocate=function('DrawUploadSlice HostDevice::AllocateDrawUpload')
page=source[source.index('struct DrawUploadPage {'):source.index('const std::filesystem::path& SnapshotDirectory')]
h=a.output/'draw-upload-test.cpp'
h.write_text(r'''
#include <array>
#include <algorithm>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define REXLOG_WARN(...) ((void)0)
namespace plume {
struct RenderBufferDesc { u64 size; static auto UploadBuffer(u64 n) {return RenderBufferDesc{n};} };
struct RenderBuffer {
 std::vector<u8> bytes; explicit RenderBuffer(u64 n):bytes(n){}
 void* map(){return bytes.data();}void unmap(){}
};
struct RenderDevice {
 u32 allocations=0;bool fail=false;
 auto createBuffer(RenderBufferDesc d)->std::unique_ptr<RenderBuffer> {
  if(fail)return {}; ++allocations;return std::make_unique<RenderBuffer>(d.size);
 }
};
struct RenderCommandList {u32 begins=0;void begin(){++begins;} };
struct Fence {};
struct Queue {u32 waits=0;void waitForCommandFence(Fence*){++waits;} };
struct Swap {bool needsResize(){return false;}};
}
struct DrawUploadSlice {
 plume::RenderBuffer* buffer=nullptr;void* mapped=nullptr;u64 offset=0;
 explicit operator bool()const{return buffer&&mapped;}
};
'''+page+r'''
struct DescriptorRetirement { void CompleteFrame(u32,auto callback){} };
enum class Backend {kD3D12,kVulkan};
struct State {
 static constexpr u32 kFramesInFlight=3;
 std::unique_ptr<plume::RenderDevice> device=std::make_unique<plume::RenderDevice>();
 std::unique_ptr<plume::Queue> queue=std::make_unique<plume::Queue>();
 std::unique_ptr<plume::Swap> swap_chain=std::make_unique<plume::Swap>();
 std::vector<int> framebuffers{1,2,3};
 std::array<std::unique_ptr<plume::RenderCommandList>,3> command_lists;
 std::array<std::unique_ptr<plume::Fence>,3> frame_fences;
 std::array<bool,3> frame_submitted{};
 Backend backend=Backend::kVulkan;u64 completed_submission=0;std::array<u64,3> slot_submission{};
 std::array<std::vector<int>,3> retired_resources,snapshots;
 std::array<std::vector<std::unique_ptr<DrawUploadPage>>,3> draw_uploads;
 u32 frame_slot=0,draw_upload_page=0,long_wait_log_count=0;
 u64 draw_upload_offset=0;bool command_list_open=false;
 u64 frame_slot_wait_calls=0;double frame_slot_wait_ms=0;
 DescriptorRetirement retired_descriptors;std::array<bool,2> texture_slots{};
 State(){for(u32 s=0;s<3;++s){command_lists[s]=std::make_unique<plume::RenderCommandList>();frame_fences[s]=std::make_unique<plume::Fence>();}}
};
void SaveSnapshots(std::vector<int>&){}
void RefreshCompletedSubmissionsLocked(State&){}
void InitializeNullTextures(State&,plume::RenderCommandList*){}
bool RebuildSwapChain(State&){return true;}
bool timing_enabled=true;
bool NativeTextureTimingEnabled(){return timing_enabled;}
std::unique_ptr<State> g_state=std::make_unique<State>();std::mutex g_mutex;
struct HostDevice {
 static auto LockRecording(){static std::recursive_mutex m;return std::unique_lock<std::recursive_mutex>(m);}
 static DrawUploadSlice AllocateDrawUpload(u32 size);
};
'''+begin+'\n'+allocate+r'''
int main() {
 assert(!HostDevice::AllocateDrawUpload(0));
 auto first=HostDevice::AllocateDrawUpload(8816);assert(first&&first.offset==0);
 std::memset(first.mapped,0xA5,8816);
 auto second=HostDevice::AllocateDrawUpload(8816);assert(second.offset==8960&&second.buffer==first.buffer);
 std::memset(second.mapped,0x5A,8816);
 for(int i=0;i<550;++i){auto s=HostDevice::AllocateDrawUpload(8816);assert(s&&s.offset%256==0);std::memset(s.mapped,i&255,8816);}
 assert(g_state->device->allocations==2);assert(g_state->queue->waits==0);
 for(int i=0;i<8816;++i) assert(static_cast<u8*>(first.mapped)[i]==0xA5);
 g_state->command_list_open=false;g_state->frame_submitted[0]=true;g_state->frame_slot=1;
 auto other=HostDevice::AllocateDrawUpload(8816);assert(other&&other.buffer!=first.buffer);
 std::memset(other.mapped,0xCC,8816);assert(g_state->queue->waits==0);
 assert(static_cast<u8*>(first.mapped)[0]==0xA5);
 g_state->command_list_open=false;g_state->frame_submitted[1]=true;g_state->frame_slot=0;
 auto reuse=HostDevice::AllocateDrawUpload(8816);assert(reuse.buffer==first.buffer&&reuse.offset==0);
 assert(g_state->queue->waits==1); // Reset only after old fence wait.
 assert(g_state->frame_slot_wait_calls==1 && g_state->frame_slot_wait_ms>=0);
 timing_enabled=false;
 g_state->command_list_open=false;g_state->frame_submitted[0]=true;
 auto untimed=HostDevice::AllocateDrawUpload(8816);assert(untimed);
 assert(g_state->queue->waits==2 && g_state->frame_slot_wait_calls==1);
 std::memset(reuse.mapped,0xDD,8816);assert(static_cast<u8*>(other.mapped)[0]==0xCC);
 auto large=HostDevice::AllocateDrawUpload(5*1024*1024);assert(large&&large.offset==0);
 assert(g_state->device->allocations==4);
 auto tail=HostDevice::AllocateDrawUpload(1);assert(tail&&tail.offset==0);
 assert(g_state->device->allocations==5);
 g_state->device->fail=true;assert(!HostDevice::AllocateDrawUpload(8*1024*1024));
 g_state.reset();assert(!HostDevice::AllocateDrawUpload(8816));
}
''')
exe=a.output/'draw-upload-test.exe'
subprocess.run(['clang++','-std=c++20','-DNOMINMAX',str(h),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
(a.output/'verification.json').write_text(json.dumps({'actual_production_functions':True,'fake_driver':True,
 'checks':['256-byte alignment','550 immutable draws','page growth','independent slots','reset after fence wait','oversize','allocation failure','shutdown'],
 'passed':True},indent=2))
print('Passed actual allocation/reset bodies: immutable draws, independent fence slots, alignment, growth and failure.')
