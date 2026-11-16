// Console-only fixture: actual diagnostic object, synthetic original guest calls.
#include "tt_lock_trace.h"
#include <rex/hook.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
using namespace legodimensions::tt_lock_trace;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
extern "C" REX_FUNC(sub_82A2A260);
extern "C" REX_FUNC(sub_82A2A2B0);
extern "C" REX_FUNC(sub_82B73D48);
std::recursive_mutex guest_mutex;
std::atomic<unsigned>original_calls{0};
std::atomic<bool>main_holds{false},waiter_started{false};
DWORD main_tid=0;
thread_local unsigned guest_depth=0;
volatile uint64_t work_result=0;
extern "C" REX_FUNC(__imp__sub_82A2A260) {
  ++original_calls;
  if(GetCurrentThreadId()!=main_tid && main_holds.load())waiter_started.store(true);
  guest_mutex.lock();++guest_depth;
  ctx.r3.u64=1;ctx.lr=77;ctx.r9.u64=19;
}
extern "C" REX_FUNC(__imp__sub_82A2A2B0) {
  ++original_calls;CHECK(guest_depth>0);--guest_depth;
  ctx.r3.u64=91;ctx.lr=99;ctx.r9.u64=23;
  guest_mutex.unlock();
  if(GetCurrentThreadId()==main_tid && !guest_depth)main_holds.store(false);
}
extern "C" REX_FUNC(__imp__sub_82B73D48) {
  ++original_calls;CHECK(guest_depth>0);
  NativeDrawScope draw;
  uint64_t sum=0;for(uint64_t i=0;i<100000;++i)sum=(sum+i)^(sum>>3);work_result=sum;
  ctx.r3.u64=7;ctx.lr=11;ctx.r9.u64=13;
}
int main(int argc,char**argv) {
  CHECK(argc==3||argc==4);const bool enabled=std::strcmp(argv[2],"enabled")==0;
  const bool existing=argc==4;
  CHECK(!_putenv_s("LEGO_TT_LOCK_TRACE",enabled?argv[1]:"0"));main_tid=GetCurrentThreadId();
  PPCContext ctx{};ctx.r3.u64=123;sub_82A2A260(ctx,nullptr);
  CHECK(ctx.r3.u64==1&&ctx.lr==77&&ctx.r9.u64==19);
  ctx.r3.u64=123;sub_82A2A2B0(ctx,nullptr);
  CHECK(ctx.r3.u64==91&&ctx.lr==99&&ctx.r9.u64==23);
  if(!existing)CHECK(!std::filesystem::exists(argv[1]));
  {NativeDrawScope outside;} // No span or output without the target lock.
  ctx.r3.u64=kGraphicsLock;sub_82A2A260(ctx,nullptr);main_holds.store(true);
  ctx.r3.u64=kGraphicsLock;sub_82A2A260(ctx,nullptr);
  sub_82B73D48(ctx,nullptr);CHECK(ctx.r3.u64==7&&ctx.lr==11&&ctx.r9.u64==13);
  ctx.r3.u64=kGraphicsLock;sub_82A2A2B0(ctx,nullptr);
  if(!existing)CHECK(!std::filesystem::exists(argv[1])); // Recursive leave cannot emit.
  std::thread waiter([]{
    PPCContext other{};other.r3.u64=kGraphicsLock;sub_82A2A260(other,nullptr);
    CHECK(other.r3.u64==1&&other.lr==77&&other.r9.u64==19);
    other.r3.u64=kGraphicsLock;sub_82A2A2B0(other,nullptr);
    CHECK(other.r3.u64==91&&other.lr==99&&other.r9.u64==23);
  });
  const auto deadline=GetTickCount64()+2000;
  while(!waiter_started.load()&&GetTickCount64()<deadline)std::this_thread::yield();
  CHECK(waiter_started.load());Sleep(5);
  ctx.r3.u64=kGraphicsLock;sub_82A2A2B0(ctx,nullptr);waiter.join();
  CHECK(ctx.r3.u64==91&&ctx.lr==99&&ctx.r9.u64==23);
  CHECK(original_calls==9&&guest_depth==0);
  CHECK(std::filesystem::exists(argv[1])==(enabled||existing));
  std::printf("PASS actual diagnostic object: enabled=%u,9 original calls,recursive ownership,contended wait,register results,no game/window\n",enabled);
}
