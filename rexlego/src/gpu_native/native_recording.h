#pragma once

#include <cstdint>
#include <array>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <plume_render_interface.h>

namespace legodimensions::gpu_native {

struct NativeDeferredCommands : plume::RenderCommandList {
  virtual void Flush() = 0;
  virtual plume::RenderCommandList* Immediate() = 0;
  virtual void External(std::function<void(plume::RenderCommandList*)> command) = 0;
  // Copies/barriers may preserve graphics bindings. These callbacks must not
  // change pipelines, descriptors, root bindings, render targets or draw state.
  // Unknown implementations retain External's conservative invalidation.
  virtual void ResourceCommand(std::function<void(plume::RenderCommandList*)> command) {
    External(std::move(command));
  }
  virtual void SetBlendFactor(const std::array<float, 4>& value) = 0;
  virtual void Upload(void* destination, const void* source, uint32_t bytes) = 0;
};

struct NativeDrawCommands : NativeDeferredCommands {
  virtual void Replay(plume::RenderCommandList* commands) = 0;
  virtual void Reset() = 0;
  virtual bool Empty() = 0;
  // Fixes the trailing draws for repetition; false unless the list is state settings followed only by draws.
  virtual bool FreezeDrawTail() = 0;
  // Appends the frozen trailing draws again; throws unless the tail is frozen.
  virtual void RepeatDrawTail() = 0;
  // Reissues a frozen single-instance draw tail `instances` times through `pipeline` with per-instance vertex
  // constants at root descriptor 4, unfreezing it; false leaves the list unchanged.
  virtual bool InstanceDrawTail(const plume::RenderPipeline* pipeline, plume::RenderBufferReference constants,
                                uint32_t instances) = 0;
};

std::unique_ptr<NativeDrawCommands> CreateNativeDrawCommands();
void SetNativeBlendFactor(plume::RenderCommandList* commands, const std::array<float, 4>& value);
// Forgets the state tapes have applied to this list; the next tape command sets it again.
void ResetNativeReplayState(plume::RenderCommandList* commands);
bool NativeFullDrawPreparationEnabled();

inline plume::RenderCommandList* NativeImmediateCommands(plume::RenderCommandList* commands) {
  const auto deferred = dynamic_cast<NativeDeferredCommands*>(commands);
  return deferred ? deferred->Immediate() : commands;
}

inline void FlushNativeCommands(plume::RenderCommandList* commands) {
  if (const auto deferred = dynamic_cast<NativeDeferredCommands*>(commands)) deferred->Flush();
}

inline void RecordNativeUpload(plume::RenderCommandList* commands, void* destination,
                               const void* source, uint32_t bytes) {
  if (const auto deferred = dynamic_cast<NativeDeferredCommands*>(commands))
    deferred->Upload(destination, source, bytes);
  else std::memcpy(destination, source, bytes);
}

template<class Command>
void RecordNativeCommand(plume::RenderCommandList* commands, Command command) {
  if (const auto deferred = dynamic_cast<NativeDeferredCommands*>(commands))
    deferred->External(std::move(command));
  else command(commands);
}

bool NativePrepareEnabled();
unsigned NativePreparationWorkerCount();
void StopNativePreparation();
std::future<void> PostNativePreparation(std::function<void()> work, size_t bytes);
void PostNativePreparationDetached(std::function<void()> work, size_t bytes);
std::unique_ptr<plume::RenderCommandList> PrepareNativeCommandList(
    std::unique_ptr<plume::RenderCommandList> commands);

} // namespace legodimensions::gpu_native
