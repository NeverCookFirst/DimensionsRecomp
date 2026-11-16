#include "settings_transaction.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace legodimensions::settings {

void Transaction::Fail(Notice notice, std::string error) {
  notice_ = notice;
  error_ = std::move(error);
}

bool Transaction::Open() {
  if (phase_ != Phase::kClosed) return false;
  ClearNotice();
  try {
    baseline_ = backend_.read();
    draft_ = baseline_;
    applied_.clear();
    written_.clear();
    phase_ = Phase::kEditing;
    return true;
  } catch (const std::exception& e) {
    Fail(Notice::kApplyFailed, e.what());
    return false;
  }
}

bool Transaction::Edit(std::string_view key, std::string_view value) {
  if (phase_ != Phase::kEditing || !draft_.contains(key)) return false;
  try {
    auto normalized = backend_.normalize(key, value);
    if (!normalized.valid) {
      Fail(Notice::kInvalidValue, normalized.error);
      return false;
    }
    draft_[std::string(key)] = std::move(normalized.value);
    ClearNotice();
    return true;
  } catch (const std::exception& e) {
    Fail(Notice::kInvalidValue, e.what());
    return false;
  }
}

bool Transaction::Defaults(const Values& defaults) {
  if (phase_ != Phase::kEditing) return false;
  Values next = draft_;
  try {
    for (const auto& [key, value] : defaults) {
      if (!next.contains(key)) {
        Fail(Notice::kInvalidValue, "Unknown setting: " + key);
        return false;
      }
      auto normalized = backend_.normalize(key, value);
      if (!normalized.valid) {
        Fail(Notice::kInvalidValue, normalized.error);
        return false;
      }
      next[key] = std::move(normalized.value);
    }
  } catch (const std::exception& e) {
    Fail(Notice::kInvalidValue, e.what());
    return false;
  }
  draft_ = std::move(next);
  ClearNotice();
  return true;
}

bool Transaction::CheckCurrent(const Values& expected, bool changed_only) {
  const auto current = backend_.read();
  for (const auto& [key, value] : expected) {
    if (changed_only && std::find(written_.begin(), written_.end(), key) == written_.end()) continue;
    const auto it = current.find(key);
    if (it == current.end() || it->second != value) {
      Fail(Notice::kConflict, "Setting changed elsewhere: " + key);
      return false;
    }
  }
  return true;
}

bool Transaction::Restore() {
  // A failed setter may have changed its value before reporting failure.
  // Keep every attempted key, restore in reverse order, and verify all reads.
  bool restored = true;
  std::string errors;
  for (auto it = written_.rbegin(); it != written_.rend(); ++it) {
    try {
      if (!backend_.write(*it, baseline_.at(*it))) {
        restored = false;
        errors += *it + " ";
      }
    } catch (...) {
      restored = false;
      errors += *it + " ";
    }
  }
  try {
    const auto current = backend_.read();
    for (const auto& key : written_) {
      const auto it = current.find(key);
      if (it == current.end() || it->second != baseline_.at(key)) {
        restored = false;
        errors += key + " ";
      }
    }
  } catch (...) {
    restored = false;
    errors += "readback";
  }
  if (!restored) {
    phase_ = Phase::kRecoveryFailed;
    Fail(Notice::kRecoveryFailed, "Could not restore settings: " + errors);
    return false;
  }
  written_.clear();
  applied_.clear();
  phase_ = Phase::kEditing;
  return true;
}

bool Transaction::Save() {
  if (pending()) return false;
  Values changes;
  for (const auto& key : written_) changes.emplace(key, draft_.at(key));
  std::string error;
  Values current;
  try {
    current = backend_.read();
    for (const auto& key : written_) {
      const auto it = current.find(key);
      if (it == current.end() || it->second != applied_.at(key)) {
        Fail(Notice::kConflict, "Setting changed elsewhere: " + key);
        return false;
      }
    }
    if (!backend_.persist(changes, error)) {
      Fail(Notice::kSaveFailed, error.empty() ? "Could not save settings" : std::move(error));
      return false;
    }
  } catch (const std::exception& e) {
    Fail(Notice::kSaveFailed, e.what());
    return false;
  }
  baseline_ = std::move(current);
  draft_ = baseline_;
  applied_.clear();
  written_.clear();
  phase_ = Phase::kEditing;
  notice_ = Notice::kSaved;
  error_.clear();
  return true;
}

bool Transaction::Apply(Clock::time_point now, std::chrono::seconds confirmation) {
  if (phase_ != Phase::kEditing) return false;
  ClearNotice();
  if (!dirty()) return true;
  try {
    if (!CheckCurrent(baseline_, false)) return false;
    // Revalidate the whole proposal before touching any live setting.
    for (const auto& [key, value] : draft_) {
      if (baseline_.at(key) == value) continue;
      auto normalized = backend_.normalize(key, value);
      if (!normalized.valid || normalized.value != value) {
        Fail(Notice::kInvalidValue, "Setting is no longer available: " + key);
        return false;
      }
    }
    if (bool(backend_.submit) != bool(backend_.observe)) {
      Fail(Notice::kApplyFailed, "Native settings acknowledgement is unavailable");
      return false;
    }
    if (backend_.submit) return ApplyAsync(now, confirmation);
    bool needs_confirmation = false;
    applied_ = baseline_;
    for (const auto& [key, value] : draft_) {
      if (baseline_.at(key) == value) continue;
      written_.push_back(key);
      if (!backend_.write(key, value)) {
        const bool restored = Restore();
        if (restored) Fail(Notice::kApplyFailed, "Could not apply setting: " + key);
        return false;
      }
      applied_[key] = value;
      needs_confirmation |= confirm_keys_.contains(key);
    }
    if (!CheckCurrent(applied_, true)) {
      const auto error = error_;
      if (Restore()) Fail(Notice::kApplyFailed, error);
      return false;
    }
    if (needs_confirmation) {
      phase_ = Phase::kPreview;
      deadline_ = now + std::clamp(confirmation, std::chrono::seconds(1), std::chrono::seconds(60));
      notice_ = Notice::kKeepChanges;
      return true;
    }
    // A save failure still has live changes to revert or retry. Keep the
    // original rollback point and use the same bounded confirmation state.
    phase_ = Phase::kPreview;
    deadline_ = now + std::chrono::seconds(15);
    return Save();
  } catch (const std::exception& e) {
    const std::string error = e.what();
    if (written_.empty() || Restore()) Fail(Notice::kApplyFailed, error);
    return false;
  }
}

bool Transaction::Keep(Clock::time_point now) {
  last_now_ = now;
  if (phase_ != Phase::kPreview) return false;
  if (now >= deadline_) {
    Revert();
    return false;
  }
  return Save();
}

bool Transaction::Revert() {
  if (backend_.submit && (phase_ == Phase::kApplying || phase_ == Phase::kRestoring ||
                         phase_ == Phase::kPreview || phase_ == Phase::kRecoveryFailed))
    return RestoreAsync(true, false, Notice::kReverted);
  if (phase_ != Phase::kPreview && phase_ != Phase::kRecoveryFailed) return false;
  if (!Restore()) return false;
  draft_ = baseline_;
  notice_ = Notice::kReverted;
  error_.clear();
  return true;
}

bool Transaction::Cancel() {
  if (backend_.submit && (phase_ == Phase::kApplying || phase_ == Phase::kRestoring ||
                         phase_ == Phase::kPreview || phase_ == Phase::kRecoveryFailed))
    return RestoreAsync(true, true, Notice::kReverted);
  if (phase_ == Phase::kPreview || phase_ == Phase::kRecoveryFailed) {
    if (!Revert()) return false;
  }
  if (phase_ != Phase::kEditing) return false;
  draft_ = baseline_;
  phase_ = Phase::kClosed;
  ClearNotice();
  return true;
}

void Transaction::Tick(Clock::time_point now) {
  last_now_ = now;
  if (pending()) {
    const bool applying = phase_ == Phase::kApplying;
    try {
      if (now >= operation_deadline_) {
        if (applying) RestoreAsync(false, false, Notice::kApplyFailed, "Timed out applying display settings");
        else RecoveryFailed("Timed out restoring display settings");
        return;
      }
      const auto result = backend_.observe(revision_);
      // A late receipt from the superseded apply cannot confirm rollback,
      // nor may an old rollback confirm a new proposal.
      if (result.revision != revision_) return;
      if (result.state == RevisionState::kPending) return;
      if (result.state == RevisionState::kComplete) {
        if (applying) FinishAsyncApply(now);
        else FinishAsyncRestore();
      } else if (applying) {
        RestoreAsync(false, false, Notice::kApplyFailed,
                     result.error.empty() ? "Could not apply display settings" : result.error);
      } else RecoveryFailed(result.error.empty() ? "Could not restore display settings" : result.error);
    } catch (const std::exception& e) {
      if (applying) RestoreAsync(false, false, Notice::kApplyFailed, e.what());
      else RecoveryFailed(e.what());
    }
    return;
  }
  if (phase_ == Phase::kPreview && now >= deadline_) Revert();
}

bool Transaction::AcceptRevision(const RevisionResult& result) {
  if (!result.revision || result.revision <= latest_revision_) return false;
  latest_revision_ = revision_ = result.revision;
  return true;
}

bool Transaction::RecoveryFailed(std::string error) {
  revision_ = 0;
  phase_ = Phase::kRecoveryFailed;
  Fail(Notice::kRecoveryFailed, "Could not restore settings: " + std::move(error));
  return false;
}

bool Transaction::ApplyAsync(Clock::time_point now, std::chrono::seconds confirmation) {
  if (!backend_.observe) {
    Fail(Notice::kApplyFailed, "Native settings acknowledgement is unavailable");
    return false;
  }
  last_now_ = now;
  confirmation_ = std::clamp(confirmation, std::chrono::seconds(1), std::chrono::seconds(60));
  needs_confirmation_ = false;
  Values changes;
  std::vector<std::string> keys;
  applied_ = draft_;
  for (const auto& [key, value] : draft_) {
    if (baseline_.at(key) == value) continue;
    changes.emplace(key, value);
    keys.push_back(key);
    needs_confirmation_ |= confirm_keys_.contains(key);
  }
  written_ = std::move(keys);
  phase_ = Phase::kApplying;
  deadline_ = {};
  operation_deadline_ = now + std::chrono::seconds(30);
  Fail(Notice::kApplying, {});
  try {
    const auto result = backend_.submit(changes);
    if (!AcceptRevision(result) || result.state == RevisionState::kFailed) {
      RestoreAsync(false, false, Notice::kApplyFailed,
                   result.error.empty() ? "Could not submit display settings" : result.error);
      return false;
    }
    if (result.state == RevisionState::kComplete) return FinishAsyncApply(now);
    return true;
  } catch (const std::exception& e) {
    RestoreAsync(false, false, Notice::kApplyFailed, e.what());
    return false;
  }
}

bool Transaction::FinishAsyncApply(Clock::time_point now) {
  try {
    if (!CheckCurrent(applied_, true)) {
      const auto error = error_;
      RestoreAsync(false, false, Notice::kApplyFailed, error);
      return false;
    }
  } catch (const std::exception& e) {
    RestoreAsync(false, false, Notice::kApplyFailed, e.what());
    return false;
  }
  revision_ = 0;
  phase_ = Phase::kPreview;
  deadline_ = now + (needs_confirmation_ ? confirmation_ : std::chrono::seconds(15));
  if (needs_confirmation_) {
    Fail(Notice::kKeepChanges, {});
    return true;
  }
  return Save();
}

bool Transaction::RestoreAsync(bool discard, bool close, Notice notice, std::string error) {
  discard_on_restore_ |= discard;
  close_on_restore_ |= close;
  if (phase_ == Phase::kRestoring) return true;
  restored_notice_ = notice;
  restored_error_ = std::move(error);
  Values restore;
  for (const auto& key : written_) restore.emplace(key, baseline_.at(key));
  phase_ = Phase::kRestoring;
  deadline_ = {};
  operation_deadline_ = last_now_ + std::chrono::seconds(30);
  Fail(Notice::kRestoring, {});
  try {
    const auto result = backend_.submit(restore);
    if (!AcceptRevision(result) || result.state == RevisionState::kFailed)
      return RecoveryFailed(result.error.empty() ? "Restoration request was rejected" : result.error);
    if (result.state == RevisionState::kComplete) return FinishAsyncRestore();
    return true;
  } catch (const std::exception& e) {
    return RecoveryFailed(e.what());
  }
}

bool Transaction::FinishAsyncRestore() {
  try {
    const auto current = backend_.read();
    for (const auto& key : written_) {
      const auto it = current.find(key);
      if (it == current.end() || it->second != baseline_.at(key))
        return RecoveryFailed("Restored value does not match: " + key);
    }
  } catch (const std::exception& e) {
    return RecoveryFailed(e.what());
  }
  if (discard_on_restore_ || close_on_restore_) draft_ = baseline_;
  written_.clear();
  applied_.clear();
  revision_ = 0;
  phase_ = close_on_restore_ ? Phase::kClosed : Phase::kEditing;
  Fail(close_on_restore_ ? Notice::kNone : restored_notice_,
       close_on_restore_ ? std::string{} : std::move(restored_error_));
  discard_on_restore_ = close_on_restore_ = false;
  return true;
}

}  // namespace legodimensions::settings
