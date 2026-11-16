#pragma once
#include "tt_lock_trace_state.h"
namespace legodimensions::tt_lock_trace {
bool Enabled() noexcept;
ClockPoint SampleClock() noexcept;
void Acquired(ClockPoint before,ClockPoint after) noexcept;
void Released(ClockPoint end) noexcept;
class NativeDrawScope {
 public:
  NativeDrawScope() noexcept;
  ~NativeDrawScope();
  NativeDrawScope(const NativeDrawScope&)=delete;
  NativeDrawScope& operator=(const NativeDrawScope&)=delete;
 private:
  SpanToken token_{};
};
class RenderScope {
 public:
  RenderScope() noexcept;
  ~RenderScope();
  RenderScope(const RenderScope&)=delete;
  RenderScope& operator=(const RenderScope&)=delete;
 private:
  SpanToken token_{};
};
}
