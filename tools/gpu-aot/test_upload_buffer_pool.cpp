#include "gpu_native/upload_buffer_pool.h"
#include <cassert>
using namespace legodimensions::gpu_native;
struct Buffer {
  static inline unsigned live=0;
  unsigned id,device;
  uint64_t capacity;
  Buffer(unsigned i,unsigned d,uint64_t c):id(i),device(d),capacity(c){++live;}
  ~Buffer(){--live;}
};
int main(){
  UploadBufferPool<Buffer> pool;
  unsigned allocations=0,device=1;
  const auto create=[&](uint64_t capacity){return std::make_unique<Buffer>(++allocations,device,capacity);};
  assert(!pool.Acquire(0,6,create));
  auto first=pool.Acquire(300,6,create);assert(first&&first->capacity==512);
  const auto id=first->id;auto in_flight=first;first.reset();
  // A recorded GPU owner forbids reuse, even after the resource's CPU cache
  // releases its own reference. Fence retirement releases in_flight later.
  auto other=pool.Acquire(300,6,create);assert(other->id!=id&&allocations==2);
  in_flight.reset();auto reused=pool.Acquire(400,6,create);
  assert(reused->id==id&&allocations==2&&pool.Stats().hits==1);
  auto index=pool.Acquire(400,7,create);assert(index->id!=id);
  index.reset();other.reset();reused.reset();
  // Physical allocation accounting includes DX12's minimum 64 KiB.
  assert(pool.Stats().bytes==3*65536&&pool.Stats().count==3);
  auto held=pool.Acquire(300,6,create);const auto old_device=held->device;
  pool.Reset();device=2;held.reset();assert(old_device==1);
  auto replacement=pool.Acquire(300,6,create);assert(replacement->device==2);
  replacement.reset();pool.Reset();assert(Buffer::live==0);
  assert(!pool.Acquire(16,6,[](uint64_t){return std::unique_ptr<Buffer>{};}));
  // An oversized single allocation never crowds out the bounded idle pool.
  auto large=pool.Acquire(65*1024*1024,6,create);large.reset();
  assert(pool.Stats().bytes==0);
  std::vector<std::shared_ptr<Buffer>> many;
  for(unsigned i=0;i<1000;++i)many.push_back(pool.Acquire(1024*1024,uint8_t(i%200),create));
  many.clear();assert(pool.Stats().bytes<=64*1024*1024&&pool.Stats().count<=512);
  pool.Reset();assert(Buffer::live==0);
}
