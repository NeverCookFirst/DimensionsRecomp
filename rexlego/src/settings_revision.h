#pragma once

#include "settings_transaction.h"
#include <memory>

namespace legodimensions::settings {

// The snapshot handed to rendering is immutable and contains the entire
// registry proposal, not a sequence of individual setter notifications.
struct SettingsRevision {
  uint64_t id = 0;
  Values values, changes;
};
using RevisionTicket = std::shared_ptr<const SettingsRevision>;

struct PresentationObservation {
  uint32_t width = 0, height = 0;
  bool vsync = false;
};

// UI-owned registry/window adapter; rendering holds only copied tickets and
// short metadata references. No renderer callback may touch the UI registry.
// Required acknowledgements are renderer commit, a presentation using that
// commit, and actual UI window/presentation agreement checked by WindowCheck.
class SettingsRevisionCoordinator {
 public:
  using WindowCheck = std::function<RevisionResult(const SettingsRevision&, const PresentationObservation&)>;
  using RenderCommit = std::function<RevisionResult(const SettingsRevision&)>;

  SettingsRevisionCoordinator(Backend registry, WindowCheck window_check);
  ~SettingsRevisionCoordinator();
  SettingsRevisionCoordinator(const SettingsRevisionCoordinator&) = delete;
  SettingsRevisionCoordinator& operator=(const SettingsRevisionCoordinator&) = delete;

  Backend MakeBackend() const;
  // Apply copied pending registry changes on the UI thread. Returns promptly
  // if rendering already owns a commit lease; there is no wait for its work.
  bool AdvanceFromUI();
  void StopFromUI();

  RevisionTicket PendingForRenderer() const;
  RevisionTicket ActiveForRenderer() const;
  // Caller prepares resource work before entering this callback. It must hold
  // the renderer recording lock; commit must not wait for UI work. Pending or
  // Failed must preserve the previous usable configuration. The lease orders
  // an already-started render commit before later UI writes, without making
  // the UI wait. Superseded tickets cannot start a commit or acknowledge it.
  bool TryCommit(const RevisionTicket& ticket, const RenderCommit& commit);
  bool AcknowledgePresentation(const RevisionTicket& ticket, PresentationObservation observation);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace legodimensions::settings
