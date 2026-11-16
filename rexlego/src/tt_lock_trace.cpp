// Compile-opt-in diagnostic. Generic hooks intentionally support both routes.
#include "tt_lock_trace.h"
#include <rex/hook.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

extern "C" REX_FUNC(__imp__sub_82A2A260);
extern "C" REX_FUNC(__imp__sub_82A2A2B0);
extern "C" REX_FUNC(__imp__sub_82B73D48);

namespace legodimensions::tt_lock_trace {
namespace {
thread_local LockState state;
struct Config {
  std::string path;
  Config() {const char* value=std::getenv("LEGO_TT_LOCK_TRACE");
    if(value && *value && std::strcmp(value,"0"))path=value;}
};
const Config* Configuration() noexcept {
  try {static const Config config;return &config;} catch(...) {return nullptr;}
}
uint64_t WallTick() noexcept {
  LARGE_INTEGER now{};return QueryPerformanceCounter(&now)?uint64_t(now.QuadPart):0;
}
SpanToken BeginSpan(SpanKind kind) noexcept {
  if(!Enabled() || !state.Held())return {};
  const auto tick=WallTick();return state.Begin(kind,tick,tick!=0);
}
void EndSpan(SpanToken token) noexcept {
  if(!token.active)return;
  const auto tick=WallTick();state.End(token,tick,tick!=0);
}
struct Sink {
  std::mutex mutex;
  HANDLE file=INVALID_HANDLE_VALUE;
  uint64_t sequence=0;
  bool opened=false,failed=false;
  ~Sink(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
  bool Write(const char* bytes,DWORD size) noexcept {
    DWORD written=0;
    if(file==INVALID_HANDLE_VALUE || !WriteFile(file,bytes,size,&written,nullptr) || written!=size){failed=true;return false;}
    return true;
  }
  void Emit(const LockRow& row) {
    std::lock_guard lock(mutex);
    if(failed)return;
    if(!opened){
      opened=true;const auto* config=Configuration();if(!config || config->path.empty())return;
      file=CreateFileW(std::filesystem::path(config->path).c_str(),GENERIC_WRITE,
          FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(file==INVALID_HANDLE_VALUE){failed=true;return;}
      LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
      char metadata[192];const int n=std::snprintf(metadata,sizeof(metadata),
          "#tt_lock_trace_v1,qpc_frequency=%llu,target=0x84959EDC,cpu_unit=100ns\n",
          static_cast<unsigned long long>(frequency.QuadPart));
      if(n<=0 || n>=int(sizeof(metadata)) || !Write(metadata,DWORD(n)))return;
      constexpr char header[]="sequence,tid,lock,wait_begin_qpc,acquired_qpc,released_qpc,wait_cpu_100ns,hold_cpu_100ns,wall_valid,cpu_valid,native_draw_attempts,native_draw_qpc,native_valid,render_calls,render_qpc,render_valid,scopes_closed,orphan_release\n";
      if(!Write(header,DWORD(sizeof(header)-1)))return;
    }
    const bool wall=WallValid(row),cpu=CpuValid(row);
    char line[512];const int n=std::snprintf(line,sizeof(line),
        "%llu,%lu,0x84959EDC,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%u,%llu,%llu,%u,%u,%u\n",
        static_cast<unsigned long long>(++sequence),GetCurrentThreadId(),
        static_cast<unsigned long long>(row.waiting.wall),static_cast<unsigned long long>(row.acquired.wall),
        static_cast<unsigned long long>(row.released.wall),
        static_cast<unsigned long long>(cpu?row.acquired.cpu-row.waiting.cpu:0),
        static_cast<unsigned long long>(cpu?row.released.cpu-row.acquired.cpu:0),
        unsigned(wall),unsigned(cpu),static_cast<unsigned long long>(row.draws),
        static_cast<unsigned long long>(row.draw_ticks),unsigned(row.draw_valid),
        static_cast<unsigned long long>(row.renders),static_cast<unsigned long long>(row.render_ticks),
        unsigned(row.render_valid),unsigned(row.scopes_closed),unsigned(row.orphan));
    if(n<=0 || n>=int(sizeof(line))){failed=true;return;}
    Write(line,DWORD(n));
  }
};
void Emit(const LockRow& row) noexcept {
  try {static Sink sink;sink.Emit(row);}catch(...) {} // Diagnostic failure cannot alter guest control flow.
}
}
bool Enabled() noexcept {const auto* config=Configuration();return config && !config->path.empty();}
ClockPoint SampleClock() noexcept {
  ClockPoint stamp;stamp.wall=WallTick();stamp.wall_valid=stamp.wall!=0;
  FILETIME creation{},exit{},kernel{},user{};
  stamp.cpu_valid=GetThreadTimes(GetCurrentThread(),&creation,&exit,&kernel,&user)!=FALSE;
  if(stamp.cpu_valid){
    const auto ticks=[](FILETIME value){return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;};
    const auto a=ticks(kernel),b=ticks(user);
    stamp.cpu_valid=UINT64_MAX-a>=b;if(stamp.cpu_valid)stamp.cpu=a+b;
  }
  return stamp;
}
void Acquired(ClockPoint before,ClockPoint after) noexcept {state.Acquired(before,after);}
void Released(ClockPoint end) noexcept {if(auto row=state.Released(end))Emit(*row);}
NativeDrawScope::NativeDrawScope() noexcept:token_(BeginSpan(SpanKind::kNativeDraw)){}
NativeDrawScope::~NativeDrawScope(){EndSpan(token_);}
RenderScope::RenderScope() noexcept:token_(BeginSpan(SpanKind::kRender)){}
RenderScope::~RenderScope(){EndSpan(token_);}
}

REX_HOOK_RAW(sub_82A2A260) {
  using namespace legodimensions::tt_lock_trace;
  const bool trace=ctx.r3.u32==kGraphicsLock && Enabled();
  const auto before=trace?SampleClock():ClockPoint{};
  __imp__sub_82A2A260(ctx,base);
  if(trace)Acquired(before,SampleClock());
}
REX_HOOK_RAW(sub_82A2A2B0) {
  using namespace legodimensions::tt_lock_trace;
  const bool trace=ctx.r3.u32==kGraphicsLock && Enabled();
  const auto end=trace?SampleClock():ClockPoint{};
  __imp__sub_82A2A2B0(ctx,base);
  if(trace)Released(end); // All output follows the original outer unlock.
}
REX_HOOK_RAW(sub_82B73D48) {
  legodimensions::tt_lock_trace::RenderScope scope;
  __imp__sub_82B73D48(ctx,base);
}
