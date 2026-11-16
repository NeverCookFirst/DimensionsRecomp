#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace legodimensions::tt_lock_trace {
constexpr uint32_t kGraphicsLock = 0x84959EDC;
struct ClockPoint {
  uint64_t wall=0,cpu=0;
  bool wall_valid=false,cpu_valid=false;
};
struct LockRow {
  ClockPoint waiting{},acquired{},released{};
  uint64_t draws=0,draw_ticks=0,renders=0,render_ticks=0;
  bool draw_valid=true,render_valid=true,scopes_closed=true,orphan=false;
};
enum class SpanKind { kNativeDraw, kRender };
struct SpanToken {
  uint64_t generation=0,start=0;
  SpanKind kind=SpanKind::kNativeDraw;
  bool active=false,outer=false,valid=false;
};
// Thread local: only the outer acquisition defines a wait/hold interval.
// A token cannot add time to another lock generation after unlock/reacquire.
class LockState {
 public:
  bool Held() const noexcept { return depth_!=0; }
  void Acquired(ClockPoint before,ClockPoint after) noexcept {
    if (depth_==std::numeric_limits<uint32_t>::max()) return;
    if (depth_++!=0) return;
    ++generation_;row_={};row_.waiting=before;row_.acquired=after;
    draw_depth_=render_depth_=0;
  }
  std::optional<LockRow> Released(ClockPoint end) noexcept {
    if (!depth_) { LockRow orphan;orphan.orphan=true;return orphan; }
    if (--depth_) return {};
    row_.released=end;row_.scopes_closed=!draw_depth_&&!render_depth_;
    return row_;
  }
  SpanToken Begin(SpanKind kind,uint64_t tick,bool valid) noexcept {
    if (!Held()) return {};
    auto& count=kind==SpanKind::kNativeDraw?row_.draws:row_.renders;
    auto& depth=kind==SpanKind::kNativeDraw?draw_depth_:render_depth_;
    auto& good=kind==SpanKind::kNativeDraw?row_.draw_valid:row_.render_valid;
    if (count==UINT64_MAX || depth==UINT32_MAX) { good=false;return {}; }
    ++count;const bool outer=depth++==0;
    return {generation_,tick,kind,true,outer,valid};
  }
  void End(SpanToken token,uint64_t tick,bool valid) noexcept {
    if (!token.active || !Held() || token.generation!=generation_) return;
    auto& depth=token.kind==SpanKind::kNativeDraw?draw_depth_:render_depth_;
    auto& sum=token.kind==SpanKind::kNativeDraw?row_.draw_ticks:row_.render_ticks;
    auto& good=token.kind==SpanKind::kNativeDraw?row_.draw_valid:row_.render_valid;
    if (!depth) {good=false;return;}
    --depth;
    if (!token.outer) return;
    if (depth || !valid || !token.valid || tick<token.start ||
        UINT64_MAX-sum<tick-token.start) {good=false;return;}
    sum+=tick-token.start;
  }
 private:
  uint32_t depth_=0,draw_depth_=0,render_depth_=0;
  uint64_t generation_=0;
  LockRow row_{};
};
inline bool WallValid(const LockRow& row) noexcept {
  return !row.orphan && row.waiting.wall_valid && row.acquired.wall_valid &&
      row.released.wall_valid && row.waiting.wall<=row.acquired.wall &&
      row.acquired.wall<=row.released.wall;
}
inline bool CpuValid(const LockRow& row) noexcept {
  return !row.orphan && row.waiting.cpu_valid && row.acquired.cpu_valid &&
      row.released.cpu_valid && row.waiting.cpu<=row.acquired.cpu &&
      row.acquired.cpu<=row.released.cpu;
}
} // namespace legodimensions::tt_lock_trace
