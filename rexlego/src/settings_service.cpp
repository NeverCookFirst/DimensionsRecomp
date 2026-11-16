#include "settings_service.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace legodimensions::settings {
namespace {
std::mutex published_mutex;
std::weak_ptr<QueuedSettings> published;
Reply Unavailable(std::string reason) {
  Reply reply;
  reply.unavailable = true;
  reply.error = std::move(reason);
  return reply;
}
}  // namespace

struct QueuedSettings::State {
  State(Backend backend, Post post, Values defaults,
        std::set<std::string, std::less<>> confirm_keys, Now now)
      : owner(std::this_thread::get_id()), transaction(std::move(backend), std::move(confirm_keys)),
        post(std::move(post)), defaults(std::move(defaults)), now(std::move(now)) {}

  std::thread::id owner;
  Transaction transaction;
  Post post;
  Values defaults;
  Now now;
  std::atomic<bool> stopped{false};
  std::string startup_error;
  // Synchronizes enqueue with stop, so a request cannot call the host's Post
  // after ShutdownFromUI has returned and the context is being destroyed.
  std::mutex enqueue_mutex;

  bool IsOwner() const { return owner == std::this_thread::get_id(); }
  Reply Snapshot(bool ok, Clock::time_point time) const {
    Reply reply;
    reply.ok = ok;
    reply.phase = transaction.phase();
    reply.notice = transaction.notice();
    reply.values = transaction.draft();
    reply.dirty = transaction.dirty();
    reply.error = transaction.error();
    reply.startup_error = startup_error;
    if (reply.phase == Phase::kPreview)
      reply.confirmation_ms = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
          transaction.deadline() - time).count());
    return reply;
  }
  Reply Run(const Command& command) {
    if (!IsOwner()) return Unavailable("Native settings queue ran outside the platform UI thread");
    if (stopped.load()) return Unavailable("Native settings are shutting down");
    const auto time = now();
    transaction.Tick(time);
    bool ok = false;
    switch (command.kind) {
      case CommandKind::kOpen: ok = transaction.Open(); break;
      case CommandKind::kEdit: ok = transaction.Edit(command.key, command.value); break;
      case CommandKind::kDefaults: ok = transaction.Defaults(defaults); break;
      case CommandKind::kApply: ok = transaction.Apply(time); break;
      case CommandKind::kKeep: ok = transaction.Keep(time); break;
      case CommandKind::kRevert: ok = transaction.Revert(); break;
      case CommandKind::kCancel: ok = transaction.Cancel(); break;
      case CommandKind::kSnapshot: ok = true; break;
      case CommandKind::kAcknowledgeStartupError: startup_error.clear(); ok = true; break;
      default: return Unavailable("Unknown native settings command");
    }
    return Snapshot(ok, time);
  }
};

QueuedSettings::QueuedSettings(Backend backend, Post post, Values defaults,
                               std::set<std::string, std::less<>> confirm_keys, Now now)
    : state_(std::make_shared<State>(std::move(backend), std::move(post), std::move(defaults),
                                     std::move(confirm_keys), std::move(now))) {}

QueuedSettings::~QueuedSettings() {
  // Destruction may be on a guest thread holding the last temporary reference.
  // Only explicit UI shutdown may invoke registry/window rollback callbacks.
  std::lock_guard lock(state_->enqueue_mutex);
  state_->stopped.store(true);
}

std::future<Reply> QueuedSettings::Submit(Command command) {
  auto promise = std::make_shared<std::promise<Reply>>();
  auto future = promise->get_future();
  const auto state = state_;
  std::lock_guard lock(state->enqueue_mutex);
  if (state->stopped.load()) {
    promise->set_value(Unavailable("Native settings are shutting down"));
    return future;
  }
  try {
    if (!state->post || !state->post([state, promise, command = std::move(command)] {
          try { promise->set_value(state->Run(command)); }
          catch (const std::exception& e) { promise->set_value(Unavailable(e.what())); }
        })) {
      promise->set_value(Unavailable("Native settings UI queue is unavailable"));
    }
  } catch (const std::exception& e) {
    promise->set_value(Unavailable(e.what()));
  }
  return future;
}

Reply QueuedSettings::TickFromUI() { return state_->Run({CommandKind::kSnapshot}); }

bool QueuedSettings::SetStartupErrorFromUI(std::string error) {
  if (!state_->IsOwner() || state_->stopped.load()) return false;
  state_->startup_error = std::move(error);
  return true;
}

bool QueuedSettings::AdvanceFromUI() {
  if (!state_->IsOwner() || state_->stopped.load()) return false;
  if (state_->transaction.pending() || state_->transaction.phase() == Phase::kPreview)
    state_->transaction.Tick(state_->now());
  return true;
}

Reply QueuedSettings::FocusLostFromUI() {
  if (!state_->IsOwner()) return Unavailable("Native settings focus callback ran outside the platform UI thread");
  if (state_->stopped.load()) return Unavailable("Native settings are shutting down");
  const auto time = state_->now();
  state_->transaction.Tick(time);
  bool ok = true;
  if (state_->transaction.pending() || state_->transaction.phase() == Phase::kPreview ||
      state_->transaction.phase() == Phase::kRecoveryFailed)
    ok = state_->transaction.Revert();
  return state_->Snapshot(ok, time);
}

Reply QueuedSettings::ShutdownFromUI() {
  if (!state_->IsOwner()) return Unavailable("Native settings shutdown ran outside the platform UI thread");
  {
    std::lock_guard lock(state_->enqueue_mutex);
    state_->stopped.store(true);
  }
  bool ok = true;
  if (state_->transaction.phase() != Phase::kClosed) ok = state_->transaction.Cancel();
  auto reply = state_->Snapshot(ok && state_->transaction.phase() == Phase::kClosed, state_->now());
  // A stopping UI cannot wait on render/OS work. The host must settle or tear
  // down its revision coordinator before destroying those consumers. Do not
  // report a submitted asynchronous rollback as a completed restoration.
  if (state_->transaction.pending()) reply.error = "Native settings restoration is still pending during shutdown";
  return reply;
}

void PublishNativeSettingsService(const std::shared_ptr<QueuedSettings>& service) {
  std::lock_guard lock(published_mutex);
  published = service;
}
std::shared_ptr<QueuedSettings> AcquireNativeSettingsService() {
  std::lock_guard lock(published_mutex);
  return published.lock();
}

}  // namespace legodimensions::settings
