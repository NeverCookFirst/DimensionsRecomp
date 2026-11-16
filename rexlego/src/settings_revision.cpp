#include "settings_revision.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace legodimensions::settings {
namespace {
constexpr unsigned kRender = 1, kPresent = 2, kWindow = 4, kComplete = 7;
struct Lease {
  std::atomic_flag& flag;
  bool owns;
  explicit Lease(std::atomic_flag& value) : flag(value), owns(!flag.test_and_set(std::memory_order_acquire)) {}
  ~Lease() { if (owns) flag.clear(std::memory_order_release); }
};
}

struct SettingsRevisionCoordinator::State {
  State(Backend backend, WindowCheck check)
      : owner(std::this_thread::get_id()), registry(std::move(backend)), window_check(std::move(check)) {
    if (!registry.read || !registry.normalize || !registry.write || !registry.persist ||
        registry.submit || registry.observe || !window_check)
      throw std::invalid_argument("Revision coordinator requires a synchronous registry and a window check");
    active = std::make_shared<SettingsRevision>(SettingsRevision{0, registry.read(), {}});
  }
  const std::thread::id owner;
  Backend registry;
  WindowCheck window_check;
  mutable std::mutex mutex;
  std::atomic_flag mutation = ATOMIC_FLAG_INIT;
  std::atomic<bool> stopped{false};
  bool writing = false;  // UI thread only, including reentrant SDK callbacks.
  uint64_t sequence = 0;
  RevisionTicket queued, sealed, active;
  unsigned acknowledged = 0;
  PresentationObservation presentation;
  std::string failure;

  void Owner() const {
    if (owner != std::this_thread::get_id())
      throw std::runtime_error("Settings revision registry access requires the platform UI thread");
    if (stopped.load()) throw std::runtime_error("Settings revision coordinator is stopped");
  }
  RevisionResult Status(uint64_t id) const {
    if (stopped.load()) return {id, RevisionState::kFailed, "Settings revision coordinator is stopped"};
    if (id != sequence) return {id, RevisionState::kFailed, "Settings revision was superseded"};
    if (!failure.empty()) return {id, RevisionState::kFailed, failure};
    return {id, acknowledged == kComplete ? RevisionState::kComplete : RevisionState::kPending, {}};
  }
  RevisionResult Submit(const Values& changes) {
    Owner();
    if (writing) throw std::runtime_error("Cannot submit a settings revision from a registry callback");
    // Copy before reserving a revision, so allocation/read failures cannot
    // replace a valid in-flight request with a half-formed proposal.
    auto request = std::make_shared<SettingsRevision>();
    const auto read = registry.read;
    request->values = read();
    request->changes = changes;
    for (const auto& [key, value] : changes) {
      if (!request->values.contains(key)) return {0, RevisionState::kFailed, "Unknown setting: " + key};
      request->values[key] = value;
    }
    std::lock_guard lock(mutex);
    if (stopped.load()) return {0, RevisionState::kFailed, "Settings revision coordinator is stopped"};
    if (sequence == UINT64_MAX) return {0, RevisionState::kFailed, "Settings revision counter exhausted"};
    request->id = ++sequence;
    queued = std::move(request);
    sealed.reset();
    acknowledged = 0;
    presentation = {};
    failure.clear();
    return {sequence, RevisionState::kPending, {}};
  }
  void Failed(const RevisionTicket& request, std::string error) {
    std::lock_guard lock(mutex);
    if (!stopped.load() && request && request->id == sequence)
      failure = error.empty() ? "Settings consumer rejected the revision" : std::move(error);
  }
  bool RegistryMatches(const RevisionTicket& request) {
    try {
      const auto read = registry.read;
      const auto current = read();
      for (const auto& [key, value] : request->values) {
        const auto it = current.find(key);
        if (it == current.end() || it->second != value) {
          Failed(request, "Setting changed after sealing revision: " + key);
          return false;
        }
      }
      return true;
    } catch (const std::exception& e) { Failed(request, e.what()); return false; }
  }
  RevisionResult Observe(uint64_t id) {
    Owner();
    RevisionTicket request;
    PresentationObservation observed;
    {
      std::lock_guard lock(mutex);
      const auto status = Status(id);
      if (status.state == RevisionState::kFailed || !sealed) return status;
      request = sealed;
      observed = presentation;
    }
    if (!RegistryMatches(request)) {
      std::lock_guard lock(mutex);
      return Status(id);
    }
    {
      std::lock_guard lock(mutex);
      const auto status = Status(id);
      if (status.state == RevisionState::kFailed || sealed != request ||
          (acknowledged & (kRender | kPresent)) != (kRender | kPresent)) return status;
    }
    try {
      const auto check = window_check;
      const auto result = check(*request, observed);
      std::lock_guard lock(mutex);
      if (!stopped.load() && sealed == request && id == sequence) acknowledged &= ~kWindow;
      if (!stopped.load() && sealed == request && id == sequence && result.revision == id) {
        if (result.state == RevisionState::kComplete) acknowledged |= kWindow;
        else if (result.state == RevisionState::kFailed)
          failure = result.error.empty() ? "Window did not apply the settings revision" : result.error;
      }
    } catch (const std::exception& e) { Failed(request, e.what()); }
    std::lock_guard lock(mutex);
    return Status(id);
  }
};

SettingsRevisionCoordinator::SettingsRevisionCoordinator(Backend registry, WindowCheck window_check)
    : state_(std::make_shared<State>(std::move(registry), std::move(window_check))) {}
SettingsRevisionCoordinator::~SettingsRevisionCoordinator() { state_->stopped.store(true); }

Backend SettingsRevisionCoordinator::MakeBackend() const {
  const auto state = state_;
  return {
    [state] { state->Owner(); const auto read=state->registry.read; return read(); },
    [state](auto key, auto value) { state->Owner(); const auto normalize=state->registry.normalize; return normalize(key, value); },
    // Coordinated live writes must go through Submit/Advance and its lease.
    [state](auto, auto) { state->Owner(); return false; },
    [state](const Values& changes, std::string& error) {
      state->Owner();
      uint64_t id;
      {
        std::lock_guard lock(state->mutex);
        id = state->sequence;
      }
      if (!id || state->Observe(id).state != RevisionState::kComplete) {
        error = "Settings revision has not completed or no longer matches the window";
        return false;
      }
      {
        std::lock_guard lock(state->mutex);
        if (!state->sequence || state->Status(state->sequence).state != RevisionState::kComplete) {
          error = "Settings revision has not completed";
          return false;
        }
      }
      const auto persist = state->registry.persist;
      return persist(changes, error);
    },
    [state](const Values& changes) { return state->Submit(changes); },
    [state](uint64_t id) { return state->Observe(id); },
  };
}

bool SettingsRevisionCoordinator::AdvanceFromUI() {
  auto& state = *state_;
  state.Owner();
  Lease lease(state.mutation);
  if (!lease.owns) return false;
  RevisionTicket request;
  {
    std::lock_guard lock(state.mutex);
    request = state.queued;
    if (!request) return true;
    state.queued.reset();
  }
  struct Writing {
    State& state;
    explicit Writing(State& value) : state(value) { state.writing = true; }
    ~Writing() { state.writing = false; }
  } writing(state);
  const auto registry = state.registry;
  try {
    // No coordinator metadata lock is held across SDK/window callbacks.
    // The mutation lease prevents rendering from starting a commit while the
    // UI changes the registry. Rendering keeps its old immutable active ticket.
    for (const auto& [key, value] : request->changes) {
      if (state.stopped.load()) return false;
      if (!registry.write(key, value)) {
        state.Failed(request, "Could not apply setting: " + key);
        return false;
      }
    }
    if (state.stopped.load()) return false;
    const auto current = registry.read();
    for (const auto& [key, value] : request->values) {
      const auto it = current.find(key);
      if (it == current.end() || it->second != value) {
        state.Failed(request, "Setting changed while sealing revision: " + key);
        return false;
      }
    }
    std::lock_guard lock(state.mutex);
    if (!state.stopped.load() && request->id == state.sequence) state.sealed = request;
    return true;
  } catch (const std::exception& e) {
    state.Failed(request, e.what());
    return false;
  }
}

void SettingsRevisionCoordinator::StopFromUI() {
  auto& state = *state_;
  if (state.owner != std::this_thread::get_id())
    throw std::runtime_error("Settings coordinator stop requires the platform UI thread");
  state.stopped.store(true);
  {
    std::lock_guard lock(state.mutex);
    state.queued.reset();
    state.sealed.reset();
  }
  // Keep an active ticket available until the renderer drains, but erase all
  // callbacks that may capture the app/window before those objects disappear.
  state.registry = {};
  state.window_check = {};
}

RevisionTicket SettingsRevisionCoordinator::PendingForRenderer() const {
  std::lock_guard lock(state_->mutex);
  if (state_->stopped.load() || !state_->failure.empty() || (state_->acknowledged & kRender)) return {};
  return state_->sealed;
}
RevisionTicket SettingsRevisionCoordinator::ActiveForRenderer() const {
  std::lock_guard lock(state_->mutex);
  return state_->active;
}

bool SettingsRevisionCoordinator::TryCommit(const RevisionTicket& ticket, const RenderCommit& commit) {
  if (!ticket || !commit) return false;
  auto& state = *state_;
  Lease lease(state.mutation);
  if (!lease.owns) return false;
  {
    std::lock_guard lock(state.mutex);
    if (state.stopped.load() || state.sealed != ticket || ticket->id != state.sequence ||
        !state.failure.empty() || (state.acknowledged & kRender)) return false;
  }
  RevisionResult result;
  try { result = commit(*ticket); }
  catch (const std::exception& e) { state.Failed(ticket, e.what()); return false; }
  if (result.revision != ticket->id) {
    state.Failed(ticket, "Renderer returned an acknowledgement for a different revision");
    return false;
  }
  std::lock_guard lock(state.mutex);
  if (result.state == RevisionState::kComplete) {
    // A later Submit may reserve a new revision during an already-started
    // commit, but its UI writes must wait for this lease. Record the actual
    // committed active ticket; never acknowledge it as the newer request.
    state.active = ticket;
    if (!state.stopped.load() && state.sealed == ticket && ticket->id == state.sequence) state.acknowledged |= kRender;
    return true;
  }
  if (result.state == RevisionState::kFailed && !state.stopped.load() && state.sealed == ticket && ticket->id == state.sequence)
    state.failure = result.error.empty() ? "Renderer could not prepare the settings revision" : result.error;
  return false;
}

bool SettingsRevisionCoordinator::AcknowledgePresentation(const RevisionTicket& ticket, PresentationObservation observation) {
  if (!ticket || !observation.width || !observation.height) return false;
  std::lock_guard lock(state_->mutex);
  if (state_->stopped.load() || state_->sealed != ticket || state_->active != ticket || ticket->id != state_->sequence ||
      !state_->failure.empty() || !(state_->acknowledged & kRender)) return false;
  state_->presentation = observation;
  state_->acknowledged |= kPresent;
  return true;
}

}  // namespace legodimensions::settings
