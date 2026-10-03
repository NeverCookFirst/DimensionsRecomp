#include "gpu_native/pool_copy.h"

#include <cassert>
#include <limits>
#include <iostream>

using namespace legodimensions::gpu_native;

int main() {
  // Touching allocations must not evict each other's cached resources.
  assert(!CopyRangesOverlap({0x1000, 64}, {0x1040, 64}));
  assert(CopyRangesOverlap({0x1000, 64}, {0x103F, 64}));
  assert(CopyRangesOverlap({0x103F, 64}, {0x1000, 64}));
  assert(!CopyRangesOverlap({0x1000, 0}, {0x1000, 64}));
  assert(CopyRangesOverlap({0xFFFFFFF0, 32}, {0xFFFFFFFE, 1}));
  assert(!CopyRangesOverlap({0xFFFFFFF0, 32}, {0, 32}));

  PoolCopyDraws copy{32769};
  assert(!copy.Consume(4, 0, 16384));  // A triangle pass never matches.
  assert(!copy.Consume(1, 1, 16384));  // Only consecutive records are consumed.
  assert(!copy.Consume(1, 0, 16385));
  assert(copy.consumed == 0);
  assert(copy.Consume(1, 0, 16384));
  assert(!copy.Consume(1, 0, 16384)); // Repeated batch must not disappear.
  assert(copy.Consume(1, 16384, 16384));
  assert(!copy.Consume(1, 32768, 2));
  assert(copy.Consume(1, 32768, 1));
  assert(copy.consumed == copy.records);
  assert(!copy.Consume(1, 32769, 1));
  assert(!copy.Consume(1, 32769, 0));
  PoolCopyDraws empty{};
  assert(!empty.Consume(1, 0, 1));
  std::cout << "pool copy overlap and POINTLIST batch checks passed\n";
}
