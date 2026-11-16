// Real SDK counter snapshot used by F3, with controlled native present times.
#include "gpu_native/fps_counter.h"
#include <rex/perf/counter.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
int main() {
  using namespace rex::perf;
  Init();legodimensions::gpu_native::NativeFpsCounter fps;
  fps.RecordPresent(1.0);
  const double times[]{1.006,1.017,1.023,1.052,1.058};
  const int64_t expected[]{6000,11000,6000,29000,6000};
  for(unsigned i=0;i<5;++i) {
    fps.RecordPresent(times[i]);
    SetCounter(CounterId::kFrameTimeUs,int64_t(fps.FrameTimeMilliseconds()*1000.0+0.5));
    SetCounter(CounterId::kFps,int64_t(fps.Value()));ResetFrameCounters();
    if(GetSnapshotCounter(CounterId::kFrameTimeUs)!=expected[i])return 23;
    std::printf("F3 snapshot=%lld us\n",GetSnapshotCounter(CounterId::kFrameTimeUs));
  }
  std::puts("PASS real SDK F3 snapshots follow individual native frame intervals");
}
