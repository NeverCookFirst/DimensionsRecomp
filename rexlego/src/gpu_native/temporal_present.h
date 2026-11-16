#pragma once
#include "gpu_native/temporal_camera.h"
#include "gpu_native/temporal_scene_depth.h"
#include "gpu_native/textures.h"
#include <optional>
#include <string_view>
namespace plume { struct RenderFramebuffer; struct RenderCommandList; }
namespace legodimensions::gpu_native {
struct PreparedTemporalSettings;
// Recording-lock scope only. Abandoned preparation releases only new resources;
// it leaves the current context/output available for an unchanged frame.
std::shared_ptr<PreparedTemporalSettings> PrepareTemporalSettings(uint32_t width,uint32_t height,bool inverted);
bool CanCommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& prepared);
bool CommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& prepared,bool reset_history=false);
bool TemporalDepthInverted();
// All calls are serialized by the recording lock. Persistent resources are
// consumed and overwritten in order on the same direct queue.
bool PrepareTemporalDraw(plume::RenderCommandList*, uint64_t frame,
    uint32_t width, uint32_t height, const std::optional<TemporalCamera>& camera,
    float& jitter_x, float& jitter_y);
plume::RenderFramebuffer* TemporalFramebuffer(plume::RenderTexture* depth);
TextureResourceView PresentTemporalFrame(TextureResourceView color,
    const TemporalSceneDepth& depth, uint64_t frame);
bool CompleteTemporalComposition(uint64_t frame);
bool TemporalSceneComposed(uint64_t frame);
void FailTemporalComposition(uint64_t frame);
// Normalized sampling offset for a spatial fallback of an already jittered
// scene. Zero after successful FSR reconstruction or an unjittered frame.
std::array<float,2> TemporalPresentationSampleOffset();
const char* NativeTemporalPresentationStatus(); // Atomic, safe for the UI thread.
void EndTemporalFrame(bool presented);
void RejectTemporalMotion(plume::RenderCommandList*);
void RecordTemporalHistoryResult(bool valid);
bool TemporalMotionSnapshotActive();
void SnapshotTemporalMotion(std::string_view label);
void ResetTemporalPresentation();
}
