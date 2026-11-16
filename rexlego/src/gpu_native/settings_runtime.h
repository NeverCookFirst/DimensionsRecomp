#pragma once

#include "gpu_native/settings_snapshot.h"
#include <atomic>

namespace rex::ui { class Window; }
namespace legodimensions::gpu_native {

struct NativeSettingsFrame {
  settings::RevisionTicket ticket;
  NativeRenderSettingsSnapshot values;
};

// UI owns construction, registry work and Stop. Render entry points require
// HostDevice::LockRecording; their immutable frame survives UI teardown.
class NativeSettingsRuntime {
 public:
  NativeSettingsRuntime(settings::Backend registry, rex::ui::Window* window);
  settings::Backend MakeBackend() const;
  bool AdvanceFromUI();
  void StopFromUI();
  std::shared_ptr<const NativeSettingsFrame> Active() const;
  NativeFsr3ApplyResult CommitFrameBoundary();
  void Presented(const std::shared_ptr<const NativeSettingsFrame>& frame,
                 settings::PresentationObservation observation, bool effects_ready);
 private:
  std::unique_ptr<settings::SettingsRevisionCoordinator> coordinator_;
  std::atomic<std::shared_ptr<const NativeSettingsFrame>> active_;
};

void PublishNativeSettingsRuntime(std::shared_ptr<NativeSettingsRuntime> runtime);
std::shared_ptr<NativeSettingsRuntime> AcquireNativeSettingsRuntime();

}  // namespace legodimensions::gpu_native
