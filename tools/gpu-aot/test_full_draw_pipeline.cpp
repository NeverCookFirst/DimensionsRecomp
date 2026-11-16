#include "gpu_native/native_worker.h"
#include "gpu_native/native_recording.h"
#include "gpu_native/draw_batch.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>
using namespace std::chrono_literals;
namespace legodimensions::gpu_native {
std::atomic<unsigned> captured{0};
std::atomic<bool> batch_pending{false};
std::vector<int> pending, recorded;
std::shared_future<void> gate;
std::mutex progress_mutex;
std::condition_variable progress;
DWORD coordinator=0, recorder=0;
bool NativePrepareEnabled(){return true;}
bool NativeFullDrawPreparationEnabled(){return true;}
bool NativeParallelDrawsEnabled(){return false;}
bool NativeDrawBatchPending(){return batch_pending.load();}
void StopNativePreparation(){}
void FlushNativeDrawBatch(){assert(false);}
void FlushFullDrawBatchIdle(){}
void RecordDeferredNativeTask(std::function<void()>& task){FlushFullDrawBatch();NativeRecorderSubmit(std::move(task));}
void FlushFullDrawBatch(){
  if(pending.empty())return;
  auto batch=std::move(pending);pending.clear();
  NativeRecorderSubmit([batch=std::move(batch)]{
    gate.wait();
    const auto thread=GetCurrentThreadId();
    assert(thread!=coordinator);
    if(!recorder)recorder=thread;
    assert(thread==recorder && OnNativeWorkerThread());
    for(int value:batch)recorded.push_back(value);
  });
  batch_pending=false;
}
}
int main(){
  using namespace legodimensions::gpu_native;
  _putenv_s("LEGO_NATIVE_THREADED","1");
  std::promise<void> release;gate=release.get_future().share();
  for(int value=0;value<128;++value){
    NativeWorkerSubmit([value]{
      coordinator=GetCurrentThreadId();pending.push_back(value);batch_pending=true;
      if(pending.size()==32)FlushFullDrawBatch();
      ++captured;progress.notify_all();
    },false,true,true);
    if(value==63)NativeWorkerSubmit([]{recorded.push_back(-1);});
  }
  std::unique_lock lock(progress_mutex);
  assert(progress.wait_for(lock,5s,[]{return captured==128;}));
  lock.unlock();
  auto drained=std::async(std::launch::async,[]{NativeWorkerDrain();});
  assert(drained.wait_for(50ms)==std::future_status::timeout);
  std::promise<void> later_release;
  auto later_gate = later_release.get_future().share();
  NativeWorkerSubmit([later_gate] { later_gate.wait(); });
  release.set_value();
  assert(drained.wait_for(5s)==std::future_status::ready);drained.get();
  later_release.set_value();
  NativeWorkerDrain();
  assert(recorded.size()==129);
  for(int i=0;i<64;++i)assert(recorded[i]==i);
  assert(recorded[64]==-1);
  for(int i=64;i<128;++i)assert(recorded[i+1]==i);
  NativeWorkerSubmit([]{assert(OnNativeWorkerThread());},true);
  NativeWorkerThrottleFrames(0);
  {
    NativeWorkerDrain();
    std::unique_lock held(NativeRecordingMutex());
    NativeWorkerSubmit([] { std::unique_lock lock(NativeRecordingMutex()); });
    NativeWorkerDrain();
    std::unique_lock nested(NativeRecordingMutex());
    assert(NativeRecordingMutex().HeldByCurrentThread());
  }
  assert(!NativeRecordingMutex().HeldByCurrentThread());
  NativeWorkerDrain();
  const auto before_tail = recorded.size();
  NativeWorkerSubmit([] { pending.push_back(128); batch_pending = true; }, false, true, true);
  NativeWorkerDrain();
  assert(recorded.size() == before_tail + 1 && recorded.back() == 128);
  StopNativeWorker();
  std::puts("PASS production coordinator advances past blocked recorder, FIFO epochs, bounded drain with later blocked work, present throttle, shutdown");
}
