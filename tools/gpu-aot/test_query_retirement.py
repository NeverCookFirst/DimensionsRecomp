"""Execute native query generation/release/readback bodies with fake GPU fences."""
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
    text = (root / path).read_text()
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

query_path = 'rexlego/src/gpu_native/queries.cpp'
functions = '\n'.join(body(query_path, sig) for sig in
                      ['u32 IssueQuery(', 'u32 GetQueryData(', 'u32 ReleaseQuery('])
assert 'Synchronize' not in functions
retire = body('rexlego/src/gpu_native/device.cpp', 'void HostDevice::RetireResource(')
harness = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
using u32=uint32_t;using u64=uint64_t;using be_u32=u32;
#define REXLOG_INFO(...) ((void)0)
#define FAILED(x) ((x)!=0)
#define SUCCEEDED(x) ((x)==0)
#define IID_PPV_ARGS(x) (x)
constexpr int D3D12_FENCE_FLAG_NONE=0;
constexpr u32 kInvalid=0x8876086C,kUnsupported=0x80004001,kFailure=0x80004005;
template<class T>struct ComPtr {
 std::shared_ptr<T> p;
 T* Get()const{return p.get();} T* operator->()const{return p.get();}
 ComPtr* operator&(){return this;}
};
struct ID3D12Fence {u64 completed=0;u64 GetCompletedValue(){return completed;}};
struct D3D12_RANGE {size_t Begin,End;};
struct Readback {
 u64 value=0;bool fail=false;
 int Map(int,const D3D12_RANGE* r,void** out){assert(r->End==8);*out=&value;return fail?1:0;}
 void Unmap(int,const D3D12_RANGE* r){assert(r->End==0);}
};
struct Samples {std::shared_ptr<Readback> readback=std::make_shared<Readback>();};
struct Query {
 bool active=false,ended=false,failed=false;
 std::vector<std::shared_ptr<Samples>> draws;
 ComPtr<ID3D12Fence> completion;
};
std::mutex mutex,g_mutex;
std::unordered_map<u32,std::shared_ptr<Query>> queries;
std::atomic<u32> diagnostics{100};
std::array<u32,256> header{};
be_u32* Header(u32 a){return header.data()+a;}
u32 Unsupported(const char*,u32){return kUnsupported;}
struct Memory {int frees=0;void SystemHeapFree(u32 a){assert(a==1);++frees;}} memory;
#define REX_KERNEL_MEMORY() (&memory)
struct DeviceImpl {
 bool fail=false;
 int CreateFence(int,int,ComPtr<ID3D12Fence>* out){if(fail)return 1;out->p=std::make_shared<ID3D12Fence>();return 0;}
};
struct QueueImpl {bool fail=false;int Signal(ID3D12Fence* f,int value){assert(f&&value==1);return fail?1:0;}};
namespace plume {struct D3D12Device {DeviceImpl* d3d;};struct D3D12CommandQueue {QueueImpl* d3d;};}
DeviceImpl device_impl;QueueImpl queue_impl;
plume::D3D12Device device{&device_impl};plume::D3D12CommandQueue queue{&queue_impl};
struct State {
 static constexpr u32 kFramesInFlight=3;
 std::array<bool,3> frame_submitted{true,false,false};
 bool command_list_open=true;u32 frame_slot=1;
 std::array<std::vector<std::shared_ptr<void>>,3> retired_resources;
};
std::unique_ptr<State> g_state=std::make_unique<State>();
struct HostDevice {
 static inline std::recursive_mutex recording;
 static inline bool submit_ok=true;
 static auto LockRecording(){return std::unique_lock(recording);}
 static auto Device(){return &device;} static auto Queue(){return &queue;}
 static bool SubmitRecordedWork(){return submit_ok;}
 static void RetireResource(std::shared_ptr<void>);
};
'''
tests = r'''
std::shared_ptr<Samples> sample(u64 value){auto s=std::make_shared<Samples>();s->readback->value=value;return s;}
void empty_slots(){for(auto& s:g_state->retired_resources)s.clear();}
int main(){
 auto initial=std::make_shared<Query>();initial->completion.p=std::make_shared<ID3D12Fence>();
 initial->draws.push_back(sample(999));initial->ended=true;
 std::weak_ptr<Query> old=initial;queries[1]=initial;initial.reset();Header(1)[3]=1;
 assert(IssueQuery(1,2)==0);auto current=queries[1];
 assert(current->active&&current->draws.empty()&&!current->ended&&!current->failed);
 assert(!old.expired()); // Previous submission and open list each hold it.
 g_state->retired_resources[0].clear();assert(!old.expired());
 g_state->retired_resources[1].clear();assert(old.expired());
 assert(IssueQuery(1,2)==kInvalid); // Active generation cannot be restarted.
 current->draws={sample(7),sample(5)};
 assert(IssueQuery(1,1)==0);assert(!current->active&&current->ended);
 assert(GetQueryData(1,100,4,0)==1); // Never invent a ready result.
 current->completion->completed=1;assert(GetQueryData(1,100,4,0)==0&&*Header(100)==12);
 current->completion->completed=UINT64_MAX;assert(GetQueryData(1,100,4,0)==kFailure);
 current->completion->completed=1;
 assert(GetQueryData(1,100,8,0)==kUnsupported);
 current->draws[0]->readback->fail=true;assert(GetQueryData(1,100,4,0)==kFailure);
 current->draws[0]->readback->fail=false;
 current->draws={sample(UINT32_MAX),sample(5)};
 assert(GetQueryData(1,100,4,0)==0&&*Header(100)==UINT32_MAX);
 device_impl.fail=true;assert(IssueQuery(1,2)==kFailure&&queries[1]==current&&current->ended);
 device_impl.fail=false;
 std::weak_ptr<Query> ended=current;current.reset();
 assert(IssueQuery(1,2)==0);assert(!ended.expired());empty_slots();assert(ended.expired());
 auto active=queries[1];active->draws={sample(19)};std::weak_ptr<Query> releasing=active;
 Header(1)[3]=2;assert(ReleaseQuery(1)==1&&queries.contains(1)&&active->active&&memory.frees==0);
 active.reset();assert(ReleaseQuery(1)==0); // Release before END is safe.
 assert(!queries.contains(1)&&memory.frees==1&&!releasing.expired());
 assert(GetQueryData(1,100,4,0)==kInvalid&&ReleaseQuery(1)==0&&memory.frees==1);
 g_state->retired_resources[0].clear();assert(!releasing.expired());
 g_state->retired_resources[1].clear();assert(releasing.expired());
 // No pending work needs retention. Same guest address may be reused.
 g_state->frame_submitted.fill(false);g_state->command_list_open=false;
 auto next=std::make_shared<Query>();queries[1]=next;Header(1)[3]=1;
 next->active=true;next->completion.p=std::make_shared<ID3D12Fence>();
 queue_impl.fail=true;assert(IssueQuery(1,1)==kFailure&&next->failed);
 assert(GetQueryData(1,100,4,0)==kFailure);
 std::weak_ptr<Query> done=next;next.reset();assert(ReleaseQuery(1)==0&&done.expired());
}
'''
source = a.output / 'query-retirement.cpp'
source.write_text(harness + retire + '\n' + functions + '\n' + tests)
exe = a.output / 'query-retirement-test.exe'
subprocess.run(['clang++', '-std=c++20', '-DNOMINMAX', str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
report = {'passed': True, 'actual_native_functions': ['IssueQuery','GetQueryData','ReleaseQuery','HostDevice::RetireResource'],
          'checks': ['no query drain', 'fresh generations', 'retain at every submitted/open slot',
                     'release before END', 'guest header reuse', 'nonfinal refcount', 'fence readiness',
                     'no fabricated result', 'removal failure', 'readback failure', 'saturating sum',
                     'allocation/signal failures'], 'real_GPU_gameplay_not_proven': True}
(a.output / 'verification.json').write_text(json.dumps(report, indent=2))
print('PASS: native query generations, fence-gated results and all-live-slot retirement.')
