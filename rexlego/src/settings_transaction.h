#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace legodimensions::settings {

using Values = std::map<std::string, std::string, std::less<>>;
using Clock = std::chrono::steady_clock;

struct NormalizedValue {
  bool valid = false;
  std::string value;
  std::string error;
};

enum class RevisionState { kPending, kComplete, kFailed };
struct RevisionResult {
  uint64_t revision = 0;
  RevisionState state = RevisionState::kFailed;
  std::string error;
};

// Invoked on the owning thread (the platform UI thread for the real backend).
// read and normalize use the same canonical value representation. write
// must report failures, including callbacks which partially mutate a
// value before failing. persist must publish the entire change set atomically
// or leave the previous config intact.
struct Backend {
  std::function<Values()> read;
  std::function<NormalizedValue(std::string_view, std::string_view)> normalize;
  std::function<bool(std::string_view, std::string_view)> write;
  std::function<bool(const Values&, std::string&)> persist;
  // Optional asynchronous whole-request path. Supply both callbacks. submit
  // copies the changed values and supersedes the previous request, including
  // a pending apply being replaced by rollback. It must include registry writes
  // and stage/publish one coherent renderer/window revision. Even a failed or
  // throwing submit may have partially changed state: rollback includes every
  // proposed key. Accepted revisions are nonzero and strictly increasing.
  // observe must not wait; kComplete means the revision's actual consumers
  // have applied it, not merely that registry setters accepted the values.
  std::function<RevisionResult(const Values&)> submit;
  std::function<RevisionResult(uint64_t)> observe;
};

enum class Phase { kClosed, kEditing, kPreview, kRecoveryFailed, kApplying, kRestoring };
enum class Notice {
  kNone, kInvalidValue, kConflict, kApplyFailed, kSaveFailed,
  kKeepChanges, kReverted, kSaved, kRecoveryFailed, kApplying, kRestoring
};

// The native menu owns the draft; it never modifies the registry while the
// player browses. All notifications can be rendered by the title's native
// GenericPage component. No function-key UI or ImGui dependency lives here.
class Transaction {
 public:
  explicit Transaction(Backend backend, std::set<std::string, std::less<>> confirm_keys)
      : backend_(std::move(backend)), confirm_keys_(std::move(confirm_keys)) {}

  bool Open();
  bool Edit(std::string_view key, std::string_view value);
  bool Defaults(const Values& defaults);
  bool Apply(Clock::time_point now, std::chrono::seconds confirmation = std::chrono::seconds(15));
  bool Keep(Clock::time_point now);
  bool Revert();
  bool Cancel();
  void Tick(Clock::time_point now);

  Phase phase() const { return phase_; }
  Notice notice() const { return notice_; }
  const std::string& error() const { return error_; }
  const Values& draft() const { return draft_; }
  bool dirty() const { return draft_ != baseline_; }
  Clock::time_point deadline() const { return deadline_; }
  bool pending() const { return phase_ == Phase::kApplying || phase_ == Phase::kRestoring; }
  void ClearNotice() { notice_ = Notice::kNone; error_.clear(); }

 private:
  bool CheckCurrent(const Values& expected, bool changed_only);
  bool Restore();
  bool Save();
  bool ApplyAsync(Clock::time_point now, std::chrono::seconds confirmation);
  bool FinishAsyncApply(Clock::time_point now);
  bool RestoreAsync(bool discard, bool close, Notice notice, std::string error = {});
  bool FinishAsyncRestore();
  bool AcceptRevision(const RevisionResult& result);
  bool RecoveryFailed(std::string error);
  void Fail(Notice notice, std::string error);

  Backend backend_;
  std::set<std::string, std::less<>> confirm_keys_;
  Values baseline_, draft_, applied_;
  std::vector<std::string> written_;
  Phase phase_ = Phase::kClosed;
  Notice notice_ = Notice::kNone;
  std::string error_;
  Clock::time_point deadline_{};
  Clock::time_point last_now_{}, operation_deadline_{};
  std::chrono::seconds confirmation_{15};
  uint64_t revision_ = 0, latest_revision_ = 0;
  bool needs_confirmation_ = false, discard_on_restore_ = false, close_on_restore_ = false;
  Notice restored_notice_ = Notice::kReverted;
  std::string restored_error_;
};

}  // namespace legodimensions::settings
