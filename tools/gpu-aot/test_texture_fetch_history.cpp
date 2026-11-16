#include "gpu_native/texture_fetch_history.h"
#include <cassert>
using namespace legodimensions::gpu_native;
int main() {
  TextureFetchHistory history;
  std::array<uint32_t,6> valid{2,0x100014,0x7FE3FF,0xD10,0x280,0x200200};
  assert(!history.Find(0x1000,5));
  history.Record(0x1000,5,0xED4B2674,0x82BCE9B8,valid);
  const auto* saved=history.Find(0x1000,5);
  assert(saved&&saved->caller==0x82BCE9B8&&saved->guest==0xED4B2674&&saved->ordinal==1);
  auto sampler=valid;sampler[0]^=0x3FFC00;sampler[1]^=0x800;sampler[3]^=0x7FF80000;sampler[4]^=0xFFFFFFFF;sampler[5]^=0x1FF;
  assert(TextureFetchHistory::ChangedWords(valid,sampler,false)==0x3B);
  assert(TextureFetchHistory::ChangedWords(valid,sampler,true)==0);
  auto corrupt=valid;corrupt[2]=0;corrupt[5]^=0x600;
  assert(TextureFetchHistory::ChangedWords(valid,corrupt,true)==0x24);
  auto moved=valid;moved[1]^=0x1000;
  assert(TextureFetchHistory::ChangedWords(valid,moved,true)==2);
  assert(!history.Find(0x2000,5)&&!history.Find(0x1000,26));
  history.Record(0x1000,5,0,0x83FAF744,{});
  assert(history.Find(0x1000,5)->guest==0&&history.Find(0x1000,5)->ordinal==2);
  history.Record(0x2000,5,0xCC,0xDD,valid);
  assert(!history.Find(0x1000,5)&&history.Find(0x2000,5));
}
