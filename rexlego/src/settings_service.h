#pragma once

#include "settings_transaction.h"

#include <future>
#include <memory>

namespace legodimensions::settings {

enum class CommandKind { kOpen, kEdit, kDefaults, kApply, kKeep, kRevert, kCancel, kSnapshot, kAcknowledgeStartupError };
struct Command {
  CommandKind kind = CommandKind::kSnapshot;
  std::string key, value;
};
struct Reply {
  bool ok = false;
  bool unavailable = false;
  Phase phase = Phase::kClosed;
  Notice notice = Notice::kNone;
  Values values;
  bool dirty = false;
  int64_t confirmation_ms = 0;
  std::string error;
  std::string startup_error;
};

// Construct on the platform UI thread. Guest frontend code only submits
// copied commands and polls futures; it must never wait on a future from the
// UI thread. Post must enqueue (never execute inline), returning false if the
// context is shutting down. Call ShutdownFromUI before destroying that context.
// Queued callbacks retain state, not the app/window, and become inert on stop.
class QueuedSettings {
 public:
  using Post = std::function<bool(std::function<void()>)>;
  using Now = std::function<Clock::time_point()>;
  QueuedSettings(Backend backend, Post post, Values defaults,
                 std::set<std::string, std::less<>> confirm_keys,
                 Now now = [] { return Clock::now(); });
  ~QueuedSettings();
  QueuedSettings(const QueuedSettings&) = delete;
  QueuedSettings& operator=(const QueuedSettings&) = delete;

  std::future<Reply> Submit(Command command);
  bool SetStartupErrorFromUI(std::string error);
  // Host frame heartbeat: no draft copy/allocation when the menu is idle.
  bool AdvanceFromUI();
  Reply TickFromUI();
  Reply FocusLostFromUI();
  Reply ShutdownFromUI();

 private:
  struct State;
  std::shared_ptr<State> state_;
};

// The app owns the service. Frontend hooks acquire a temporary reference;
// shutdown invalidates pending and future requests before the app goes away.
void PublishNativeSettingsService(const std::shared_ptr<QueuedSettings>& service);
std::shared_ptr<QueuedSettings> AcquireNativeSettingsService();

}  // namespace legodimensions::settings
