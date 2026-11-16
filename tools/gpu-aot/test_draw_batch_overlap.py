from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('output', type=Path)
out = parser.parse_args().output
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
draw = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
queue = draw[draw.index('void QueueNativeDrawBatch() {'):draw.index('bool CaptureParallelDraw(')]
flush = draw[draw.index('void FlushNativeDrawBatch() {'):draw.index('DrawTiming ConsumeDrawTiming()')]
retain = draw[draw.index('std::shared_ptr<void> RetainDrawBatchResources('):draw.index('struct FullDrawWork {')]
source = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <future>
#include <immintrin.h>
#include <memory>
#include <stdexcept>
#include <vector>
#include "gpu_native/draw_work_pool.h"
using namespace legodimensions::gpu_native;
using u8=unsigned char;using u32=uint32_t;using u64=uint64_t;
struct D3DDevice {};
struct Slice {u8* mapped=nullptr;u64 offset=0;explicit operator bool()const{return mapped!=nullptr;}};
struct Owner {std::shared_ptr<void> owner;};
struct Version {std::shared_ptr<void> owner,storage;};
struct Shaders {std::array<std::shared_ptr<void>,2> owners;};
struct ParallelDrawWork {
  std::shared_ptr<D3DDevice> device=std::make_shared<D3DDevice>();
  int bindings=0;Shaders shaders;
  std::array<Owner,26> texture_inputs;
  std::array<Owner,16> vertex_inputs;
  std::array<Version,16> vertices;
  Owner index_input;Version indices;
  u32 primitive=0,start=0,count=3;int base_vertex=0;bool indexed=true,ready=false;
  u64 serial=0;Slice allocation;
};
struct PendingDrawBatch {
  std::shared_ptr<std::vector<std::shared_ptr<ParallelDrawWork>>> draws;
  std::vector<std::future<void>> jobs;
};
std::vector<std::shared_ptr<ParallelDrawWork>> g_draw_batch;
std::vector<PendingDrawBatch> g_pending_draw_batches;
std::atomic<bool> g_draw_batch_pending{true};
std::atomic<u64> g_parallel_draws{0},g_parallel_jobs{0},g_parallel_work_us{0},g_parallel_wait_us{0};
ParallelDrawWork* t_recording_draw=nullptr;
std::vector<std::shared_ptr<void>> retired;
std::vector<u32> recorded;
std::array<u8,4*1024*1024> storage;
u32 used=0;
bool fail_allocation=false,allow_fallback=false;
int fail_draw=-1;
std::shared_future<void> gate;
std::atomic<unsigned> active{0},finished{0};
struct HostDevice {
  static Slice AllocateDrawUpload(u32 bytes){
    if(fail_allocation)return {};
    assert(used+bytes<=storage.size());Slice result{storage.data()+used,used};used+=bytes;return result;
  }
  struct View {u64 recording_serial=7;};
  static View CurrentDrawDeviceView(){return {};}
  static void RetireResource(std::shared_ptr<void> resource){retired.push_back(std::move(resource));}
};
unsigned NativePreparationWorkerCount(){return 3;}
std::future<void> PostNativePreparation(std::function<void()> work,size_t){
  return std::async(std::launch::async,[work=std::move(work)]{++active;gate.wait();try{work();}catch(...){--active;throw;}--active;});
}
void PrepareDrawWork(ParallelDrawWork& work){
  if(int(work.start)==fail_draw)throw std::runtime_error("preparation failure");
  assert(work.serial==7);work.allocation.mapped[0]=u8(work.start);work.ready=true;++finished;
}
struct DrawBindingsScope {explicit DrawBindingsScope(int){}};
struct ShaderBindingsScope {explicit ShaderBindingsScope(const Shaders&){}};
bool PooledDrawWorkEnabled(){return true;}
bool DispatchDraw(D3DDevice*,u32,bool,u32 start,u32,int){
  assert(!active && (t_recording_draw->ready || allow_fallback));
  if(!allow_fallback)assert(t_recording_draw->allocation.mapped[0]==u8(start));
  recorded.push_back(start);return true;
}
void FailActiveQueries(){assert(false);}
'''
source += retain + queue + flush
source += r'''
void AddBatch(unsigned start,unsigned count){
  for(unsigned i=0;i<count;++i){auto work=std::make_shared<ParallelDrawWork>();work->start=start+i;g_draw_batch.push_back(work);}
  g_draw_batch_pending=true;
}
int main(){
  std::promise<void> release;gate=release.get_future().share();
  std::weak_ptr<ParallelDrawWork> lifetime;
  std::weak_ptr<void> gpu_lifetime;
  for(unsigned batch=0;batch<4;++batch){
    AddBatch(batch*32,32);
    if(!batch){lifetime=g_draw_batch.front();auto owner=std::make_shared<int>(5);
      gpu_lifetime=owner;g_draw_batch.front()->vertices[0].storage=owner;}
    QueueNativeDrawBatch();
  }
  assert(g_pending_draw_batches.size()==4 && recorded.empty() && !finished && !lifetime.expired());
  release.set_value();FlushNativeDrawBatch();
  assert(recorded.size()==128 && finished==128 && !g_draw_batch_pending);
  for(unsigned i=0;i<128;++i)assert(recorded[i]==i);
  assert(lifetime.expired() && !gpu_lifetime.expired());
  retired.clear();assert(gpu_lifetime.expired());
  struct Pooled {unsigned value=0,resets=0;void Reset(){value=0;++resets;}};
  DrawWorkPool<Pooled,2> pool;
  auto a=pool.Acquire();a->value=42;auto b=pool.Acquire();auto c=pool.Acquire();
  assert(a->value==42 && a.get()!=c.get());
  auto* address=a.get();a.reset();b.reset();c.reset();
  bool reused=false;
  for(unsigned i=0;i<8 && !reused;++i){auto next=pool.Acquire();
    if(next.get()==address){assert(next->value==0 && next->resets==2);reused=true;}}
  assert(reused);
  auto duplicate=std::make_shared<int>(1);
  DrawResourceLease::Builder builder;builder.Add(duplicate);builder.Add(duplicate);
  auto lease=builder.Finish();assert(duplicate.use_count()==2);lease.reset();assert(duplicate.use_count()==1);
  DrawResourceLease::Builder crowded;
  std::vector<std::weak_ptr<void>> retained_owners;
  for(unsigned i=0;i<4200;++i){auto owner=std::make_shared<unsigned>(i);retained_owners.push_back(owner);crowded.Add(owner);}
  auto crowded_lease=crowded.Finish();
  for(auto& owner:retained_owners)assert(!owner.expired());
  crowded_lease.reset();for(auto& owner:retained_owners)assert(owner.expired());
  recorded.clear();AddBatch(128,5);FlushNativeDrawBatch();
  assert(recorded==std::vector<u32>({128,129,130,131,132}));
  recorded.clear();fail_allocation=true;allow_fallback=true;AddBatch(133,2);FlushNativeDrawBatch();
  assert(recorded==std::vector<u32>({133,134}));
  fail_allocation=false;allow_fallback=false;recorded.clear();fail_draw=200;
  AddBatch(200,32);QueueNativeDrawBatch();AddBatch(232,32);QueueNativeDrawBatch();
  bool failed=false;try{FlushNativeDrawBatch();}catch(const std::runtime_error&){failed=true;}
  assert(failed && !active && recorded.empty());
}
'''
cpp = out / 'draw-batch-overlap.cpp'
cpp.write_text(source)
exe = out / 'draw-batch-overlap.exe'
subprocess.run(['clang++', '-std=c++23', '-O2', '-UNDEBUG', '-I' + str(root / 'rexlego/src'), str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True, timeout=30)
print('PASS production batch overlap: asynchronous capture, FIFO commit, fence ownership, tails, allocation fallback and failure propagation')
