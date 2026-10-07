"""Exercise real timestamp helper, Plume writes and all native close paths.

Fake COM/fences assert that CPU readback follows verified completion. No real
GPU, frame latency or utilization claim follows from this asset-free fixture.
"""
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
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--device", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    helper = (args.helper or root / "rexlego/src/gpu_native/gpu_timestamps.h").read_text()
    helper = helper.replace("#include <plume_d3d12.h>", "").replace("#pragma once", "")
    device = (args.device or root / "rexlego/src/gpu_native/device.cpp").read_text()
    plume = (root / "thirdparty/plume/plume_d3d12.cpp").read_text()
    timestamp_write = function(plume, "void D3D12CommandList::writeTimestamp(")
    completion = "\n".join(function(device, signature) for signature in (
        "void RefreshCompletedSubmissionsLocked(", "void MarkSubmissionLocked(",
        "bool WaitForSubmissionLocked(", "bool WaitForSubmittedFramesLocked("))
    paths = []
    for signature, kind in (("bool HostDevice::Synchronize(", "synchronize"),
                            ("bool HostDevice::SubmitRecordedWork(", "kickoff"),
                            ("bool HostDevice::PresentTexture(", "acquire_failed"),
                            ("bool HostDevice::PresentTexture(", "present")):
        body = function(device, signature)
        call = body.index('state.gpu_timestamps->End(commands, slot, "' + kind + '"')
        guard = body.rfind("if (state.gpu_timestamps)", 0, call)
        start = body.rfind("\n", 0, guard) + 1
        end = body.index("MarkSubmissionLocked(state, slot);", start) + len("MarkSubmissionLocked(state, slot);")
        paths.append("void Close_" + kind + "(State& state,unsigned slot,unsigned image_index=1) {\n"
                     "auto* commands=&state.commands;bool timing_enabled=false;SyncReason reason=SyncReason::Resource;\n"
                     + body[start:end] + "\n}\n")
    begin = function(device, "plume::RenderCommandList* BeginFrameCommandsLocked(")
    start = begin.index("  commands->begin();")
    end = begin.index("  return commands;", start) + len("  return commands;")
    begin = "plume::RenderCommandList* Open(State& state,unsigned slot) {auto* commands=&state.commands;\n" + begin[start:end] + "\n}"
    present = function(device, "bool HostDevice::PresentTexture(")
    call = present.index("state.gpu_timestamps->PresentResult(slot, presented);")
    guard = present.rfind("if (state.gpu_timestamps)", 0, call)
    present_result = ("void ReportPresent(State& state,unsigned slot,bool presented){\n"
                      + present[guard:call + len("state.gpu_timestamps->PresentResult(slot, presented);")]
                      + "\n}\n")
    harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
using u32=uint32_t;using u64=uint64_t;using UINT64=unsigned long long;using HRESULT=long;
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
#define REXLOG_ERROR(...) ((void)0)
constexpr int D3D12_QUERY_TYPE_TIMESTAMP=2;
struct D3D12_RANGE {size_t Begin,End;};
enum class Failure {None,Frequency,ZeroFrequency,Heap,Buffer,Resource,Count,Map,NullMap};
Failure failure=Failure::None;
struct Heap {bool initialized[2]{};UINT64 ticks[2]{};};
struct Resource {
 UINT64 ticks[2]{};bool ready=false;int maps=0,unmaps=0;
 HRESULT Map(int index,const D3D12_RANGE* range,void** out) {
  assert(ready && "timestamp premature map oracle");assert(index==0&&range->Begin==0&&range->End==16);
  ++maps;if(failure==Failure::Map)return -1;
  *out=failure==Failure::NullMap?nullptr:ticks;return 0;
 }
 void Unmap(int index,const D3D12_RANGE* range){assert(index==0&&range->Begin==0&&range->End==0);++unmaps;}
};
struct NativeQueue {int frequency_calls=0;HRESULT GetTimestampFrequency(UINT64* value) {
 ++frequency_calls;*value=failure==Failure::ZeroFrequency?0:1000000000ull;
 return failure==Failure::Frequency?-1:0;
}};
struct Fence {UINT64 completed=0;UINT64 GetCompletedValue(){return completed;}};
struct NativeCommands {
 UINT64 tick=0;int writes=0,resolves=0;
 void EndQuery(Heap* heap,int type,unsigned index){assert(heap&&type==2&&index<2);heap->initialized[index]=true;heap->ticks[index]=tick;++writes;}
 void ResolveQueryData(Heap* heap,int type,unsigned index,unsigned count,Resource* resource,UINT64 offset){
  assert(type==2&&count==1&&index<2&&heap->initialized[index]&&offset==index*8);
  resource->ticks[index]=heap->ticks[index];resource->ready=false;++resolves;tick+=5;
 }
};
namespace plume {
struct RenderBuffer {virtual ~RenderBuffer()=default;};
struct D3D12Buffer:RenderBuffer {Resource native;Resource* d3d=&native;};
struct RenderQueryPool {virtual ~RenderQueryPool()=default;virtual unsigned getCount() const=0;};
struct D3D12QueryPool:RenderQueryPool {
 Heap native;Heap* d3d=&native;std::unique_ptr<RenderBuffer> readbackBuffer;
 unsigned getCount()const override{return failure==Failure::Count?1:2;}
 D3D12QueryPool(){if(failure==Failure::Heap)d3d=nullptr;if(failure!=Failure::Buffer){
  auto b=std::make_unique<D3D12Buffer>();if(failure==Failure::Resource)b->d3d=nullptr;readbackBuffer=std::move(b);}}
};
struct RenderDevice {int creates=0;std::vector<D3D12QueryPool*> pools;
 std::unique_ptr<RenderQueryPool> createQueryPool(unsigned count){assert(count==2);++creates;auto pool=std::make_unique<D3D12QueryPool>();pools.push_back(pool.get());return pool;}
 Resource& resource(unsigned slot){return static_cast<D3D12Buffer*>(pools.at(slot)->readbackBuffer.get())->native;}
};
struct RenderCommandFence {virtual ~RenderCommandFence()=default;};
struct D3D12CommandFence:RenderCommandFence {Fence native;Fence* d3d=&native;UINT64 fenceValue=1;};
struct RenderCommandSemaphore {};
struct RenderCommandList {virtual ~RenderCommandList()=default;virtual void writeTimestamp(const RenderQueryPool*,unsigned)=0;virtual void begin()=0;virtual void end()=0;};
struct D3D12CommandList:RenderCommandList {NativeCommands native;NativeCommands* d3d=&native;int begins=0,ends=0;
 void writeTimestamp(const RenderQueryPool*,unsigned)override;void begin()override{++begins;}void end()override{++ends;}};
struct RenderCommandQueue {virtual ~RenderCommandQueue()=default;int executes=0,waits=0;
 void executeCommandLists(const RenderCommandList*,RenderCommandFence* fence){++executes;++static_cast<D3D12CommandFence*>(fence)->fenceValue;}
 void executeCommandLists(const RenderCommandList** lists,unsigned count,RenderCommandSemaphore** w,unsigned wc,RenderCommandSemaphore** s,unsigned sc,RenderCommandFence* f){assert(count==1&&lists[0]&&wc==1&&sc==1&&w[0]&&s[0]);executeCommandLists(lists[0],f);}
 void waitForCommandFence(RenderCommandFence*){++waits;}
};
struct D3D12CommandQueue:RenderCommandQueue {NativeQueue native;NativeQueue* d3d=&native;};
''' + timestamp_write + r'''
}
''' + helper + r'''
using namespace legodimensions::gpu_native;
enum class Backend {kD3D12,kVulkan};enum class SyncReason {Resource=3};
struct State {
 Backend backend=Backend::kD3D12;unsigned command_slot_count=3,present_number=0;
 std::array<bool,12> frame_submitted{};std::array<u64,12> slot_submission{};
 u64 last_submission=0,completed_submission=0;bool command_list_open=false;
 std::array<std::unique_ptr<plume::RenderCommandFence>,12> frame_fences;
 std::array<std::unique_ptr<plume::RenderCommandSemaphore>,12> acquire_semaphores;
 std::array<std::unique_ptr<plume::RenderCommandSemaphore>,3> render_semaphores;
 plume::RenderDevice device;std::unique_ptr<plume::D3D12CommandQueue> queue=std::make_unique<plume::D3D12CommandQueue>();
 std::unique_ptr<GpuSubmissionTimestamps> gpu_timestamps;plume::D3D12CommandList commands;
 double present_submit_cpu_ms=0;
 State(){for(auto& f:frame_fences)f=std::make_unique<plume::D3D12CommandFence>();for(auto& s:acquire_semaphores)s=std::make_unique<plume::RenderCommandSemaphore>();for(auto& s:render_semaphores)s=std::make_unique<plume::RenderCommandSemaphore>();}
 auto& fence(unsigned slot){return *static_cast<plume::D3D12CommandFence*>(frame_fences[slot].get());}
};
void InitializeNullTextures(State& state,plume::RenderCommandList*){state.commands.native.tick+=100;}
''' + completion + begin + "\n" + "\n".join(paths) + present_result + r'''
std::vector<std::string> lines(const std::filesystem::path& path){std::ifstream f(path);std::vector<std::string> r;std::string s;while(std::getline(f,s))r.push_back(s);return r;}
int main(int argc,char** argv){assert(argc==2);const std::filesystem::path directory=argv[1];
 auto make=[&](State& state,const char* name,unsigned count=3){auto path=directory/name;return GpuSubmissionTimestamps::Create(&state.device,state.queue.get(),count,path.string().c_str(),"0123456789abcdef");};
 {
  State state;assert(!GpuSubmissionTimestamps::Create(&state.device,state.queue.get(),3,nullptr,"x"));assert(state.device.creates==0&&state.queue->native.frequency_calls==0);
  Open(state,0);Close_kickoff(state,0);state.fence(0).native.completed=1;assert(WaitForSubmissionLocked(state,0));
  assert(state.commands.native.writes==0&&state.queue->executes==1&&state.queue->waits==1);
 }
 {
  std::array<int,4> default_schedule{};
  for(bool diagnostic:{false,true}) {
   State state;if(diagnostic)state.gpu_timestamps=make(state,"same-schedule.csv");
   void(*close[])(State&,unsigned,unsigned)={Close_synchronize,Close_kickoff,Close_acquire_failed,Close_present};
   for(unsigned step=0;step<4;++step){const unsigned slot=step%3;Open(state,slot);close[step](state,slot,1);
    if(diagnostic)state.device.resource(slot).ready=true;
    state.fence(slot).native.completed=state.fence(slot).fenceValue-1;assert(WaitForSubmissionLocked(state,slot));
   }
   const std::array<int,4> schedule{state.commands.begins,state.commands.ends,state.queue->executes,state.queue->waits};
   if(!diagnostic)default_schedule=schedule;else {assert(schedule==default_schedule&&"timestamp unchanged submission/wait schedule oracle");state.gpu_timestamps->Shutdown(0);}
   assert(state.completed_submission==4&&state.commands.native.writes==(diagnostic?8:0));
  }
 }
 for(auto mode:{Failure::Frequency,Failure::ZeroFrequency,Failure::Heap,Failure::Buffer,Failure::Resource,Failure::Count}){
  State state;failure=mode;assert(!make(state,"allocation-failure.csv"));failure=Failure::None;
 }
 {
  State state;auto bad=directory/"absent-parent"/"file.csv";assert(!GpuSubmissionTimestamps::Create(&state.device,state.queue.get(),3,bad.string().c_str(),"x"));
 }
 {
  State state;state.command_slot_count=12;state.gpu_timestamps=make(state,"paths.csv",12);assert(state.gpu_timestamps&&state.device.creates==12);
  void(*close[])(State&,unsigned,unsigned)={Close_synchronize,Close_kickoff,Close_acquire_failed,Close_present};
  for(unsigned slot=0;slot<12;++slot){state.present_number=17;state.commands.native.tick=(1ull<<62)+slot*1000000;Open(state,slot);state.commands.native.tick+=2000000;close[slot%4](state,slot,1);assert(state.slot_submission[slot]==slot+1);}
  assert(state.commands.native.writes==24&&state.commands.native.resolves==24&&state.queue->executes==12);
  RefreshCompletedSubmissionsLocked(state);assert(state.completed_submission==0);
  for(unsigned slot=0;slot<12;++slot){assert(state.device.resource(slot).maps==0);state.device.resource(slot).ready=true;state.fence(slot).native.completed=1;}
  RefreshCompletedSubmissionsLocked(state);RefreshCompletedSubmissionsLocked(state);assert(state.completed_submission==12);
  for(unsigned slot=0;slot<12;++slot)assert(state.device.resource(slot).maps==1&&state.device.resource(slot).unmaps==1&&"timestamp exactly-once map oracle");
  assert(WaitForSubmittedFramesLocked(state));assert(state.queue->waits==12);
  for(unsigned slot=0;slot<12;++slot)assert(state.device.resource(slot).maps==1);
  // Same-slot reuse after actual wait, with a new ticket and present failure metadata.
  Open(state,0);Close_present(state,0,2);ReportPresent(state,0,false);
  state.device.resource(0).ready=true;state.fence(0).native.completed=2;assert(WaitForSubmissionLocked(state,0));
  state.gpu_timestamps->Shutdown(19);auto rows=lines(directory/"paths.csv");assert(rows.size()==14&&"timestamp all-path/double-drain oracle");
  for(unsigned slot=0;slot<12;++slot){assert(rows[slot+1].find(",18,"+std::to_string(slot)+",")!=std::string::npos);assert(rows[slot+1].ends_with(",2.000105000,ok"));}
  assert(rows[13].find(",13,present,4294967295,2,0,")!=std::string::npos);
 }
 for(auto mode:{Failure::Map,Failure::NullMap}) {
  State state;state.gpu_timestamps=make(state,mode==Failure::Map?"map.csv":"null-map.csv");Open(state,0);Close_kickoff(state,0);
  state.device.resource(0).ready=true;state.fence(0).native.completed=1;failure=mode;RefreshCompletedSubmissionsLocked(state);failure=Failure::None;
  RefreshCompletedSubmissionsLocked(state);Open(state,1);assert(state.commands.native.writes==2);state.gpu_timestamps->Shutdown(0);
  auto rows=lines(directory/(mode==Failure::Map?"map.csv":"null-map.csv"));assert(rows.size()==2&&rows[1].ends_with(",,map_failed"));
  assert(state.device.resource(0).unmaps==(mode==Failure::Map?0:1));
 }
 {
  State state;state.gpu_timestamps=make(state,"removed.csv");Open(state,0);Close_present(state,0);
  state.fence(0).native.completed=UINT64_MAX;RefreshCompletedSubmissionsLocked(state);assert(!WaitForSubmissionLocked(state,0));assert(state.device.resource(0).maps==0);
  state.gpu_timestamps->Shutdown(0);auto rows=lines(directory/"removed.csv");assert(rows.size()==2&&rows[1].ends_with(",,device_removed"));
 }
 {
  State state;state.gpu_timestamps=make(state,"short.csv");Open(state,0);Close_kickoff(state,0);
  assert(!WaitForSubmissionLocked(state,0));assert(state.device.resource(0).maps==0);
  state.gpu_timestamps->Complete(0,100,0,0);assert(state.device.resource(0).maps==0);
  state.gpu_timestamps->Shutdown(0);auto rows=lines(directory/"short.csv");assert(rows.size()==2&&rows[1].ends_with(",,unverified_shutdown"));
 }
 {
  State state;state.gpu_timestamps=make(state,"unsubmitted.csv");Open(state,0);state.gpu_timestamps->Shutdown(0);
  assert(state.device.resource(0).maps==0&&state.queue->executes==0);auto rows=lines(directory/"unsubmitted.csv");assert(rows.size()==2&&rows[1].ends_with(",,unsubmitted_shutdown"));
 }
 {
  State state;state.gpu_timestamps=make(state,"reversed.csv");Open(state,0);Close_kickoff(state,0);auto& r=state.device.resource(0);r.ticks[0]=2;r.ticks[1]=1;r.ready=true;state.fence(0).native.completed=1;
  RefreshCompletedSubmissionsLocked(state);state.gpu_timestamps->Shutdown(0);auto rows=lines(directory/"reversed.csv");assert(rows.size()==2&&rows[1].ends_with(",,reversed_ticks"));
 }
 {
  State state;state.gpu_timestamps=make(state,"unsafe-reuse.csv");Open(state,0);Close_kickoff(state,0);
  Open(state,0);assert(state.commands.native.writes==2);state.gpu_timestamps->Shutdown(0);assert(state.device.resource(0).maps==0);
 }
}
'''
    cpp = args.output / "gpu-timestamps.cpp"
    cpp.write_text(harness)
    executable = args.output / "gpu-timestamps.exe"
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(cpp), "-o", str(executable)],
                   check=True, timeout=45)
    subprocess.run([str(executable), str(args.output)], check=True, timeout=10)
    (args.output / "verification.json").write_text(json.dumps({
        "passed": True, "actual_helper": True, "actual_plume_timestamp_write": True,
        "actual_native_completion_and_four_close_paths": True, "fake_gpu": True,
        "checks": ["disabled no queries/maps", "actual queue frequency failure/zero", "partial allocations", "output failure",
                   "3/12 slots and all four close paths", "delayed completion", "one map per ticket", "wait/poll double-drain",
                   "same-slot reuse", "source-frame/image/present-failure metadata", "large absolute tick subtraction",
                   "failed/null map", "device removal", "short/invalid target", "unsubmitted shutdown", "reversed ticks", "unsafe reuse disables diagnostic"],
        "limits": ["Mock COM/fences test ownership and ordering; no real GPU latency/utilization result."]}, indent=2) + "\n")
    print("PASS: actual GPU timestamp helper, Plume writes and four native submission paths")


if __name__ == "__main__":
    main()
