#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include "gpu_native/tonemap_dof.h"
using legodimensions::gpu_native::ApplyDisabledTonemapDof;
int main() {
  std::array<float,1024> original;
  for (unsigned i=0;i<original.size();++i) original[i]=float(i)*0.125f;
  original[48]=1;original[49]=0;original[50]=0.2f;original[51]=7;
  auto host=original;
  constexpr std::uint64_t hash=0x3A47E5DDE66B42C6ull;
  assert(!ApplyDisabledTonemapDof(hash,false,host.data(),sizeof(host))&&host==original);
  assert(!ApplyDisabledTonemapDof(hash+1,true,host.data(),sizeof(host))&&host==original);
  assert(!ApplyDisabledTonemapDof(hash,true,host.data(),199)&&host==original);
  assert(!ApplyDisabledTonemapDof(hash,true,nullptr,sizeof(host)));
  assert(ApplyDisabledTonemapDof(hash,true,host.data(),sizeof(host)));
  for(unsigned i=0;i<host.size();++i)
    assert(host[i]==((i==48||i==49)?0:original[i]));
  // Actual translated mix, including fixed16's finite fetch exponent range.
  for(int i=-4096;i<=4096;++i) {
    const float alpha=float(i)/128;
    const float w=std::clamp(2*(alpha*host[48]+host[49])-1,0.0f,1.0f);
    assert(w==0);
  }
  assert(original[48]==1&&original[50]==0.2f); // Guest/source never touched.
  host=original; // Re-enabled setting uses a fresh draw snapshot.
  assert(!ApplyDisabledTonemapDof(hash,false,host.data(),sizeof(host))&&host==original);
}
