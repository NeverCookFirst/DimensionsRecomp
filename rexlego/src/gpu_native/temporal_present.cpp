#include "gpu_native/native_recording.h"
#include "gpu_native/temporal_present.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/device.h"
#include <plume_d3d12.h>
#include <rex/logging.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <string>
namespace legodimensions::gpu_native {
struct PreparedTemporalSettings {
  std::shared_ptr<Fsr3Upscaler> upscaler;
  std::shared_ptr<plume::RenderTexture> motion,reactive,output;
  std::shared_ptr<plume::RenderTextureView> view;
  uint64_t generation = 0;
  uint32_t width = 0,height = 0,descriptor = ~0u;
  bool inverted = false,reuse_context = false,replace_resources = false,committed = false;
  ~PreparedTemporalSettings() { if (descriptor!=~0u) HostDevice::UnregisterTexture(descriptor); }
};
namespace {
struct Presentation {
  std::shared_ptr<Fsr3Upscaler> upscaler;
  std::shared_ptr<plume::RenderTexture> motion, reactive, output;
  std::shared_ptr<plume::RenderTextureView> output_view;
  std::shared_ptr<plume::RenderFramebuffer> framebuffer;
  plume::RenderTexture* attachment = nullptr;
  uint32_t descriptor = ~0u, width = 0, height = 0;
  uint64_t frame = ~uint64_t{0}, epoch = 1;
  uint64_t composed_frame = ~uint64_t{0};
  uint64_t reconstructed_frame = ~uint64_t{0};
  std::optional<TemporalCamera> camera, previous_camera;
  bool incompatible = false, inverted = false, jittered = false, motion_disabled = false;
  bool reported_dispatch = false;
  float jitter_x = 0, jitter_y = 0;
  std::array<float,2> presentation_offset{};
  uint32_t presents = 0, dispatched = 0, resets = 0;
  uint32_t missing_camera = 0, incompatible_frames = 0, missing_depth = 0;
  uint32_t extent_mismatch = 0, dispatch_failed = 0, valid_motion = 0, rejected_motion = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
Presentation g;
uint64_t temporal_settings_generation = 1;
std::atomic<uint32_t> presentation_status{0};
void ReportPresentationWindow() {
  if (++g.presents < 300) return;
  REXLOG_INFO("Native FSR 3 window: presents={} dispatched={} resets={} missing_camera={} incompatible={} missing_depth={} extent_mismatch={} dispatch_failed={} motion_valid={} motion_rejected={}",
      g.presents,g.dispatched,g.resets,g.missing_camera,g.incompatible_frames,
      g.missing_depth,g.extent_mismatch,g.dispatch_failed,g.valid_motion,g.rejected_motion);
  g.presents=g.dispatched=g.resets=g.missing_camera=g.incompatible_frames=0;
  g.missing_depth=g.extent_mismatch=g.dispatch_failed=g.valid_motion=g.rejected_motion=0;
}
std::shared_ptr<plume::RenderTexture> Texture(uint32_t w, uint32_t h,
    plume::RenderFormat format, bool storage) {
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w; desc.height = h; desc.depth = desc.mipLevels = desc.arraySize = 1;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.format = format; desc.committed = true;
  desc.flags = storage ? plume::RenderTextureFlag::UNORDERED_ACCESS : plume::RenderTextureFlag::RENDER_TARGET;
  auto texture=HostDevice::Device()->createTexture(desc);
  if (texture && !static_cast<plume::D3D12Texture*>(texture.get())->d3d) return {};
  return texture;
}
ID3D12Resource* Raw(plume::RenderTexture* texture) {
  return texture ? static_cast<plume::D3D12Texture*>(texture)->d3d : nullptr;
}
bool Create(uint32_t w, uint32_t h, bool inverted) {
  auto prepared=PrepareTemporalSettings(w,h,inverted);
  return prepared && CommitTemporalSettings(prepared);
}
}
std::shared_ptr<PreparedTemporalSettings> PrepareTemporalSettings(uint32_t w,uint32_t h,bool inverted) {
  auto* device = HostDevice::Device();
  if (!device || !w || !h) return {};
  auto prepared=std::make_shared<PreparedTemporalSettings>();
  prepared->generation=temporal_settings_generation;
  prepared->width=w;prepared->height=h;prepared->inverted=inverted;
  const bool complete=g.motion && g.reactive && g.output && g.output_view && g.descriptor!=~0u;
  if (g.upscaler && g.width==w && g.height==h && g.inverted==inverted && complete) {
    prepared->reuse_context=true;
    return prepared;
  }
  auto context = Fsr3Upscaler::Create(static_cast<plume::D3D12Device*>(device)->d3d,
      {w,h}, {1920,1080}, inverted);
  if (!context) return {};
  prepared->upscaler=std::move(context);
  if (g.width != w || g.height != h || !complete) {
    auto motion = Texture(w,h,plume::RenderFormat::R16G16_FLOAT,false);
    auto reactive = Texture(w,h,plume::RenderFormat::R8_UNORM,false);
    auto output = Texture(1920,1080,plume::RenderFormat::R16G16B16A16_FLOAT,true);
    auto view = output ? output->createTextureView(plume::RenderTextureViewDesc::Texture2D(
        plume::RenderFormat::R16G16B16A16_FLOAT)) : nullptr;
    if (!motion || !reactive || !output || !view) return {};
    // Stable debugger labels let full-frame captures identify the real FSR
    // inputs independently of resource IDs and alias allocations.
    Raw(motion.get())->SetName(L"LEGO FSR Input Motion");
    Raw(reactive.get())->SetName(L"LEGO FSR Input Reactive");
    Raw(output.get())->SetName(L"LEGO FSR Reconstructed Output");
    const auto descriptor = HostDevice::RegisterTexture(output.get(),view.get());
    if (descriptor == ~0u) return {};
    prepared->motion=std::move(motion);prepared->reactive=std::move(reactive);
    prepared->output=std::move(output);prepared->view=std::move(view);
    prepared->descriptor=descriptor;prepared->replace_resources=true;
  }
  return prepared;
}
bool CanCommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& prepared) {
  return prepared && !prepared->committed && prepared->generation==temporal_settings_generation &&
      temporal_settings_generation!=UINT64_MAX;
}
bool CommitTemporalSettings(const std::shared_ptr<PreparedTemporalSettings>& prepared,bool reset_history) {
  if (!CanCommitTemporalSettings(prepared)) return false;
  if (prepared->reuse_context) {
    if (reset_history) {
      g.upscaler->InvalidateHistory();g.previous_camera.reset();++g.epoch;
    }
    prepared->committed=true;++temporal_settings_generation;
    return true;
  }
  if (prepared->replace_resources) {
    if (g.descriptor != ~0u) HostDevice::UnregisterTexture(g.descriptor);
    HostDevice::RetireResource(std::move(g.framebuffer));
    HostDevice::RetireResource(std::move(g.motion));
    HostDevice::RetireResource(std::move(g.reactive));
    HostDevice::RetireResource(std::move(g.output_view));
    HostDevice::RetireResource(std::move(g.output));
    g.attachment = nullptr;
    g.motion = std::move(prepared->motion); g.reactive = std::move(prepared->reactive);
    g.output = std::move(prepared->output); g.output_view = std::move(prepared->view);
    g.descriptor = std::exchange(prepared->descriptor,~0u);
    g.width = prepared->width; g.height = prepared->height;
  }
  HostDevice::RetireResource(std::move(g.upscaler));
  g.upscaler = std::move(prepared->upscaler); g.inverted = prepared->inverted; ++g.epoch;
  g.previous_camera.reset();prepared->committed=true;++temporal_settings_generation;
  REXLOG_INFO("Native FSR 3: {}x{} -> 1920x1080, frame generation disabled", g.width,g.height);
  return true;
}
bool TemporalDepthInverted() {return g.inverted;}
bool PrepareTemporalDraw(plume::RenderCommandList* commands, uint64_t frame,
    uint32_t w, uint32_t h, const std::optional<TemporalCamera>& camera,
    float& x, float& y) {
  x = y = 0;
  if (TemporalSceneComposed(frame)) return false;
  if (!NativeFsr3Enabled() || !commands) return false;
  if (g.frame != frame) {
    g.frame = frame; g.camera.reset(); g.incompatible = false; g.jittered = false; g.motion_disabled = false;
    g.jitter_x = g.jitter_y = 0;
    if ((!g.upscaler || g.width != w || g.height != h) && !Create(w,h,camera && camera->inverted))
      return false;
    const plume::RenderTexture* attachments[]{g.motion.get(),g.reactive.get()};
    auto clear_fb = HostDevice::Device()->createFramebuffer(plume::RenderFramebufferDesc(attachments,2));
    if (!clear_fb) return false;
    const plume::RenderTextureBarrier barriers[]{
        {g.motion.get(),plume::RenderTextureLayout::COLOR_WRITE},
        {g.reactive.get(),plume::RenderTextureLayout::COLOR_WRITE}};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,barriers,2);
    commands->setFramebuffer(clear_fb.get());
    commands->clearColor(0,plume::RenderColor(0,0,0,0));
    commands->clearColor(1,plume::RenderColor(1,1,1,1));
    commands->setFramebuffer(nullptr);
    HostDevice::RetireResource(std::shared_ptr<plume::RenderFramebuffer>(std::move(clear_fb)));
    // Freeze jitter at the first scene draw. A camera discovered later cannot
    // introduce a different sample position between prepass and material pass.
    // Jitter belongs to the scene frame, not to the first shader's ability to
    // decode its camera. A later verified camera can still enable dispatch.
    g.jittered = g.upscaler->Jitter(frame,g.jitter_x,g.jitter_y);
  }
  if (g.width != w || g.height != h) { g.incompatible = true; return false; }
  if (camera) {
    if (g.previous_camera && (g.previous_camera->inverted != camera->inverted ||
        std::abs(g.previous_camera->vertical_fov-camera->vertical_fov) > 0.001f ||
        std::abs(g.previous_camera->near_plane-camera->near_plane) > 0.01f*camera->near_plane ||
        std::abs(g.previous_camera->far_plane-camera->far_plane) > 0.01f*camera->far_plane))
      g.upscaler->InvalidateHistory();
    if (g.camera && (g.camera->inverted != camera->inverted ||
        std::abs(g.camera->near_plane-camera->near_plane) > 0.01f*camera->near_plane ||
        std::abs(g.camera->far_plane-camera->far_plane) > 0.01f*camera->far_plane ||
        std::abs(g.camera->vertical_fov-camera->vertical_fov) > 0.001f)) {
      static uint32_t camera_conflicts = 0;
      if (camera_conflicts++ < 12) REXLOG_INFO(
          "Native FSR camera conflict frame={} near={}/{} far={}/{} fov={}/{} inverted={}/{}",
          frame,g.camera->near_plane,camera->near_plane,g.camera->far_plane,camera->far_plane,
          g.camera->vertical_fov,camera->vertical_fov,g.camera->inverted,camera->inverted);
      g.incompatible = true;
    }
    g.camera = camera;
  }
  x = g.jitter_x; y = g.jitter_y;
  return !g.incompatible && g.upscaler;
}
plume::RenderFramebuffer* TemporalFramebuffer(plume::RenderTexture* depth) {
  if (!g.motion || g.incompatible || g.motion_disabled || TemporalSceneComposed(g.frame)) return nullptr;
  if (!g.framebuffer || g.attachment != depth) {
    const plume::RenderTexture* attachments[]{g.motion.get(),g.reactive.get()};
    auto fb = HostDevice::Device()->createFramebuffer(plume::RenderFramebufferDesc(attachments,2,depth));
    if (!fb) { g.incompatible = true; return nullptr; }
    HostDevice::RetireResource(std::move(g.framebuffer));
    g.framebuffer = std::move(fb); g.attachment = depth;
  }
  return g.framebuffer.get();
}
TextureResourceView PresentTemporalFrame(TextureResourceView color,
    const TemporalSceneDepth& depth, uint64_t frame) {
  g.presentation_offset = {};
  // Warp and UI already consumed the reconstructed scene at native extent.
  // Their final color must pass through unchanged, without a second dispatch.
  if (TemporalSceneComposed(frame)) return color;
  presentation_status = NativeFsr3Enabled() ? 1u : 0u;
  if (!NativeFsr3Enabled()) return color;
  // A failed temporal dispatch must not expose the Halton camera movement.
  // The final spatial copy samples at the same offset used to rasterize.
  if (g.frame == frame && g.jittered && color.width == g.width &&
      color.height == g.height && g.width && g.height)
    g.presentation_offset = {g.jitter_x/g.width,g.jitter_y/g.height};
  if (!NativeFsr3Enabled() || !g.upscaler || g.frame != frame || g.incompatible ||
      !g.jittered || !g.camera || !depth.texture || depth.frame != frame ||
      depth.width != g.width || depth.height != g.height ||
      color.width != g.width || color.height != g.height) {
    if (g.upscaler) g.upscaler->InvalidateHistory();
    if (!g.jittered || !g.camera || g.frame != frame) ++g.missing_camera;
    if (g.incompatible) ++g.incompatible_frames;
    if (!depth.texture || depth.frame != frame) ++g.missing_depth;
    if (depth.width != g.width || depth.height != g.height ||
        color.width != g.width || color.height != g.height) ++g.extent_mismatch;
    return color;
  }
  if (g.inverted != g.camera->inverted && !Create(g.width,g.height,g.camera->inverted)) return color;
  HostDevice::SnapshotTexture(depth.texture.get(),"fsr-depth",false);
  HostDevice::SnapshotTexture(g.motion.get(),"fsr-motion",true);
  HostDevice::SnapshotTexture(g.reactive.get(),"fsr-reactive",true);
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) return color;
  const plume::RenderTextureBarrier inputs[]{
      {color.texture,plume::RenderTextureLayout::SHADER_READ},
      {depth.texture.get(),plume::RenderTextureLayout::SHADER_READ},
      {g.motion.get(),plume::RenderTextureLayout::SHADER_READ},
      {g.reactive.get(),plume::RenderTextureLayout::SHADER_READ},
      {g.output.get(),plume::RenderTextureLayout::GENERAL}};
  commands->setFramebuffer(nullptr);
  commands->barriers(plume::RenderBarrierStage::COMPUTE,inputs,5);
  auto now = std::chrono::steady_clock::now();
  Fsr3FrameInputs in;
  in.color = Raw(color.texture); in.depth = Raw(depth.texture.get());
  in.motion = Raw(g.motion.get()); in.reactive = Raw(g.reactive.get()); in.output = Raw(g.output.get());
  in.frame = frame; in.scene_epoch = g.epoch; in.jitter_x = g.jitter_x; in.jitter_y = g.jitter_y;
  in.frame_time_ms = std::clamp(std::chrono::duration<float,std::milli>(now-g.last).count(),0.1f,1000.0f);
  in.camera_near = g.camera->near_plane; in.camera_far = g.camera->far_plane;
  in.camera_fov = g.camera->vertical_fov;
  in.sharpen = NativeFsr3Sharpening(); in.sharpness = NativeFsr3Sharpness();
  auto* native = static_cast<plume::D3D12CommandList*>(NativeImmediateCommands(commands));
  const bool ok = g.upscaler->Dispatch(native->d3d,in);
  native->notifyDescriptorHeapWasChangedExternally();
  native->notifyCommandStateWasChangedExternally();
  g.last = now;
  if (!ok) { ++g.dispatch_failed; g.upscaler->InvalidateHistory(); return color; }
  ++g.dispatched;
  g.reconstructed_frame=frame;
  presentation_status = 2;
  g.presentation_offset = {};
  if (g.upscaler->LastDispatchResetHistory()) ++g.resets;
  if (!g.reported_dispatch) {
    REXLOG_INFO("Native FSR 3: presenting upscaled scene with independent depth/motion/reactive inputs; sharpening={} strength={}",in.sharpen,in.sharpness);
    g.reported_dispatch = true;
  }
  color.texture = g.output.get(); color.view = g.output_view.get();
  color.descriptor_index = g.descriptor; color.width = 1920; color.height = 1080;
  color.format = plume::RenderFormat::R16G16B16A16_FLOAT;
  return color;
}
bool CompleteTemporalComposition(uint64_t frame) {
  if(g.frame!=frame || g.reconstructed_frame!=frame || presentation_status.load()!=2) return false;
  g.composed_frame=frame;g.presentation_offset={};return true;
}
bool TemporalSceneComposed(uint64_t frame) {
  return NativeFsr3Enabled() && g.composed_frame!=~uint64_t{0} && g.composed_frame==frame;
}
void FailTemporalComposition(uint64_t frame) {
  if(g.frame!=frame) return;
  if(g.composed_frame==frame) g.composed_frame=~uint64_t{0};
  g.incompatible=true;
  if(g.upscaler) g.upscaler->InvalidateHistory();
}
std::array<float,2> TemporalPresentationSampleOffset() {return g.presentation_offset;}
const char* NativeTemporalPresentationStatus() {
  switch(presentation_status.load()) {
    case 1: return "Spatial fallback (temporal inputs unavailable)";
    case 2: return "FSR 3 temporal reconstruction";
    default: return "Native rendering";
  }
}
void EndTemporalFrame(bool presented) {
  if(NativeFsr3Enabled()) ReportPresentationWindow();
  if (!presented && g.upscaler) g.upscaler->InvalidateHistory();
  g.previous_camera = presented && !g.incompatible ? g.camera : std::nullopt;
  // The framebuffer references guest-owned depth; retire before that surface
  // can be destroyed or replaced, even when a new allocation reuses its address.
  HostDevice::RetireResource(std::move(g.framebuffer)); g.attachment = nullptr;
}
void RecordTemporalHistoryResult(bool valid) {
  if (valid) ++g.valid_motion;
  else ++g.rejected_motion;
}
bool TemporalMotionSnapshotActive() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_MOTION_STAGE_SNAPSHOTS");
    return value && *value && value[0]!='0';
  }();
  return enabled && HostDevice::SnapshotFrameActive();
}
void SnapshotTemporalMotion(std::string_view label) {
  if (!g.motion || !g.reactive) return;
  HostDevice::SnapshotTexture(g.motion.get(),std::string("fsr-motion-")+std::string(label),true);
  HostDevice::SnapshotTexture(g.reactive.get(),std::string("fsr-reactive-")+std::string(label),true);
}
void RejectTemporalMotion(plume::RenderCommandList* commands) {
  if(TemporalSceneComposed(g.frame)) return;
  if (!g.reactive || !commands) return;
  const plume::RenderTexture* attachments[]{g.motion.get(),g.reactive.get()};
  auto fb = HostDevice::Device()->createFramebuffer(plume::RenderFramebufferDesc(attachments,2));
  if (!fb) {g.incompatible = true;return;}
  commands->setFramebuffer(fb.get());
  commands->clearColor(0,plume::RenderColor(0,0,0,0));
  commands->clearColor(1,plume::RenderColor(1,1,1,1));
  commands->setFramebuffer(nullptr);
  HostDevice::RetireResource(std::shared_ptr<plume::RenderFramebuffer>(std::move(fb)));
  g.motion_disabled = true;
}
void ResetTemporalPresentation() {
  // Called by HostDevice::Shutdown under its mutex after all GPU fences pass.
  // The enclosing device destroys the entire descriptor registry next.
  g = {};
  if(temporal_settings_generation!=UINT64_MAX) ++temporal_settings_generation;
  presentation_status = 0;
}
}
